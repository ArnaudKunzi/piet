// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Extended-missing .dta round-trip — all 27 Stata missing values (`.`, `.a`..`.z`)
// survive a write followed by a read, in every numeric storage type the writer
// can narrow to.
//
// Why this test exists (P6 rider, 2026-08-05): the READ side learned the 27 kinds
// at @96caa73, but `emit_cell` still tested a bare `std::isnan(v)` and wrote the
// system-missing code for every kind. So a file containing `.a` loaded correctly,
// behaved correctly in-session, and then lost the letter on `save FILE.dta` — no
// warning, no reject. Silent value change, i.e. the cardinal defect class.
//
// The RED arm is the pre-fix binary: run this against a build of dta_writer.cpp
// from before the fix and every kind >= 1 reads back as `.` (kind 0), so each of
// the 26 extended codes fails in each of the 5 storage types.
//
// The oracle for the encoding is dta_reader.cpp's missing_code_raw(), which was
// itself confirmed against a real StataBE 19.5 file. This test does NOT re-derive
// the bit patterns from prose — it asserts the writer and the reader agree, and
// pins ONE externally-verified pattern (`.a` as a double = 0x7FE0010000000000)
// so the pair cannot drift together into a self-consistent wrong encoding.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "data/frame.h"
#include "data/missing.h"
#include "io/win_compat.h"  // native_temp_path: /tmp on POSIX, %TEMP% on Windows
#include "io/dta_reader.h"
#include "io/dta_writer.h"

using namespace cecon;

static int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
}

// A column of values whose FINITE part forces the narrowing scan to pick a
// specific storage type, with all 27 missing kinds appended.
static std::vector<double> make_col(double finite_lo, double finite_hi) {
    std::vector<double> v{finite_lo, finite_hi};
    for (int k = kMissSys; k <= kMissZ; ++k) v.push_back(stata_missing(k));
    return v;
}

// Every kind must come back as itself.
static void check_kinds(const Frame& f, const std::string& tag) {
    const auto& data = f.col(tag).f64;
    check(data.size() == 29, tag + ": 29 rows");
    if (data.size() != 29) return;
    for (int k = kMissSys; k <= kMissZ; ++k) {
        const double got = data[static_cast<size_t>(2 + k)];
        const int gotKind = missing_kind(got);
        check(gotKind == k, tag + ": row for kind " + std::to_string(k) + " read back as kind " +
                                std::to_string(gotKind) + " (expected " + std::to_string(k) +
                                ", i.e. `" + missing_text(k) + "`)");
    }
}

