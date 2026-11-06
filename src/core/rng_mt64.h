// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from MT19937-64 2004/9/29 (BSD-3-Clause).
// See THIRD_PARTY_NOTICES.
//
// cecon — Stata's session random-number generator, draw for draw.
//
// Stata's default RNG (`set rng default`, c(rng_current) = "mt64") is the
// 64-bit Mersenne Twister of Nishimura & Matsumoto, the reference mt19937-64.c
// ([FN] Random-number functions: "runiform() uses the mt64 RNG by default ... The
// mt64 RNG implements the 64-bit version"). Identified bit for bit against the
// oracle machine's StataBE 19.5 (internal remaining-150 g13/g03 probes):
//   set seed s        init_genrand64(s): mt[0] = s, mt[i] = 6364136223846793005 *
//                     (mt[i-1] ^ (mt[i-1] >> 62)) + i. c(rngstate) after `set seed 1`
//                     carries mt[1] = 5851f42d4c957f2e.
//   runiform()        genrand64_real3: ((x >> 12) + 0.5) * 2^-52 -- strictly inside
//                     (0,1). The first 5 draws after `set seed 1` match in %21x;
//                     the (x>>11)*2^-53 and real1 variants do not.
//   uniform()         the same stream and transform as runiform().
//   runiformint(a,b)  v = x >> (64 - k), k = bit length of b-a, redrawn while
//                     v > b-a (60/60 seed x range cases, including how many raw
//                     draws each call consumes).
//   lasso CV folds    one runiform() per estimation-sample row (handlers_e3_lasso).
//   boottest          Rademacher v = runiform(G, B+1) >= .5, filled row by row
//                     (boottest.mata:1505-1507) after the ado's `set seed`.
//
// ONE stream per PIET session (SessionState::seed.stream()). Every consumer draws
// from it in Stata's order -- observation order, `if`/`in` rows only, left to
// right within a row -- and advances it by the draws it took, so a do-file's
// runiform() lines, bsample, boottest and the lasso folds see the numbers Stata
// would have handed them. The engine is a subprocess: the state rides the argv
// as the c(rngstate) string (`--rng-mt64`) and the engine reports how many raw
// draws it consumed (`rng_draws`), which the session then skips over.
//
// This header is the small API other workstreams call:
//   Mt64 g(seed);              // == `set seed seed`
//   g.next()                   // one raw 64-bit draw (genrand64_int64)
//   g.runiform()               // Stata runiform()
//   g.runiformint(a, b)        // Stata runiformint(a,b)
//   g.discard(n)               // fast-forward n raw draws
//   g.state_string()           // c(rngstate), 5011 characters
//   g.restore(text)            // `set rngstate text`
//   g.words(), g.index()       // the 312 state words and the next-word index
//   g.taken()                  // raw draws since the last seed()/restore()
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace cecon {

class Mt64 {
 public:
    static constexpr int kN = 312;
    static constexpr int kM = 156;
    // Stata's start-up seed ([R] set seed: "The default seed is 123456789").
    static constexpr uint64_t kDefaultSeed = 123456789ull;
    // c(rngstate): "XAA" + 312 x 16 hex + "0001" + "0000" + %04x index + 4-hex tail.
    static constexpr size_t kStateStringLength = 3 + 16 * kN + 16;

    Mt64() { seed(kDefaultSeed); }
    explicit Mt64(uint64_t s) { seed(s); }

    // init_genrand64.
    void seed(uint64_t s) {
        mt_[0] = s;
        for (int i = 1; i < kN; ++i)
            mt_[i] = 6364136223846793005ull * (mt_[i - 1] ^ (mt_[i - 1] >> 62)) +
                     static_cast<uint64_t>(i);
        mti_ = kN;
        taken_ = 0;
    }

    // genrand64_int64.
    uint64_t next() {
        if (mti_ >= kN) regenerate();
        ++taken_;
        uint64_t x = mt_[mti_++];
        x ^= (x >> 29) & 0x5555555555555555ull;
        x ^= (x << 17) & 0x71D67FFFEDA60000ull;
        x ^= (x << 37) & 0xFFF7EEE000000000ull;
        x ^= (x >> 43);
        return x;
    }

    void discard(uint64_t n) {
        taken_ += n;
        while (n > 0) {
            if (mti_ >= kN) regenerate();
            const uint64_t room = static_cast<uint64_t>(kN - mti_);
            const uint64_t take = n < room ? n : room;
            mti_ += static_cast<int>(take);
            n -= take;
        }
    }

    // runiform(): genrand64_real3.
    double runiform() {
        return (static_cast<double>(next() >> 12) + 0.5) * (1.0 / 4503599627370496.0);
    }

    // runiformint(a,b) for whole a <= b (the caller answers missing otherwise).
    // A degenerate range answers a WITHOUT drawing: after `set seed 5`,
    // runiformint(7,7) then runiform() gives the first runiform() of the stream
    // (StataBE probe, impl_C/stata_probe P3). runiform(a,b) with a >= b likewise
    // answers missing without drawing (P1), which is the caller's branch.
    double runiformint(double a, double b) {
        const uint64_t span = static_cast<uint64_t>(b - a);  // b - a, exact below 2^53
        if (span == 0) return a;
        int k = 0;
        while (k < 64 && (span >> k) != 0) ++k;
        for (;;) {
            const uint64_t v = next() >> (64 - k);
            if (v <= span) return a + static_cast<double>(v);
        }
    }

    // runiform(a,b) for a < b: a + (b-a)*u (P2: runiform(2,5) after `set seed 5`
    // equals 2 + 3*runiform() in %21x).
    double runiform(double a, double b) { return a + (b - a) * runiform(); }

    const uint64_t* words() const { return mt_; }
    int index() const { return mti_; }
    uint64_t taken() const { return taken_; }

    // c(rngstate) in Stata's layout. The last four hex digits are a check value
    // whose rule is not identified (internal g03 probe4: neither 16-bit sums, xor,
    // CRC-CCITT nor CRC32/Adler halves); this writes a 16-bit xor-fold of the
    // words in its place, and restore() ignores the field, so a string Stata
    // printed is accepted and PIET's own strings round-trip.
    std::string state_string() const {
        std::string s;
        s.reserve(kStateStringLength);
        s += "XAA";
        char buf[20];
        uint64_t fold = 0;
        for (int i = 0; i < kN; ++i) {
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(mt_[i]));
            s += buf;
            fold ^= mt_[i];
        }
        fold ^= static_cast<uint64_t>(mti_);
        const unsigned check = static_cast<unsigned>((fold ^ (fold >> 16) ^ (fold >> 32) ^
                                                      (fold >> 48)) & 0xFFFFu);
        std::snprintf(buf, sizeof buf, "00010000%04x%04x", static_cast<unsigned>(mti_), check);
        s += buf;
        return s;
    }

    // `set rngstate`: true on a well-formed mt64 state string (the state is
    // left untouched otherwise).
    bool restore(const std::string& s) {
        if (s.size() != kStateStringLength || s.compare(0, 3, "XAA") != 0) return false;
        uint64_t w[kN];
        for (int i = 0; i < kN; ++i) {
            uint64_t v = 0;
            if (!hex(s, 3 + static_cast<size_t>(i) * 16, 16, &v)) return false;
            w[i] = v;
        }
        uint64_t idx = 0;
        const size_t tail = 3 + static_cast<size_t>(kN) * 16;
        if (!hex(s, tail + 8, 4, &idx) || idx > static_cast<uint64_t>(kN)) return false;
        for (int i = 0; i < kN; ++i) mt_[i] = w[i];
        mti_ = static_cast<int>(idx);
        taken_ = 0;
        return true;
    }

 private:
    void regenerate() {
        static constexpr uint64_t kMatrixA = 0xB5026F5AA96619E9ull;
        static constexpr uint64_t kUpper = 0xFFFFFFFF80000000ull;
        static constexpr uint64_t kLower = 0x7FFFFFFFull;
        int i = 0;
        for (; i < kN - kM; ++i) {
            const uint64_t x = (mt_[i] & kUpper) | (mt_[i + 1] & kLower);
            mt_[i] = mt_[i + kM] ^ (x >> 1) ^ ((x & 1ull) ? kMatrixA : 0ull);
        }
        for (; i < kN - 1; ++i) {
            const uint64_t x = (mt_[i] & kUpper) | (mt_[i + 1] & kLower);
            mt_[i] = mt_[i + (kM - kN)] ^ (x >> 1) ^ ((x & 1ull) ? kMatrixA : 0ull);
        }
        const uint64_t x = (mt_[kN - 1] & kUpper) | (mt_[0] & kLower);
        mt_[kN - 1] = mt_[kM - 1] ^ (x >> 1) ^ ((x & 1ull) ? kMatrixA : 0ull);
        mti_ = 0;
    }

    static bool hex(const std::string& s, size_t at, size_t len, uint64_t* out) {
        uint64_t v = 0;
        for (size_t i = at; i < at + len; ++i) {
            const char c = s[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = (v << 4) | static_cast<uint64_t>(d);
        }
        *out = v;
        return true;
    }

    uint64_t mt_[kN];
    int mti_ = kN;
    uint64_t taken_ = 0;
};

}  // namespace cecon
