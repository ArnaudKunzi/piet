// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Native .dta format-114 parity test (contract A §35).
//
// Format 114 (Stata 10/11 `saveold`) shares the legacy binary container with 115
// (Stata 12): every version-dependent branch in parse_old_header /
// decode_old_typecode keys off `> 113` / `> 108` / `>= 111`, which 114 and 115
// answer identically. The parser body has always handled it; only the container
// gate in read_dta ("supported: 115") kept it out, so a real 114 file was refused
// rather than mis-read.
//
// Two REAL format-114 files already in the repo are used as fixtures (first byte
// == 114 on both, asserted here so the test cannot silently start pinning a
// re-saved 115 file):
//   tests/validate/drdid2x2.dta   600 x 6 all-numeric 2x2 DID panel
//   tests/validate/deryugina.dta  620279 x 9 real-world pollution/mortality panel
//
// The expected values below were cross-checked cell-for-cell against
// pandas.read_stata() — an INDEPENDENT format-114 decoder — over every cell of
// both files (max |diff| == 0.0, NaN patterns equal). drdid2x2's cell values are
// additionally tied to the Stata answer book: collapsing the 114 load to the 2x2
// cell form reproduces tests/validate/drdid2x2_cell.csv (the fixture whose DRDID
// ATT test_drdid pins to drdid2x2.log's 2.05610834185971) to <= 4.5e-16.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
}

static bool eq(double a, double b, double tol = 1e-12) {
    return std::fabs(a - b) <= tol * (std::fabs(b) + 1.0);
}

// The gate is version-keyed, so pin the version byte on disk: if someone re-saves
// a fixture as 115 this test must fail loudly rather than quietly stop covering 114.
static int first_byte(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return -1;
    return f.get();
}

int test_dta114_main() {
    std::printf("test_dta114:\n");
    const std::string drdid = cobra_test::validate_path("drdid2x2.dta");
    const std::string dery = cobra_test::validate_path("deryugina.dta");

    check(first_byte(drdid) == 114, "drdid2x2.dta is on-disk format 114");
    check(first_byte(dery) == 114, "deryugina.dta is on-disk format 114");

    // --- drdid2x2.dta: 600 x 6, long/double mix ---------------------------------
    {
        DtaResult r = read_dta(drdid);
        const Frame& f = r.frame;
        std::printf("  drdid2x2: format=%d rows=%lld cols=%d\n", r.format_version,
                    (long long)f.n_rows(), f.n_cols());
        check(r.format_version == 114, "drdid2x2: reported format_version == 114");
        check(f.n_rows() == 600, "drdid2x2: n_rows == 600 (matches drdid2x2.log 'Number of obs = 600')");
        check(f.n_cols() == 6, "drdid2x2: n_cols == 6");

        const char* names[6] = {"id", "t", "Y", "x1", "x2", "treat"};
        for (const char* nm : names) check(f.has(nm), std::string("drdid2x2: has ") + nm);
        for (const char* nm : names) check(!f.col(nm).is_factor, std::string("drdid2x2: ") + nm + " is f64");

        // Cell values, pandas-verified. Rows 0/1 are unit id=0 (t=0 then t=1);
        // row 2 is unit id=1 at t=0. x1/x2 are time-invariant within a unit.
        check(eq(f.col("id").f64[0], 0.0), "drdid2x2: id[0] == 0");
        check(eq(f.col("t").f64[0], 0.0), "drdid2x2: t[0] == 0");
        check(eq(f.col("t").f64[1], 1.0), "drdid2x2: t[1] == 1");
        check(eq(f.col("treat").f64[0], 1.0), "drdid2x2: treat[0] == 1");
        check(eq(f.col("x1").f64[0], 0.30471707975443135, 1e-15), "drdid2x2: x1[0]");
        check(eq(f.col("x2").f64[0], 1.727350214164185, 1e-15), "drdid2x2: x2[0]");
        check(eq(f.col("x1").f64[1], 0.30471707975443135, 1e-15), "drdid2x2: x1[1] (time-invariant)");
        check(eq(f.col("x1").f64[2], -1.0399841062404955, 1e-15), "drdid2x2: x1[2]");
        check(eq(f.col("x2").f64[2], -1.5338614049161376, 1e-15), "drdid2x2: x2[2]");
        check(eq(f.col("treat").f64[2], 0.0), "drdid2x2: treat[2] == 0");
        check(eq(f.col("id").f64[599], 299.0), "drdid2x2: id[599] == 299 (300 units x 2 periods)");

        // Column means over all 600 rows (pandas-verified to 1e-12).
        auto mean = [&](const char* nm) {
            double s = 0;
            for (int64_t i = 0; i < f.n_rows(); ++i) s += f.col(nm).f64[i];
            return s / static_cast<double>(f.n_rows());
        };
        check(eq(mean("id"), 149.5, 1e-12), "drdid2x2: mean(id) == 149.5");
        check(eq(mean("t"), 0.5, 1e-12), "drdid2x2: mean(t) == 0.5");
        check(eq(mean("treat"), 0.5233333333333333, 1e-12), "drdid2x2: mean(treat)");
        check(eq(mean("Y"), 2.0285154278829696, 1e-12), "drdid2x2: mean(Y)");

        // Stata storage types survive the 114 typlist decode (251..255 direct codes).
        for (const auto& vm : r.var_meta) {
            if (vm.name == "id") check(vm.stata_type == "long", "drdid2x2: id is Stata long");
            if (vm.name == "Y") check(vm.stata_type == "double", "drdid2x2: Y is Stata double");
            if (vm.name == "Y") check(vm.format == "%10.0g", "drdid2x2: Y format %10.0g");
            if (vm.name == "id") check(vm.format == "%12.0g", "drdid2x2: id format %12.0g");
        }
    }

    // --- deryugina.dta: 620279 x 9, a real-world 114 file ------------------------
    // Size matters here: it exercises the 114 row loop far past any 16-bit or
    // block boundary, which a 600-row fixture cannot.
    {
        DtaResult r = read_dta(dery);
        const Frame& f = r.frame;
        std::printf("  deryugina: format=%d rows=%lld cols=%d\n", r.format_version,
                    (long long)f.n_rows(), f.n_cols());
        check(r.format_version == 114, "deryugina: reported format_version == 114");
        check(f.n_rows() == 620279, "deryugina: n_rows == 620279");
        check(f.n_cols() == 9, "deryugina: n_cols == 9");
        const char* names[9] = {"y_all", "pm25", "tmaxb", "tminb", "precd", "wsd", "wq", "year", "sg"};
        for (const char* nm : names) check(f.has(nm), std::string("deryugina: has ") + nm);
        // No missing cells anywhere (pandas: n_missing == 0 on every column) — a
        // silent mis-parse of the 114 missing sentinels would show up here.
        int64_t n_nan = 0;
        for (const char* nm : names)
            for (int64_t i = 0; i < f.n_rows(); ++i)
                if (std::isnan(f.col(nm).f64[i])) ++n_nan;
        check(n_nan == 0, "deryugina: zero NaN cells across all 9 columns");
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
