// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-471 T1 — `cobra eval --store-type byte|int|long|float|double`: the store boundary
// that makes a column hold what Stata's storage type would hold. End-to-end through the
// cobra CLI (argv[1]), because the flag lives in apps/cmds, which only the cobra
// executable links; the result .hbin is read back with Dataset::open.
//
// Oracle: tests/eval/golden/aer_w2/w2e_float_default.log (StataBE, 168 PREC). Values
// there are %21x; they are parsed below as hex floats (stata_hex), never compared as
// text. `+1.0000000000000X+3ff` is Stata's system missing. Every arm cites the log line
// it rests on. The fixture is the capture's own `mkfix` (w2e_float_default.do:34-41):
// 12 rows, g = mod(_n,4), src = _n/7, src2 = _n/11.
//
// Old behaviour (before this task, measured on the frozen main cobra): `eval` is outside
// the unknown-flag net (cecon_cli.cpp dispatch_command), so `--store-type float` is
// silently ignored and the column keeps the double (gen_double_13's +1.5555555555555X-002
// where gen_float_13 wants +1.5555560000000X-002), `--store-type int8` is not refused,
// and `float(1/3)` fails with "expr: unknown function float" — every arm below fails.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "data/missing.h"
#include "io/columnar.h"
#include "io/win_compat.h"

namespace fs = std::filesystem;
using cecon::Dataset;

static int g_fail = 0;
static std::string g_cobra;
static fs::path g_dir;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg.c_str());
        g_fail = 1;
    }
}