int test_dta_missing_roundtrip_main() {
    const std::string path = cecon::native_temp_path("cecon_missing_roundtrip.dta");

    Frame fr;
    fr.set_n_rows_hint(29);  // 2 finite + 27 missing kinds
    // Ranges chosen to land the narrowing ladder on each storage type in turn.
    fr.append_f64_column("as_byte", make_col(-100.0, 100.0));
    fr.append_f64_column("as_int", make_col(-30000.0, 32000.0));
    fr.append_f64_column("as_long", make_col(-2000000000.0, 2000000000.0));
    fr.append_f64_column("as_float", make_col(0.5, 1.5));           // exact in f32
    fr.append_f64_column("as_double", make_col(1e300, 1.0 / 3.0));  // needs f64
    fr.finalize_after_bulk_load();

    DtaWriteOptions wo;
    write_dta(fr, path, wo);

    DtaResult r = read_dta(path);
    const Frame& back = r.frame;
    check(back.n_cols() == 5, "5 columns survive");
    if (back.n_cols() != 5) { std::remove(path.c_str()); return fail; }

    for (const char* n : {"as_byte", "as_int", "as_long", "as_float", "as_double"}) {
        check_kinds(back, n);
    }

    // The finite values must be untouched by the kind-carrying change.
    check(back.col("as_byte").f64[0] == -100.0 && back.col("as_byte").f64[1] == 100.0,
          "byte finite values intact");
    check(back.col("as_long").f64[0] == -2000000000.0, "long finite value intact");
    check(std::fabs(back.col("as_double").f64[1] - 1.0 / 3.0) < 1e-15, "double finite value intact");

    // External anchor: `.a` in a double column is 0x7FE0010000000000 in a real
    // StataBE 19.5 file (P1 fixture, verified 2026-08-04). Read the raw bytes of
    // the file we just wrote and confirm we emit that exact pattern — this is what
    // stops the writer and reader from agreeing on an encoding Stata does not use.
    {
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        check(fp != nullptr, "reopen written file for byte check");
        if (fp) {
            std::fseek(fp, 0, SEEK_END);
            long n = std::ftell(fp);
            std::fseek(fp, 0, SEEK_SET);
            std::vector<uint8_t> buf(static_cast<size_t>(n));
            size_t got = std::fread(buf.data(), 1, buf.size(), fp);
            std::fclose(fp);
            check(got == buf.size(), "read whole file");
            const uint64_t want = 0x7FE0010000000000ull;
            uint8_t le[8];
            for (int i = 0; i < 8; ++i) le[i] = static_cast<uint8_t>((want >> (8 * i)) & 0xFF);
            bool found = false;
            for (size_t i = 0; i + 8 <= buf.size() && !found; ++i) {
                found = std::memcmp(buf.data() + i, le, 8) == 0;
            }
            check(found, "written file contains the real-Stata `.a` double pattern "
                         "0x7FE0010000000000 (little-endian)");
        }
    }

    // BUG-272 (closes BUG-200): the writer emits the Frame's sort claim into
    // <sortlist> and the reader carries it back — through real bytes, both
    // directions. Includes the adversarial half: after a value write on a key
    // column the claim (and hence the emitted <sortlist>) must be GONE, because a
    // stale claim would be a false "Sorted by:" in real Stata.
    {
        const std::string sp = cecon::native_temp_path("cecon_sortclaim_roundtrip.dta");
        Frame sf;
        sf.set_n_rows_hint(4);
        sf.append_f64_column("a", {1, 1, 2, 2});
        sf.append_f64_column("b", {1, 2, 1, 2});
        sf.finalize_after_bulk_load();
        Frame sorted = sf.gsort({"a", "b"}, {true, true});
        write_dta(sorted, sp, DtaWriteOptions{});
        DtaResult rt = read_dta(sp);
        check((rt.frame.sorted_by() == std::vector<std::string>{"a", "b"}),
              "sort claim round-trips through .dta bytes (BUG-272)");
        // claim-breaking transform: overwrite key `a`, re-export, re-import → blank
        Frame broken = std::move(rt.frame);
        broken.generate("a", "4 - a");
        check(broken.sorted_by().empty(), "value write on key clears the claim");
        write_dta(broken, sp, DtaWriteOptions{});
        check(read_dta(sp).frame.sorted_by().empty(),
              "re-export after the break carries NO claim (never a stale lie)");
        std::remove(sp.c_str());
    }

    // Native save now emits genuine DTA tempfiles; long strings and embedded
    // binary NULs must therefore survive the actual persistence plane.
    {
        const std::string lp = cecon::native_temp_path("cecon_strl_roundtrip.dta");
        const std::vector<std::string> values = {"", std::string(2045, 'a'),
            std::string(2046, 'b'), std::string(2100, 'c'), std::string("a\0b", 3),
            std::string("\0ab", 3), std::string("ab\0", 3), std::string(2046, 'b')};
        Frame strings;
        strings.set_n_rows_hint(values.size());
        strings.append_factor_column("longtext", values);
        strings.append_factor_column("shortlong", std::vector<std::string>(values.size(), "abc"));
        strings.finalize_after_bulk_load();
        for (int release : {117, 118}) {
            DtaWriteOptions options;
            options.release = release;
            DtaWriteVarMeta declared;
            declared.name = "shortlong";
            declared.stata_type = "strL";
            options.var_meta.push_back(declared);
            write_dta(strings, lp, options);
            // Independent byte anchor from installed [P] dta §5.11: the
            // first nonempty cell is (v=2,o=1), ASCII 130, length 4, abc\0.
            std::FILE* input = std::fopen(lp.c_str(), "rb");
            check(input != nullptr, "strL byte anchor opens the file");
            if (input) {
                std::fseek(input, 0, SEEK_END);
                const long size = std::ftell(input);
                std::rewind(input);
                std::string bytes(static_cast<size_t>(size), '\0');
                const size_t got = std::fread(bytes.data(), 1, bytes.size(), input);
                std::fclose(input);
                const auto section = bytes.find("<strls>");
                std::string expected = "GSO";
                expected.append("\2\0\0\0", 4);
                expected.append("\1\0\0\0\0\0\0\0", release == 117 ? 4 : 8);
                expected.push_back(static_cast<char>(130));
                expected.append("\4\0\0\0abc\0", 8);
                check(got == bytes.size() && section != std::string::npos &&
                          bytes.compare(section + 7, expected.size(), expected) == 0,
                      "strL GSO header, byte order, length and text terminator match the specification");
            }
            const DtaResult rt = read_dta(lp);
            check(rt.var_meta[0].stata_type == "strL" && rt.var_meta[1].stata_type == "strL",
                  "strL physical type survives, including short declared strL");
            const auto& column = rt.frame.col("longtext");
            for (size_t i = 0; i < values.size(); ++i) {
                const int32_t code = column.codes[i];
                const std::string got = code < 0 ? "" : column.levels[static_cast<size_t>(code)];
                check(got == values[i], "strL length/raw bytes round-trip, row " + std::to_string(i));
            }
        }
        std::remove(lp.c_str());
    }

    std::remove(path.c_str());
    std::printf(fail ? "test_dta_missing_roundtrip: FAIL\n" : "test_dta_missing_roundtrip: PASS\n");
    return fail;
}
