// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data/datasignature.h"
#include "data/missing.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace cecon {
namespace {

// FNV-1a-64, run as a THREE-accumulator streaming mix over structured content
// (identity plane + two differently-seeded value planes), folded to 32 bits.
//
// Relationship to the one other FNV in the tree, stated so nobody "de-duplicates"
// them into a bug: apps/cmds/common.cpp's `hash_bytes_hex` is the provenance
// mapping — a whole-buffer bytes→16-hex-digit hash whose output is PINNED in
// goldens (`provenance.fnv1a64`). This is a different function with a different
// contract (incremental, multi-seed, structure-aware, 32-bit decimal output) and
// must never be substituted for it, nor it for this. Only the standard kernel
// constants are shared.
constexpr uint64_t kPrime = 1099511628211ULL;
constexpr uint64_t kBasis = 1469598103934665603ULL;   // canonical FNV-1a-64 offset basis
constexpr uint64_t kBasisB = 0x9ae16a3b2f90404fULL;   // second, independent seed for the value pair

// Structural separators, chosen from the ASCII unit/record-separator pair so they
// cannot occur inside a factor level that came from a text or .dta source.
constexpr unsigned char kFieldSep = 0x1f;
constexpr unsigned char kColSep = 0x1e;
constexpr unsigned char kMissing = 0x00;

struct Fnv {
    uint64_t h;
    explicit Fnv(uint64_t basis) : h(basis) {}

    void byte(unsigned char c) {
        h ^= c;
        h *= kPrime;
    }
    void bytes(const void* p, std::size_t n) {
        const unsigned char* q = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) byte(q[i]);
    }
    void str(const std::string& s) {
        bytes(s.data(), s.size());
        byte(kFieldSep);
    }
    // Counts ride the hash as well as the visible fields: a file truncated to
    // whole rows must not be able to keep its checksums.
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(static_cast<unsigned char>((v >> (i * 8)) & 0xff));
    }
    // A stored double, byte-order-independently (LSB-first regardless of host
    // endianness) so the same data signs identically on any platform.
    //   · every NaN collapses to ONE missing token — NaN is the engine's missing
    //     representation and no payload bit may be allowed to perturb a signature
    //     (a reader that starts distinguishing extended missing would have to
    //     revisit this line deliberately, which is the point of naming it; the
    //     order-free plane, BUG-1461, adds the kind after this token);
    //   · -0.0 is canonicalized to +0.0 — the two compare equal, so a sign bit
    //     must not fork the fingerprint of equal data.
    void real(double v) {
        if (std::isnan(v)) {
            byte(kMissing);
            return;
        }
        if (v == 0.0) v = 0.0;
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u64(bits);
    }
    uint32_t fold() const {
        return static_cast<uint32_t>((h >> 32) ^ (h & 0xffffffffULL));
    }
};

// The value plane of one column (stored values in row order) into both value
// accumulators; shared by data_signature and data_signature_vars.
void sign_values(const FrameColumn& c, int64_t nrows, Fnv& va, Fnv& vb) {
    // ── value plane: the STORED values, in column-then-row order. A factor
    // hashes its LEVEL STRING, not its code, so a dictionary that was rebuilt
    // in a different order over identical data still signs its values the same
    // (that reordering shows up in `meta`, where identity changes belong).
    va.byte(c.is_factor ? 'S' : 'F');
    vb.byte(c.is_factor ? 'S' : 'F');
    if (c.is_factor) {
        const LevelCol& lv = c.levels;
        const CodeCol& cd = c.codes;
        const std::size_t nl = lv.size();
        for (int64_t i = 0; i < nrows; ++i) {
            const int32_t code =
                (static_cast<std::size_t>(i) < cd.size()) ? cd[static_cast<std::size_t>(i)] : -1;
            if (code < 0 || static_cast<std::size_t>(code) >= nl) {
                va.byte(kMissing);
                vb.byte(kMissing);
            } else {
                const std::string& s = lv[static_cast<std::size_t>(code)];
                va.str(s);
                vb.str(s);
            }
        }
    } else {
        // LADDER: read through the narrow-aware reader — the values are identical
        // by the narrowing predicate (float-exact), so the hash is identical, and
        // the signature must not widen the frame it is fingerprinting.
        const F64Reader f = c.f64.reader();
        const int64_t fn = static_cast<int64_t>(c.f64.size());
        for (int64_t i = 0; i < nrows; ++i) {
            const double v = (i < fn)
                                 ? f[static_cast<std::size_t>(i)]
                                 : std::numeric_limits<double>::quiet_NaN();
            va.real(v);
            vb.real(v);
        }
    }
}