static std::string q(const std::string& s) { return "\"" + s + "\""; }
static std::string P(const std::string& name) { return (g_dir / name).string(); }

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static uint64_t bits(double v) {
    uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

static std::string hex(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%a", v);
    return b;
}

// Stata's %21x: [+-]L.MMMMMMMMMMMMMX[+-]EEE, L the leading digit (0 or 1), M 13 hex
// digits of mantissa, EEE a HEX exponent. Exponent 0x3ff is Stata's missing range
// (`.` is +1.0000000000000X+3ff); it is returned as NaN.
static double stata_hex(const char* s) {
    const double sign = (s[0] == '-') ? -1.0 : 1.0;
    const int lead = s[1] - '0';
    const uint64_t mant = std::strtoull(std::string(s + 3, 13).c_str(), nullptr, 16);
    const char* x = std::strchr(s, 'X');
    const long e = std::strtol(std::string(x + 2).c_str(), nullptr, 16) * (x[1] == '-' ? -1 : 1);
    if (e == 0x3ff) return std::numeric_limits<double>::quiet_NaN();
    return sign * std::ldexp(static_cast<double>(lead) + std::ldexp(static_cast<double>(mant), -52),
                             static_cast<int>(e));
}

// Runs `cobra eval --data <in> <ops> --out <tag>.hbin`; returns rc, stderr in `err`.
static int eval(const std::string& in, const std::string& ops, const std::string& tag,
                std::string* err = nullptr) {
    const std::string outp = P(tag + ".hbin"), errp = P(tag + ".err");
    std::error_code ec;
    fs::remove(outp, ec);
    std::string cmd = q(g_cobra) + " eval --data " + q(in) + " " + ops + " --no-provenance --out " +
                      q(outp) + " > " + q(P(tag + ".out")) + " 2> " + q(errp);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd /c strips one outer quote pair
#endif
    const int rc = std::system(cmd.c_str());
    if (err) *err = slurp(errp);
    return rc;
}

// Column `name` of <tag>.hbin as doubles (empty when the file or the column is absent).
static std::vector<double> col(const std::string& tag, const std::string& name) {
    const std::string p = P(tag + ".hbin");
    if (!fs::exists(p)) return {};
    Dataset d = Dataset::open(p);
    if (!d.has(name)) return {};
    const double* v = d.col(name).f64();
    return std::vector<double>(v, v + d.n_rows());
}

// Every row of column `name` in <tag> has exactly the bits of `want` (NaN: the same
// missing kind — `.` stays `.`, `.a` stays `.a`).
static void all_eq(const std::string& tag, const std::string& name, double want,
                   const std::string& arm) {
    std::vector<double> v = col(tag, name);
    check(!v.empty(), arm + ": column " + name + " present in " + tag);
    for (size_t i = 0; i < v.size(); ++i) {
        const bool ok = std::isnan(want)
                            ? (std::isnan(v[i]) && cecon::missing_kind(v[i]) == cecon::missing_kind(want))
                            : bits(v[i]) == bits(want);
        if (!ok) {
            check(false, arm + ": " + name + "[" + std::to_string(i + 1) + "] = " + hex(v[i]) +
                             ", want " + hex(want));
            return;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_eval_store_type <cobra-binary>\n");
        return 2;
    }
    g_cobra = argv[1];
    // The options-table drift detector: --store-type must be declared in
    // apps/cmds/options_table.cpp's `eval` row, or the int8 refusal below surfaces as
    // "options-table drift" (internal) instead of bad_input (eval checks the table on
    // its throwing path only — cecon_cli.cpp main()).
#if defined(_WIN32)
    _putenv_s("COBRA_OPTIONS_STRICT", "1");
#else
    setenv("COBRA_OPTIONS_STRICT", "1", 1);
#endif
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / ("cobra_store_type_" + std::to_string(cecon::native_process_id()));
    fs::create_directories(g_dir, ec);

    // mkfix (w2e_float_default.do:34-41), written exactly (%.17g round-trips a double).
    const std::string csv = P("mkfix.csv"), D = P("mkfix.hbin");
    {
        std::ofstream f(csv, std::ios::binary);
        f << "g,src,src2\n";
        for (int n = 1; n <= 12; ++n) {
            char b[128];
            std::snprintf(b, sizeof b, "%d,%.17g,%.17g\n", n % 4, n / 7.0, n / 11.0);
            f << b;
        }
    }
    {
        std::string cmd = q(g_cobra) + " convert " + q(csv) + " " + q(D) + " > " + q(P("cvt.out")) + " 2>&1";
#if defined(_WIN32)
        cmd = "\"" + cmd + "\"";
#endif
        check(std::system(cmd.c_str()) == 0, "convert mkfix.csv");
    }
    // ids: 120 distinct values, past byte's 100 (egen nvals arms below).
    const std::string icsv = P("ids.csv"), IDS = P("ids.hbin");
    {
        std::ofstream f(icsv, std::ios::binary);
        f << "id\n";
        for (int n = 1; n <= 120; ++n) f << n << "\n";
    }
    {
        std::string cmd = q(g_cobra) + " convert " + q(icsv) + " " + q(IDS) + " > " + q(P("cvt2.out")) + " 2>&1";
#if defined(_WIN32)
        cmd = "\"" + cmd + "\"";
#endif
        check(std::system(cmd.c_str()) == 0, "convert ids.csv");
    }
    const double MISS = cecon::stata_missing(cecon::kMissSys);
    const double MISS_A = cecon::stata_missing(cecon::kMissA);

    // --- float: generate ---------------------------------------------------------------
    {
        std::string err;
        const int rc = eval(D, "--store-type float --gen \"u13=1/3\" --gen \"u27=2.7\" "
                               "--gen \"ubig=123456789\" --gen \"up24=16777217\" --gen \"u300=1e300\" "
                               "--gen \"una=.a\" --gen \"usys=.\"", "flt", &err);
        check(rc == 0, "float gen: rc 0 (stderr: " + err + ")");
        // :96 gen_untyped_13 / :110 gen_float_13
        all_eq("flt", "u13", stata_hex("+1.5555560000000X-002"), "gen_float_13 :110");
        all_eq("flt", "u13", static_cast<double>(static_cast<float>(1.0 / 3.0)), "(double)(float)(1/3)");
        all_eq("flt", "u27", stata_hex("+1.59999a0000000X+001"), "gen_float_27 :194");
        all_eq("flt", "ubig", stata_hex("+1.d6f3460000000X+01a"), "gen_float_123456789 :446");
        all_eq("flt", "up24", stata_hex("+1.0000000000000X+018"), "gen_untyped_16777217 :558");
        // :530 gen_float_1e300 — system missing at rc 0
        all_eq("flt", "u300", MISS, "gen_float_1e300 :530");
        // missing values pass through untouched (SPEC §3.3; float(.a) is fn_float_missing :1818)
        all_eq("flt", "una", MISS_A, ".a survives --store-type float");
        all_eq("flt", "usys", MISS, ". survives --store-type float");
    }
    // --- the no-flag column is the double, and `double` is bit-identical to no flag ----
    {
        check(eval(D, "--gen \"u13=1/3\" --egen \"em=mean(src)\" --rowegen \"rt=rowtotal(src,src2)\"",
                   "none") == 0, "no flag: rc 0");
        check(eval(D, "--store-type double --gen \"u13=1/3\" --egen \"em=mean(src)\" "
                      "--rowegen \"rt=rowtotal(src,src2)\"", "dbl") == 0, "double: rc 0");
        // :124 gen_double_13
        all_eq("none", "u13", stata_hex("+1.5555555555555X-002"), "gen_double_13 :124 (no flag)");
        all_eq("dbl", "u13", stata_hex("+1.5555555555555X-002"), "gen_double_13 :124 (--store-type double)");
        std::vector<double> f = col("flt", "u13"), n = col("none", "u13");
        check(!f.empty() && !n.empty() && bits(f[0]) != bits(n[0]),
              "--store-type float differs from the no-flag column");
        for (const char* c : {"u13", "em", "rt"}) {
            std::vector<double> a = col("none", c), b = col("dbl", c);
            bool same = !a.empty() && a.size() == b.size();
            for (size_t i = 0; same && i < a.size(); ++i) same = bits(a[i]) == bits(b[i]);
            check(same, std::string("--store-type double is bit-identical to no flag: ") + c);
        }
        check(slurp(P("none.hbin")) == slurp(P("dbl.hbin")),
              "--store-type double writes the same .hbin bytes as no flag");
    }
    // --- byte / int / long: truncation toward zero, out of range is `.` -------------------
    {
        const char* T[] = {"byte", "int", "long"};
        for (const char* t : T) {
            const std::string tag = std::string("i_") + t;
            std::string err;
            const int rc = eval(D, std::string("--store-type ") + t +
                                       " --gen \"v13=1/3\" --gen \"v27=2.7\" --gen \"vm25=-2.5\" "
                                       "--gen \"v1e10=1e10\" --gen \"vbig=123456789\" --gen \"vp24=16777217\" "
                                       "--gen \"vm13=-1/3\" --gen \"vna=.a\"",
                                tag, &err);
            check(rc == 0, tag + ": rc 0 (stderr: " + err + ")");
            // :138 gen_byte_13 / :152 gen_int_13 / :166 gen_long_13
            all_eq(tag, "v13", stata_hex("+0.0000000000000X-3ff"), tag + " 1/3 -> 0 (:138/:152/:166)");
            // :222 / :236 / :250 gen_*_27
            all_eq(tag, "v27", stata_hex("+1.0000000000000X+001"), tag + " 2.7 -> 2 (:222/:236/:250)");
            // :306 / :320 / :334 gen_*_m25
            all_eq(tag, "vm25", stata_hex("-1.0000000000000X+001"), tag + " -2.5 -> -2 (:306/:320/:334)");
            // :390 / :404 / :418 gen_*_1e10
            all_eq(tag, "v1e10", MISS, tag + " 1e10 -> . (:390/:404/:418)");
            // an integer type has no -0 (DESIGN: trunc(-1/3) stores +0)
            all_eq(tag, "vm13", 0.0, tag + " -1/3 -> +0");
            all_eq(tag, "vna", MISS_A, tag + " .a survives");
        }
        // :474 gen_byte_123456789 / :488 gen_int_123456789 -> . ; :502 gen_long_123456789 exact
        all_eq("i_byte", "vbig", MISS, "gen_byte_123456789 :474");
        all_eq("i_int", "vbig", MISS, "gen_int_123456789 :488");
        all_eq("i_long", "vbig", stata_hex("+1.d6f3454000000X+01a"), "gen_long_123456789 :502");
        // :586 gen_long_16777217 exact
        all_eq("i_long", "vp24", stata_hex("+1.0000010000000X+018"), "gen_long_16777217 :586");
    }
    {   // :601 gen_byte_300 / :615 gen_int_40000 / :629 gen_long_3e9 — `.` at rc 0
        check(eval(D, "--store-type byte --gen \"o=300\"", "ob") == 0, "byte 300: rc 0");
        check(eval(D, "--store-type int --gen \"o=40000\"", "oi") == 0, "int 40000: rc 0");
        check(eval(D, "--store-type long --gen \"o=3e9\"", "ol") == 0, "long 3e9: rc 0");
        all_eq("ob", "o", MISS, "gen_byte_300 :601");
        all_eq("oi", "o", MISS, "gen_int_40000 :615");
        all_eq("ol", "o", MISS, "gen_long_3e9 :629");
    }
    // --- float() in the row evaluator --------------------------------------------------
    {
        std::string err;
        const int rc = eval(D, "--gen \"f13=float(1/3)\" --gen \"f300=float(1e300)\" "
                               "--gen \"fbig=float(123456789)\" --gen \"fneg=float(-2.7)\" "
                               "--gen \"fna=float(.a)\" --gen \"fsys=float(.)\" --gen \"d13=1/3\" "
                               "--gen \"fsrc=float(src)\"", "fn", &err);
        check(rc == 0, "float(): rc 0 (stderr: " + err + ")");
        all_eq("fn", "f13", stata_hex("+1.5555560000000X-002"), "fn_float_13 :1773");
        all_eq("fn", "f300", MISS, "fn_float_1e300 :1839");
        all_eq("fn", "fbig", stata_hex("+1.d6f3460000000X+01a"), "fn_float_big :1849");
        all_eq("fn", "fneg", stata_hex("-1.59999a0000000X+001"), "fn_float_neg :1859");
        all_eq("fn", "fna", MISS_A, "fn_float_missing :1818 (.a stays .a)");
        all_eq("fn", "fsys", MISS, "fn_float_sysmiss :1828");
        // float(1/3) equals the --store-type float column bit for bit (:1773 vs :96)
        std::vector<double> a = col("fn", "f13"), b = col("flt", "u13");
        check(!a.empty() && !b.empty() && bits(a[0]) == bits(b[0]),
              "float(1/3) == the --store-type float column");
        // the vector path: float() over a column, row by row
        std::vector<double> s = col("fn", "fsrc");
        bool ok = s.size() == 12;
        for (size_t i = 0; ok && i < s.size(); ++i)
            ok = bits(s[i]) == bits(static_cast<double>(static_cast<float>((i + 1) / 7.0)));
        check(ok, "float(src) rounds every row");
    }
    {   // :1784 fn_float_eq_count — a float column equals float(1/3) on every row, and
        // (control, fn_lit_eq_count) never equals the double literal.
        check(eval(D, "--store-type float --gen \"u13=1/3\"", "eqa") == 0, "eq stage 1: rc 0");
        check(eval(P("eqa.hbin"), "--gen \"eq=(u13==float(1/3))\" --gen \"lit=(u13==1/3)\"", "eqb") == 0,
              "eq stage 2: rc 0");
        all_eq("eqb", "eq", 1.0, "fn_float_eq_count :1784");
        all_eq("eqb", "lit", 0.0, "fn_lit_eq_count (control)");
    }
    // --- the egen family, row egen, bygen: one flag reaches every output ----------------
    {
        std::string err;
        const int rc = eval(D, "--store-type float --gen \"u13=1/3\" --egen \"em=mean(src)\" "
                               "--egen \"emb=mean(src), by(g)\" --egen \"et=total(src)\" "
                               "--rowegen \"rt=rowtotal(src,src2)\" --bygen \"bg=1/3, by(g)\"",
                            "eg", &err);
        check(rc == 0, "egen float: rc 0 (stderr: " + err + ")");
        // one --store-type applies to the --gen AND the --egen of the same argv
        all_eq("eg", "u13", stata_hex("+1.5555560000000X-002"), "gen in an egen argv (:96)");
        all_eq("eg", "em", stata_hex("+1.db6db60000000X-001"), "egen_mean_untyped :953");
        all_eq("none", "em", stata_hex("+1.db6db6db6db6dX-001"), "egen_mean_double :968 (no flag)");
        all_eq("eg", "et", stata_hex("+1.64924a0000000X+003"), "egen_total_untyped :1010");
        std::vector<double> r = col("eg", "rt");
        check(!r.empty() && bits(r[0]) == bits(stata_hex("+1.dec0d60000000X-003")),
              "egen_rowtotal_untyped :1038 (row 1)");
        // :982 egen_mean_by_untyped — rows 1 and 2 (g = 1, 2)
        std::vector<double> m = col("eg", "emb");
        check(m.size() == 12 && bits(m[0]) == bits(stata_hex("+1.6db6dc0000000X-001")) &&
                  bits(m[1]) == bits(stata_hex("+1.b6db6e0000000X-001")),
              "egen_mean_by_untyped :982");
        // bysort g: gen — the gen rule (:96) through the bygen path
        all_eq("eg", "bg", stata_hex("+1.5555560000000X-002"), "bygen float (rule of :96)");
    }
    {   // cut forwards the flag; mode does not. cut(src), at(0.1 5) stores the left edge
        // 0.1 on every row, float-rounded like any literal (rule of :96). mode(src),
        // minmode is the smallest src = 1/7: egen mode
        // replaces the requested type with the SOURCE variable's own type (src is a
        // double) and copies a source value, so the result is exact under float AND
        // under byte (a byte store would give 0).
        std::string err;
        const int rc = eval(D, "--store-type float --egen \"md=mode(src), minmode\" "
                               "--egen \"ct=cut(src), at(0.1 5)\"", "eg2", &err);
        check(rc == 0, "egen mode/cut float: rc 0 (stderr: " + err + ")");
        all_eq("eg2", "md", 1.0 / 7.0, "egen mode keeps the source's value");
        all_eq("eg2", "ct", static_cast<double>(static_cast<float>(0.1)), "egen cut float");
        const int rcb = eval(D, "--store-type byte --egen \"mb=mode(src), minmode\"", "eg2b", &err);
        check(rcb == 0, "egen mode byte: rc 0 (stderr: " + err + ")");
        all_eq("eg2b", "mb", 1.0 / 7.0, "egen mode byte keeps the source's value");
    }
    {   // nvals (built as a running sum, then replaced into the variable): an integer type WIDENS under replace (replace_byte_overflow),
        // so 120 distinct values are an exact 120 under byte, where a byte store gives `.`
        // (> 100). Float rounds the count once; 120 is a float, the control.
        std::string err;
        check(eval(IDS, "--store-type byte --egen \"nv=nvals(id)\"", "nvb", &err) == 0,
              "egen nvals byte: rc 0 (stderr: " + err + ")");
        all_eq("nvb", "nv", 120.0, "egen nvals byte is exact (widening under replace)");
        check(eval(IDS, "--store-type float --egen \"nv=nvals(id)\"", "nvf", &err) == 0,
              "egen nvals float: rc 0 (stderr: " + err + ")");
        all_eq("nvf", "nv", 120.0, "egen nvals float control");
    }
    // --- egen results Stata builds with `replace` ----------------------------------------
    // replace keeps a float variable float (replace_float_23 :761) but WIDENS an integer
    // one (replace_byte_promote :810, replace_byte_overflow :876, replace_int_overflow
    // :893) instead of truncating it or storing `.`.
    {
        // seq: Stata ignores the requested type and replaces into
        // a byte variable, so seq(), from(95) to(120) under byte is 95..106, exact past
        // byte's 100 (widened to int, as at :876). Old behaviour: 101..106 stored `.`.
        std::string err;
        check(eval(D, "--store-type byte --egen \"sq=seq(), from(95) to(120)\"", "eg3", &err) == 0,
              "egen seq byte: rc 0 (stderr: " + err + ")");
        std::vector<double> s = col("eg3", "sq");
        bool ok = s.size() == 12;
        for (size_t i = 0; ok && i < s.size(); ++i) ok = s[i] == 95.0 + static_cast<double>(i);
        check(ok, "egen seq byte: 95..106, the type ignored (widening as for replace)");
        // rowtotal/rowmax/rowmin, default rank and a fractional cut point under an integer
        // type are refused by name: Stata widens (:810/:876) where a fixed store type would
        // truncate. Old behaviour: rc 0 with rowtotal(src,src2) row 1 truncated to 0.
        struct Ref { const char* ops; const char* tag; const char* msg; };
        const Ref R[] = {
            {"--store-type byte --rowegen \"rt=rowtotal(src,src2)\"", "rf1", "egen rowtotal(): --store-type"},
            {"--store-type int --rowegen \"rx=rowmax(src,src2)\"", "rf2", "egen rowmax(): --store-type"},
            {"--store-type long --rowegen \"rn=rowmin(src,src2)\"", "rf3", "egen rowmin(): --store-type"},
            {"--store-type byte --egen \"rk=rank(g)\"", "rf4", "egen rank(): --store-type"},
            {"--store-type int --egen \"ct=cut(src), at(0.1 5)\"", "rf5", "egen cut(): --store-type"},
        };
        for (const Ref& r : R) {
            const int rc = eval(D, r.ops, r.tag, &err);
            check(rc != 0, std::string(r.tag) + ": refused (rc != 0)");
            check(err.find("bad_input") != std::string::npos && err.find(r.msg) != std::string::npos,
                  std::string(r.tag) + ": bad_input naming " + r.msg + " (stderr: " + err + ")");
            check(!fs::exists(P(std::string(r.tag) + ".hbin")), std::string(r.tag) + ": no output written");
        }
        // Controls that stay accepted under byte: rowmiss counts (exact under widening),
        // rank, field (a single store of the first tied `_n`), cut with integer edges and
        // icodes (exact under widening), and rowtotal under float (:1038, above).
        check(eval(D, "--store-type byte --rowegen \"rm=rowmiss(src,src2)\" --egen \"rk=rank(src), field\" "
                      "--egen \"ci=cut(src), at(0.1 5) icodes\" --egen \"cz=cut(src), at(0 5)\"",
                   "eg4", &err) == 0,
              "byte rowmiss/rank field/cut icodes: rc 0 (stderr: " + err + ")");
        all_eq("eg4", "rm", 0.0, "rowmiss under byte");
        all_eq("eg4", "ci", 0.0, "cut icodes under byte");
        all_eq("eg4", "cz", 0.0, "cut at(0 5) under byte");
        std::vector<double> k = col("eg4", "rk");
        ok = k.size() == 12;
        for (size_t i = 0; ok && i < k.size(); ++i) ok = k[i] == 12.0 - static_cast<double>(i);
        check(ok, "rank(src), field under byte: 12..1");
    }
    // --- an unknown type is a named refusal ------------------------------------------------
    {
        std::string err;
        const int rc = eval(D, "--store-type int8 --gen \"u=1\"", "bad", &err);
        check(rc != 0, "--store-type int8: refused (rc != 0)");
        check(err.find("bad_input") != std::string::npos, "--store-type int8: bad_input (stderr: " + err + ")");
        check(err.find("--store-type expects byte|int|long|float|double") != std::string::npos,
              "--store-type int8: message names the accepted types");
        check(!fs::exists(P("bad.hbin")), "--store-type int8: no output written");
    }

    fs::remove_all(g_dir, ec);
    std::printf(g_fail ? "eval_store_type: FAIL\n" : "eval_store_type: all arms pass\n");
    return g_fail;
}
