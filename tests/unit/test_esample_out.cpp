// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// e(sample) T1a — `--esample-out PATH [--esample-index COL --esample-n N]` on the
// linear verbs PIET fits with (feols, iv, xtreg, newey, prais). End-to-end through
// the cobra CLI (argv[1]), because the flag lives in apps/cmds, which only the cobra
// executable links.
//
// Oracle: tests/eval/golden/aer_w2/w2_esample.log (StataBE 19.5, 103 arms). The
// fixture below is that capture's data, rebuilt from w2_esample.do's deterministic
// generate lines (61 rows: 10 ids x 6 periods + a singleton id 11; y missing in row
// 7, x1 in row 13, cl in row 20, w == 0 in row 26, w missing in row 31). Every
// expected count cites the PREC| line it comes from; the engine's 1-count must equal
// Stata's e(N) == `count if e(sample)` on that arm.
//
// Old behaviour (before this task): every `--esample-out` run is refused as an
// unrecognized option (bad_input, no file), so every positive arm here fails, and the
// bad-index arms fail their "message names --esample-index / no file" checks only
// where the refusal would have to name the index column.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "io/win_compat.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

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

struct Run {
    int rc = -1;
    json out;          // parsed --out JSON (null when absent)
    std::string err;   // stderr text
};

// Runs `cobra <args> --out <json> --no-provenance`, stderr captured to a file.
static Run cobra(const std::string& args, const std::string& tag) {
    const std::string outp = P(tag + ".json"), errp = P(tag + ".err");
    std::error_code ec;
    fs::remove(outp, ec);
    std::string cmd = q(g_cobra) + " " + args + " --no-provenance --out " + q(outp) + " 2> " + q(errp);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd /c strips one outer quote pair
#endif
    Run r;
    r.rc = std::system(cmd.c_str());
    r.err = slurp(errp);
    if (fs::exists(outp)) {
        try { r.out = json::parse(slurp(outp)); } catch (const std::exception&) { r.out = json(); }
    }
    return r;
}

// The mask file as a vector of 0/1, or {-1} when a line is not exactly "0"/"1".
static std::vector<int> read_mask(const std::string& path) {
    std::vector<int> v;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line == "1") v.push_back(1);
        else if (line == "0") v.push_back(0);
        else return {-1};
    }
    return v;
}

static int ones(const std::vector<int>& m) {
    int n = 0;
    for (int v : m) n += (v == 1);
    return n;
}

// Stata's mod() for the non-negative arguments the .do uses.
static double smod(double a, double b) { return a - b * std::floor(a / b); }

struct Row {
    int id, t, cl, st;
    double x1, x2, z1, w, y;
    bool y_mis = false, x1_mis = false, cl_mis = false, w_mis = false;
};
static std::vector<Row> g_rows;  // index r-1 == Stata row r

// w2_esample.do:10-27, line for line.
static void build_rows() {
    for (int n = 1; n <= 61; ++n) {
        Row r{};
        r.id = static_cast<int>(std::ceil(n / 6.0));
        r.t = static_cast<int>(smod(n - 1, 6)) + 1;
        if (n == 61) { r.id = 11; r.t = 1; }
        r.x1 = smod(n * 7, 11) + smod(n, 5) / 10;
        r.x2 = smod(n * 3, 5) + smod(n * 13, 7) / 10;
        r.z1 = smod(n * 5, 13) + r.x1 / 2;
        r.w = 1 + smod(n, 4);
        r.cl = 1 + static_cast<int>(smod(n, 7));
        r.y = 1 + 2 * r.x1 - r.x2 + r.id / 3.0 + smod(n * 11, 17) / 5;
        // Survey strata for the svy arm (not in the .do): a function of the PSU `cl`
        // so every PSU nests in one stratum (svy refuses PSUs that cross strata).
        r.st = 1 + (r.cl % 2);
        if (n == 7) r.y_mis = true;
        if (n == 13) r.x1_mis = true;
        if (n == 20) r.cl_mis = true;
        if (n == 26) r.w = 0;
        if (n == 31) r.w_mis = true;
        g_rows.push_back(r);
    }
}

static std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