// BUG-1461: the order-free value plane. Each row's value is hashed on its own
// (both seeds, then mixed so the sum is not linear in the value bytes) and the
// column keeps the 64-bit SUM of those hashes, which no permutation of rows can
// move; an edited value still moves it.
uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void sign_values_unordered(const FrameColumn& c, int64_t nrows, Fnv& va, Fnv& vb) {
    va.byte(c.is_factor ? 'S' : 'F');
    vb.byte(c.is_factor ? 'S' : 'F');
    uint64_t sumA = 0, sumB = 0;
    auto add = [&](const auto& feed) {
        Fnv ra(kBasis), rb(kBasisB);
        feed(ra);
        feed(rb);
        sumA += mix64(ra.h);
        sumB += mix64(rb.h);
    };
    if (c.is_factor) {
        const LevelCol& lv = c.levels;
        const CodeCol& cd = c.codes;
        const std::size_t nl = lv.size();
        for (int64_t i = 0; i < nrows; ++i) {
            const int32_t code =
                (static_cast<std::size_t>(i) < cd.size()) ? cd[static_cast<std::size_t>(i)] : -1;
            if (code < 0 || static_cast<std::size_t>(code) >= nl)
                add([](Fnv& f) { f.byte(kMissing); });
            else
                add([&](Fnv& f) { f.str(lv[static_cast<std::size_t>(code)]); });
        }
    } else {
        const F64Reader f = c.f64.reader();
        const int64_t fn = static_cast<int64_t>(c.f64.size());
        for (int64_t i = 0; i < nrows; ++i) {
            const double v = (i < fn)
                                 ? f[static_cast<std::size_t>(i)]
                                 : std::numeric_limits<double>::quiet_NaN();
            // Stata's confirm sees a `.` <-> `.a` edit, so here (unlike the
            // ordered plane above) an extended missing's kind is signed too.
            const int kind = missing_kind(v);
            add([v, kind](Fnv& h) {
                h.real(v);
                if (kind > 0) h.byte(static_cast<unsigned char>(kind));
            });
        }
    }
    va.u64(sumA);
    vb.u64(sumB);
}

// One column's value plane under `opts`: its storage type first, when known.
void sign_column(const FrameColumn& c, int64_t nrows, const SignatureOptions& opts, Fnv& va,
                 Fnv& vb) {
    const auto t = opts.types.find(c.name);
    if (t != opts.types.end()) {
        va.str(t->second);
        vb.str(t->second);
    }
    if (opts.unordered)
        sign_values_unordered(c, nrows, va, vb);
    else
        sign_values(c, nrows, va, vb);
}

}  // namespace

std::string data_signature(const Frame& fr, const SignatureOptions& opts) {
    Fnv meta(kBasis), va(kBasis), vb(kBasisB);

    const int64_t nrows = fr.n_rows();
    const int ncols = fr.n_cols();
    meta.u64(static_cast<uint64_t>(nrows));
    meta.u64(static_cast<uint64_t>(ncols));
    va.u64(static_cast<uint64_t>(nrows));
    vb.u64(static_cast<uint64_t>(nrows));

    for (const auto& c : fr.columns()) {
        // ── identity plane: name, position (implicit in traversal order), kind,
        // and, for a factor, its dictionary. `c.format` is DELIBERATELY absent:
        // Stata's own signature was byte-identical across a %tw→%td edit
        // (golden/datasig_oracle.log), and P1-A0 carriage forbids any data-layer
        // decision from reading that display string.
        meta.str(c.name);
        meta.byte(c.is_factor ? 'S' : 'F');
        meta.byte(kFieldSep);
        if (c.is_factor) {
            const LevelCol& lv = c.levels;
            meta.u64(static_cast<uint64_t>(lv.size()));
            for (std::size_t k = 0; k < lv.size(); ++k) meta.str(lv[k]);
        }
        meta.byte(kColSep);

        sign_column(c, nrows, opts, va, vb);
        va.byte(kColSep);
        vb.byte(kColSep);
    }

    char buf[96];
    std::snprintf(buf, sizeof buf, "%lld:%d(%u):%u:%u", static_cast<long long>(nrows), ncols,
                  meta.fold(), va.fold(), vb.fold());
    return buf;
}

// Stata `_datasignature varlist, esample nodefault nonames`'s SHAPE over a chosen
// variable list (repeats allowed, as signestimationsample passes `y x1 x2 id id
// id2` after areg) and the frame's rows as given (the caller restricts them to
// the estimation sample): `<rows>:<k>:<vals-a>:<vals-b>`, no identity term
// (nonames). Same Q9 ruling as data_signature: deterministic, NOT Stata-exact.
std::string data_signature_vars(const Frame& fr, const std::vector<std::string>& vars,
                                const SignatureOptions& opts) {
    Fnv va(kBasis), vb(kBasisB);
    const int64_t nrows = fr.n_rows();
    va.u64(static_cast<uint64_t>(nrows));
    vb.u64(static_cast<uint64_t>(nrows));
    for (const auto& v : vars) {
        sign_column(fr.col(v), nrows, opts, va, vb);
        va.byte(kColSep);
        vb.byte(kColSep);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%lld:%d:%u:%u", static_cast<long long>(nrows),
                  static_cast<int>(vars.size()), va.fold(), vb.fold());
    return buf;
}

}  // namespace cecon
