// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Frame::from_csv: column type inference (all-numeric -> f64, else factor),
// missing-value handling (empty / "." / "NA" -> NaN), RFC-4180 quoting (quoted
// comma + "" escape), and the .hbin write/read roundtrip.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "data/frame.h"
#include "io/columnar.h"
#include "io/win_compat.h"

using namespace cecon;

static int g_fail = 0;
static void check(const char* what, bool ok) {
    if (!ok) { std::printf("  FAIL %s\n", what); ++g_fail; }
}

int test_csv_main() {
    std::printf("test_csv:\n");
    // Non-ASCII (Hangul) leaf on purpose: on a KR Windows box a UTF-8 path taken
    // through a narrow ANSI API is decoded as CP949 and the real UTF-16 file is
    // never found (BUG-050). native_temp_path + to_native_path make this the
    // regression gate for that; on POSIX u8path copies the bytes verbatim, so the
    // path is just an ordinary UTF-8 name under /tmp and behaviour is unchanged.
    const std::string csv = native_temp_path(u8"cobra_한글_from_csv.csv");
    const std::string hbin = native_temp_path(u8"cobra_한글_from_csv.hbin");
    {
        // binary: the CSV bytes below are written verbatim on every platform
        // (text mode would expand \n to \r\n on Windows). No-op on POSIX.
        std::ofstream f(to_native_path(csv), std::ios::binary);
        f << "id,val,grp,note\n";
        f << "1,3.5,a,\"hello, world\"\n";   // quoted comma in note
        f << "2,.,b,x\n";                     // Stata missing "."
        f << "3,NA,a,y\n";                    // NA missing
        f << "4,7,b,\"quote \"\"q\"\"\"\n";   // "" -> literal quote
    }

    Frame fr = Frame::from_csv(csv, ',', {});
    check("n_rows == 4", fr.n_rows() == 4);
    check("n_cols == 4", fr.n_cols() == 4);
    check("has id/val/grp/note", fr.has("id") && fr.has("val") && fr.has("grp") && fr.has("note"));

    const FrameColumn& id = fr.col("id");
    const FrameColumn& val = fr.col("val");
    const FrameColumn& grp = fr.col("grp");
    const FrameColumn& note = fr.col("note");

    check("id is f64", !id.is_factor);
    check("val is f64", !val.is_factor);
    check("grp is factor", grp.is_factor);
    check("note is factor", note.is_factor);

    check("id values", id.f64[0] == 1 && id.f64[3] == 4);
    check("val[0]==3.5", val.f64[0] == 3.5);
    check("val[1] NaN (.)", std::isnan(val.f64[1]));
    check("val[2] NaN (NA)", std::isnan(val.f64[2]));
    check("val[3]==7", val.f64[3] == 7.0);

    check("grp levels {a,b}", grp.levels.size() == 2 && grp.levels[0] == "a" && grp.levels[1] == "b");
    check("grp codes 0,1,0,1", grp.codes[0] == 0 && grp.codes[1] == 1 && grp.codes[2] == 0 && grp.codes[3] == 1);
    check("note quoted comma", note.levels.size() == 4 && note.levels[0] == "hello, world");
    check("note escaped quote", note.levels[3] == "quote \"q\"");

    // force a numeric column to factor
    Frame fr2 = Frame::from_csv(csv, ',', {"id"});
    check("forced factor id", fr2.col("id").is_factor && fr2.col("id").levels.size() == 4);

    // roundtrip through .hbin
    fr.write_hbin(hbin);
    Dataset ds = Dataset::open(hbin);
    check("hbin n_rows", ds.n_rows() == 4);
    check("hbin val f64", ds.col("val").dtype == DType::F64);
    check("hbin val NaN", std::isnan(ds.col("val").f64()[1]));
    check("hbin grp factor", ds.col("grp").dtype == DType::Factor);
    check("hbin grp levels", ds.col("grp").levels.size() == 2 && ds.col("grp").levels[1] == "b");
    check("hbin grp code", ds.col("grp").codes()[3] == 1);

    native_unlink(csv);
    native_unlink(hbin);
    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("PASSED\n");
    return 0;
}