// Writes the rows `order` (1-based Stata row numbers, in that order) as CSV with an
// extra `pos` column = the Stata row number (PIET's __piet_inidx__ shape) and
// converts it to .hbin.
static std::string write_fixture(const std::string& name, const std::vector<int>& order,
                                 const std::string& pos_override_row = "") {
    const std::string csv = P(name + ".csv"), hbin = P(name + ".hbin");
    std::ofstream f(csv, std::ios::binary);
    f << "id,t,x1,x2,z1,w,cl,st,y,rowno,pos\n";
    for (size_t i = 0; i < order.size(); ++i) {
        const Row& r = g_rows[static_cast<size_t>(order[i] - 1)];
        std::string pos = std::to_string(order[i]);
        if (!pos_override_row.empty() && i == 0) pos = pos_override_row;
        f << r.id << "," << r.t << "," << (r.x1_mis ? "" : num(r.x1)) << "," << num(r.x2) << ","
          << num(r.z1) << "," << (r.w_mis ? "" : num(r.w)) << "," << (r.cl_mis ? "" : std::to_string(r.cl))
          << "," << r.st << "," << (r.y_mis ? "" : num(r.y)) << "," << order[i] << "," << pos << "\n";
    }
    f.close();
    std::string cmd = q(g_cobra) + " convert " + q(csv) + " " + q(hbin) + " > " + q(P(name + ".cvt")) + " 2>&1";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) {
        std::printf("  FAIL: convert %s\n", name.c_str());
        g_fail = 1;
    }
    return hbin;
}

// One positive arm: runs `args` with --esample-out, checks the file shape, the
// 1-count against the golden, esample_n against the 1-count and n_obs (or n_rows),
// and returns the mask for arm-specific row checks.
static std::vector<int> arm(const std::string& tag, const std::string& args, int want_lines,
                            int want_ones, const char* n_key = "n_obs") {
    const std::string mp = P(tag + ".mask");
    std::error_code ec;
    fs::remove(mp, ec);
    Run r = cobra(args + " --esample-out " + q(mp), tag);
    check(r.rc == 0, tag + ": rc 0 (stderr: " + r.err + ")");
    std::vector<int> m = read_mask(mp);
    check(!(m.size() == 1 && m[0] == -1), tag + ": every line is exactly 0 or 1");
    check(static_cast<int>(m.size()) == want_lines,
          tag + ": " + std::to_string(m.size()) + " lines, want " + std::to_string(want_lines));
    check(ones(m) == want_ones,
          tag + ": " + std::to_string(ones(m)) + " ones, golden count " + std::to_string(want_ones));
    if (r.out.is_object()) {
        check(r.out.contains("esample_n") && r.out["esample_n"] == ones(m),
              tag + ": esample_n == number of 1s");
        check(r.out.contains(n_key) && r.out[n_key] == ones(m),
              tag + ": esample_n == " + std::string(n_key));
    } else {
        check(false, tag + ": JSON result present");
    }
    return m;
}

// A refusal arm: nonzero rc, the named error code, a message naming `needle`, and
// no mask file left behind.
static void refusal(const std::string& tag, const std::string& args, const std::string& code,
                    const std::string& needle) {
    const std::string mp = P(tag + ".mask");
    std::error_code ec;
    fs::remove(mp, ec);
    Run r = cobra(args + " --esample-out " + q(mp), tag);
    check(r.rc != 0, tag + ": refused (rc != 0)");
    check(r.err.find("\"" + code + "\"") != std::string::npos, tag + ": error code " + code + " (stderr: " + r.err + ")");
    check(r.err.find(needle) != std::string::npos, tag + ": message names " + needle);
    check(!fs::exists(mp), tag + ": no mask file written");
}

static void strip_volatile(json& j) {
    if (!j.is_object()) return;
    j.erase("timing_ms");
    j.erase("time_ms");
    j.erase("esample_n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_esample_out <cobra-binary>\n");
        return 2;
    }
    g_cobra = argv[1];
    // Run every child under the options-table drift detector, so each flag an arm
    // drives (the three esample flags on every wired verb, and on the ols / prais
    // --corc refusals, which consume them to refuse by name) must be declared in
    // apps/cmds/options_table.cpp or the arm fails with "options-table drift".
#if defined(_WIN32)
    _putenv_s("COBRA_OPTIONS_STRICT", "1");
#else
    setenv("COBRA_OPTIONS_STRICT", "1", 1);
#endif
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / ("cobra_esample_" + std::to_string(cecon::native_process_id()));
    fs::create_directories(g_dir, ec);
    build_rows();

    std::vector<int> all(61);
    for (int i = 0; i < 61; ++i) all[static_cast<size_t>(i)] = i + 1;
    const std::string D = write_fixture("full", all);
    const std::string base = "--data " + q(D);

    // --- no --keepif: identity ------------------------------------------------------
    {   // PREC|C1_listwise|0|59|59| — rows 7 (y) and 13 (x1) out.
        auto m = arm("C1", "feols " + base + " --y y --x x1,x2", 61, 59);
        if (m.size() == 61) check(m[6] == 0 && m[12] == 0 && m[0] == 1, "C1: rows 7/13 are 0, row 1 is 1");
    }
    {   // PREC|C2_cluster_missing|0|58|58|7| — row 20 (cl) out.
        auto m = arm("C2", "feols " + base + " --y y --x x1,x2 --vcov cluster --cluster cl", 61, 58);
        if (m.size() == 61) check(m[19] == 0, "C2: missing-cluster row 20 is 0");
    }
    {   // PREC|C3_pweight_zero_missing|0|57|57| — row 26 (w==0) and 31 (w missing) out.
        auto m = arm("C3", "feols " + base + " --y y --x x1,x2 --weight w --wtype pw --vcov hc1", 61, 57);
        if (m.size() == 61) check(m[25] == 0 && m[30] == 0, "C3: zero-weight row 26 and missing-weight row 31 are 0");
    }
    // PREC|C4_aweight|0|57|57|
    arm("C4", "feols " + base + " --y y --x x1,x2 --weight w --wtype aw", 61, 57);
    {   // fweight: e(N) is sum(w), so esample_n is the ROW count and must equal n_rows.
        arm("C4fw", "feols " + base + " --y y --x x1,x2 --weight w --wtype fw", 61, 57, "n_rows");
        Run r = cobra("feols " + base + " --y y --x x1,x2 --weight w --wtype fw", "C4fw_plain");
        check(r.out.is_object() && r.out["n_obs"] != 57, "C4fw: n_obs is sum(w), not the row count");
    }
    // PREC|C5_tsop_loss|0|49|49| — L.x1 loses each panel's first period (+ row 13's lead).
    arm("C5", "feols " + base + " --y y --x L.x1,x2 --xtset id,t", 61, 49);
    // Multi-outcome batch (common sample): y missing in row 7, x1 in 13 -> 59.
    arm("multi", "feols " + base + " --y y,z1 --x x1,x2", 61, 59);

    // --- singleton drops --------------------------------------------------------------
    {   // PREC|D1_reghdfe_singleton_out|0|58|58|1| and PREC|D1b_reghdfe_id11|0|
        auto m = arm("D1", "feols " + base + " --y y --x x1,x2 --fe id", 61, 58);
        if (m.size() == 61) check(m[60] == 0, "D1b: the singleton id 11 (row 61) is 0");
    }
    {   // PREC|D2_reghdfe_keepsingletons|0|59|59|
        auto m = arm("D2", "feols " + base + " --y y --x x1,x2 --fe id --keep-singletons", 61, 59);
        if (m.size() == 61) check(m[60] == 1, "D2: --keep-singletons keeps row 61");
    }
    // PREC|D5_reghdfe_cluster_missing|0|57|57|7| — singleton AND missing cluster.
    arm("D5", "feols " + base + " --y y --x x1,x2 --fe id --vcov cluster --cluster cl", 61, 57);
    // The reghdfe-stages-ffirst.log:423-432 shape (count if e(sample) after an aweight +
    // absorb + cluster fit with a singleton): here aw + absorb + cluster + singleton +
    // zero/missing weight + missing cluster: 61 - {7,13,20,26,31} - {61} = 55.
    arm("ffirst_shape_aw", "feols " + base + " --y y --x x1,x2 --fe id --weight w --wtype aw "
        "--vcov cluster --cluster cl", 61, 55);

    // --- plain --keepif: physically reduced scratch, mapped back -----------------------
    {   // PREC|B0_basefit|0|28| / PREC|B1_count|0|28|28|
        auto m = arm("B1", "feols " + base + " --y y --x x1,x2 --keepif \"id<=5\"", 61, 28);
        if (m.size() == 61) {
            bool ok = true;
            for (int r = 1; r <= 61; ++r) {
                const int want = (g_rows[static_cast<size_t>(r - 1)].id <= 5 && r != 7 && r != 13) ? 1 : 0;
                ok = ok && m[static_cast<size_t>(r - 1)] == want;
            }
            check(ok, "B1: exactly the id<=5 rows minus 7 and 13 are 1");
        }
    }
    // PREC|H6_xtreg_fe_if|0|50|50| (xtreg,fe routes to feols --fe in PIET)
    arm("H6", "feols " + base + " --y y --x x1,x2 --fe id --keep-singletons --keepif \"t>=2\"", 61, 50);

    // --- --keepif + TS operator: the E1 mask path over the FULL file -------------------
    {   // PREC|C6_tsop_if_boundary|0|40|40| and PREC|C6b_tsop_if_boundary_t3|10|
        auto m = arm("C6", "feols " + base + " --y y --x L.x1,x2 --xtset id,t --keepif \"t>=3\"", 61, 40);
        if (m.size() == 61) {
            int t3 = 0;
            for (int r = 1; r <= 61; ++r)
                if (g_rows[static_cast<size_t>(r - 1)].t == 3) t3 += m[static_cast<size_t>(r - 1)];
            check(t3 == 10, "C6b: the t==3 boundary rows (lag from an if-excluded row) count 10, got " +
                                std::to_string(t3));
        }
    }

    // --- iv ------------------------------------------------------------------------------
    // PREC|H3_ivregress|0|59|59|
    arm("H3", "iv " + base + " --y y --x x2 --endog x1 --instruments z1", 61, 59);
    // PREC|H4_ivreg2_cluster|0|58|58|
    arm("H4", "iv " + base + " --y y --x x2 --endog x1 --instruments z1 --vcov cluster --cluster cl", 61, 58);
    {   // PREC|H5_ivreghdfe_singleton|0|58|58|
        auto m = arm("H5", "iv " + base + " --y y --x x2 --endog x1 --instruments z1 --fe id", 61, 58);
        if (m.size() == 61) check(m[60] == 0, "H5: ivreghdfe-style singleton row 61 is 0");
    }
    // IV + absorb + cluster + singleton (reghdfe-stages-ffirst A1 shape without the
    // aweight, which `cobra iv` does not take): 61 - {7,13,20} - {61} = 57.
    arm("ffirst_shape_iv", "iv " + base + " --y y --x x2 --endog x1 --instruments z1 --fe id "
        "--vcov cluster --cluster cl", 61, 57);
    // iv plain --keepif: B-arm sample under 2SLS (28).
    arm("iv_keepif", "iv " + base + " --y y --x x2 --endog x1 --instruments z1 --keepif \"id<=5\"", 61, 28);

    // --- --esample-index scatter ---------------------------------------------------------
    {   // PREC|H9_regress_if_in|0|22|22|: `regress ... if id<=5 in 1/24`. PIET pre-filters
        // `in 1/24` physically and carries each row's head position; the subset is written
        // in REVERSE order so identity would put the ones in the wrong rows.
        std::vector<int> sub;
        for (int r = 24; r >= 1; --r) sub.push_back(r);
        const std::string S = write_fixture("sub", sub);
        auto m = arm("H9", "feols --data " + q(S) + " --y y --x x1,x2 --keepif \"id<=5\" "
                     "--esample-index pos --esample-n 61", 61, 22);
        if (m.size() == 61) {
            bool ok = true;
            for (int r = 1; r <= 61; ++r) ok = ok && m[static_cast<size_t>(r - 1)] == ((r <= 24 && r != 7 && r != 13) ? 1 : 0);
            check(ok, "H9: ones exactly on head rows 1..24 minus 7 and 13");
        }
        // Scatter with no --keepif (identity load): 24 rows minus 7, 13 = 22.
        arm("scatter_plain", "feols --data " + q(S) + " --y y --x x1,x2 --esample-index pos --esample-n 61", 61, 22);
    }
    {   // bad_input: out-of-range, non-integral, missing and repeated positions.
        std::vector<int> sub;
        for (int r = 1; r <= 24; ++r) sub.push_back(r);
        const std::string R = write_fixture("bad_range", sub, "62");
        refusal("bad_range", "feols --data " + q(R) + " --y y --x x1,x2 --esample-index pos --esample-n 61",
                "bad_input", "--esample-index");
        const std::string F = write_fixture("bad_frac", sub, "1.5");
        refusal("bad_frac", "feols --data " + q(F) + " --y y --x x1,x2 --esample-index pos --esample-n 61",
                "bad_input", "--esample-index");
        const std::string Z = write_fixture("bad_zero", sub, "0");
        refusal("bad_zero", "feols --data " + q(Z) + " --y y --x x1,x2 --esample-index pos --esample-n 61",
                "bad_input", "--esample-index");
        const std::string M = write_fixture("bad_miss", sub, ".");  // "." = a missing pos cell
        refusal("bad_miss", "feols --data " + q(M) + " --y y --x x1,x2 --esample-index pos --esample-n 61",
                "bad_input", "--esample-index");
        const std::string U = write_fixture("bad_dup", sub, "2");
        refusal("bad_dup", "feols --data " + q(U) + " --y y --x x1,x2 --esample-index pos --esample-n 61",
                "bad_input", "repeats position");
        // N smaller than the positions actually present.
        refusal("bad_small_n", "iv --data " + q(P("sub.hbin")) +
                " --y y --x x2 --endog x1 --instruments z1 --esample-index pos --esample-n 10",
                "bad_input", "--esample-index");
        // Pairing rules.
        refusal("bad_pair", "feols " + base + " --y y --x x1,x2 --esample-index pos", "bad_input", "--esample-n");
        refusal("bad_n", "feols " + base + " --y y --x x1,x2 --esample-index pos --esample-n 6x",
                "bad_input", "--esample-n");
    }
    {   // --esample-index without --esample-out is refused too (nothing would be written).
        Run r = cobra("feols " + base + " --y y --x x1,x2 --esample-index pos --esample-n 61", "bad_noout");
        check(r.rc != 0 && r.err.find("--esample-out") != std::string::npos,
              "bad_noout: --esample-index without --esample-out is bad_input");
    }

    // --- xtreg (RE), newey, prais ----------------------------------------------------------
    // xtreg,re keeps the singleton (no FE absorption): 61 - {7,13} = 59.
    arm("xtreg_re", "xtreg " + base + " --y y --x x1,x2 --ivar id", 61, 59);
    arm("xtreg_re_keepif", "xtreg " + base + " --y y --x x1,x2 --ivar id --keepif \"id<=5\"", 61, 28);
    // The listwise drop of rows 7 and 13 leaves rowno with gaps, which newey refuses
    // without --force (newey.ado's spacing check, BUG-1503); the mask is the same.
    arm("newey", "newey " + base + " --y y --x x1,x2 --timevar rowno --lag 1 --force", 61, 59);
    {
        auto m = arm("newey_keepif", "newey " + base + " --y y --x x1,x2 --timevar rowno --lag 1 --force --keepif \"id<=5\"", 61, 28);
        if (m.size() == 61) check(m[40] == 0, "newey_keepif: row 41 (id 7) is 0");
    }
    arm("prais", "prais " + base + " --y y --x x1,x2 --timevar rowno", 61, 59);
    refusal("prais_corc", "prais " + base + " --y y --x x1,x2 --timevar rowno --corc", "unsupported", "--corc");

    // --- svy: regress --------------------------------------------------------------------
    // pw drops 26/31, psu cl drops 20, listwise 7/13: 56.
    arm("svy", "feols " + base + " --y y --x x1,x2 --svy --psu cl --strata st --weight w --wtype pw", 61, 56);

    // --- ols: no admitted-row set is exposed -> named refusal ----------------------------
    refusal("ols", "ols " + base + " --y y --x x1,x2", "unsupported", "--esample-out");

    // --- the file is exactly what `cobra setcol` takes -----------------------------------
    {
        const std::string mp = P("C1.mask");
        Run r = cobra("setcol --data " + q(D) + " --var es --values " + q(mp) + " --dtype f64 --new --out " +
                      q(P("with_es.hbin")), "setcol");
        check(r.rc == 0, "setcol accepts the --esample-out file as --values (stderr: " + r.err + ")");
    }

    // --- without the flag nothing changes -------------------------------------------------
    {
        const char* specs[] = {
            "feols --y y --x x1,x2 --fe id --vcov cluster --cluster cl",
            "feols --y y --x x1,x2 --weight w --wtype fw",
            "feols --y y --x L.x1,x2 --xtset id,t --keepif \"t>=3\"",
            "iv --y y --x x2 --endog x1 --instruments z1 --fe id",
            "xtreg --y y --x x1,x2 --ivar id",
            "newey --y y --x x1,x2 --timevar rowno --lag 1 --force",
            "prais --y y --x x1,x2 --timevar rowno",
        };
        int i = 0;
        for (const char* s : specs) {
            std::string sp(s);
            const size_t sp1 = sp.find(' ');
            const std::string args = sp.substr(0, sp1) + " " + base + sp.substr(sp1);
            const std::string tag = "same" + std::to_string(i++);
            const std::string mp = P(tag + ".mask");
            Run a = cobra(args, tag + "_off");
            Run b = cobra(args + " --esample-out " + q(mp), tag + "_on");
            check(a.rc == 0 && b.rc == 0, tag + ": both runs succeed");
            check(!a.out.contains("esample_n"), tag + ": no esample_n key without the flag");
            strip_volatile(a.out);
            strip_volatile(b.out);
            check(a.out == b.out, tag + ": the flag changes no other JSON field (" + sp + ")");
        }
    }

    fs::remove_all(g_dir, ec);
    std::printf(g_fail ? "esample_out: FAIL\n" : "esample_out: all arms pass\n");
    return g_fail;
}
