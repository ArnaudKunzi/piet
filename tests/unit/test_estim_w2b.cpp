// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// w2b_estim qreg arms — the engine half of `qreg ..., vce(robust)` and
// `qreg ... [pw=w], vce(robust)`, plus the default-VCE and q(.25) controls.
//
// Oracle: tests/eval/golden/aer_w2/w2b_estim.log (StataBE 19.5, 2026-09-19):
//   :791 qreg_pw_vce_robust  _b[x1] 2.017204446266646  _se[x1] .0854069206829948
//   :818 qreg_vce_robust     _b[x1] 2.010232206078236  _se[x1] .0805316862193467
//   :844 qreg_ctl            _b[x1] 2.010232206078236  _se[x1] .0843024787277759
//   :874 qreg_q25            _b[x1] 2.040846062804042
//
// Fixture = the do-file's: set obs 60; every `generate` there is default-typed,
// i.e. a FLOAT column, and Stata fits on the stored floats. The 7e-9 gap between
// a double-data fit and the oracle's _b[x1] is exactly that storage (the LP basis
// rows re-solved on float data reproduce 2.0102322060782356), so this test
// rebuilds the columns the way Stata stores them: each `generate` evaluated in
// double, then rounded to float. y reads the float x1/x2, as Stata's does.
//
// Tier (tests/eval/TOLERANCE.md): b and se at rtol 1e-9 (T2). The interior-point
// solver stops at a duality gap of 1e-10 (qreg_solve), which bounds b well inside
// that; the robust se is a sparsity-function quantity (a T4 mechanism) but is
// held to T2 here because it reaches it.
//
// qreg_fit_robust did not exist before this TU; the two robust checks fail on the
// old engine by not compiling, and the only robust-adjacent number the old engine
// could produce for these arms is the iid se (.0843025, 4.7% off .0805317).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/feols.h"
#include "estimators/qreg.h"
#include "inference/wildboot.h"
#include "core/error.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;

static int g_fail = 0;

static void close_to(double got, double want, double rtol, const std::string& what) {
    const double rel = std::fabs(got - want) / std::fabs(want);
    const bool ok = rel <= rtol;
    std::printf("  %-44s got %.16g want %.16g rel %.2e %s\n", what.c_str(), got, want, rel,
                ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

// w2g_ebmat.do:48-69 fixture (20 rows, 5 clusters of 4) plus an instrument g and a 0/1
// outcome yb for the CLI arms; x1/x2 are stored as Stata floats.
static const double fy[20] = {4.6612, 3.2857, 1.2851, 3.3677, 4.0168, 4.379, 4.2264, 1.4462, 0.8513, 4.3434,
                              4.5986, 3.7891, 3.8416, 3.2475, 4.3595, 6.3316, 3.1369, 1.1121, 3.55, 5.401};
static const double fx1[20] = {2.0271, 1.231, -1.0755, -1.3669, 0.9302, 2.5971, 1.3382, -0.8557, -0.624, 1.8403,
                               3.0735, 1.4155, -0.5582, 0.1903, 2.7111, 3.4576, 1.4827, -0.1738, 1.0613, 3.5251};
static const double fx2[20] = {1.4473, 0.855, 0.1427, -1.4133, -1.1047, -0.1354, 1.1798, 1.1633, 1.7998, 1.7309,
                               1.1301, -0.7789, -1.1216, -0.7956, 0.1867, 0.3045, 1.4791, 2.0992, 2.0141, 0.2051};
static const int fid[20] = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
static const int fg[20] = {2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3};
static const int fyb[20] = {0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0};

// ---- T-E1 CLI arms: `--emit-v` end to end through the cobra executable ---------------------
// add_e_v, the iv display permutation and the logit / probit emission live in apps/cmds,
// which only the cobra target links, so these arms run the binary: argv[1] when given,
// else the `cobra` built beside this test. A missing binary FAILS (never a silent skip).
using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

static std::string g_cobra;
static fs::path g_dir;

static std::string qt(const std::string& s) { return "\"" + s + "\""; }

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void fail(const std::string& msg) {
    std::printf("  <<FAIL %s\n", msg.c_str());
    g_fail = 1;
}

struct CliRun {
    int status = -1;
    ojson out;
    std::string err;
};

static CliRun run_cobra(const std::string& args, const std::string& tag) {
    const fs::path outp = g_dir / (tag + ".json"), errp = g_dir / (tag + ".err");
    std::error_code ec;
    fs::remove(outp, ec);
    std::string cmd = qt(g_cobra) + " " + args + " --no-provenance --out " + qt(outp.string()) +
                      " 2> " + qt(errp.string());
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    CliRun r;
    r.status = std::system(cmd.c_str());
    r.err = slurp(errp);
    if (fs::exists(outp)) {
        try { r.out = ojson::parse(slurp(outp)); } catch (const std::exception&) { r.out = ojson(); }
    }
    return r;
}

static std::string drop_volatile(ojson j) {
    if (j.is_object()) {
        j.erase("timing_ms");
        j.erase("time_ms");
    }
    return j.dump();
}

// The reply's coefficient order with the se each one reports (and se_iid when present).
struct ReplyCoefs {
    std::vector<std::string> names;
    std::vector<double> se, se_iid;
};

static ReplyCoefs reply_coefs(const ojson& j) {
    ReplyCoefs rc;
    if (j.contains("coefs") && j["coefs"].is_array()) {
        for (const auto& c : j["coefs"]) {
            rc.names.push_back(c["name"].get<std::string>());
            rc.se.push_back(c["se"].get<double>());
            rc.se_iid.push_back(c.contains("se_iid") ? c["se_iid"].get<double>() : std::nan(""));
        }
    } else if (j.contains("se") && j["se"].is_object()) {
        for (auto it = j["se"].begin(); it != j["se"].end(); ++it) {
            rc.names.push_back(it.key());
            rc.se.push_back(it.value().get<double>());
            rc.se_iid.push_back(std::nan(""));
        }
    }
    return rc;
}

// One family arm: the flagless reply has no e_V; the flagged reply equals it plus e_V; e_V.names
// is the reply's own coefficient order (== `want_names`); diag(e_V) == se^2 of that same reply at
// 1e-12 and e_V is symmetric. `want_sandwich` additionally requires diag != se_iid^2 (logit /
// probit robust and cluster: proves the sandwich, not the OIM bread, was posted).
static ojson emit_arm(const std::string& tag, const std::string& args,
                      const std::vector<std::string>& want_names, bool want_sandwich = false) {
    std::printf("  [%s]\n", tag.c_str());
    CliRun plain = run_cobra(args, tag + "_plain");
    CliRun flag = run_cobra(args + " --emit-v", tag + "_flag");
    if (plain.status != 0 || flag.status != 0 || !plain.out.is_object() || !flag.out.is_object()) {
        fail(tag + ": cobra exited nonzero: " + plain.err + flag.err);
        return ojson();
    }
    if (plain.out.contains("e_V")) fail(tag + ": flagless reply carries e_V");
    if (plain.out.contains("e_Vmb")) fail(tag + ": flagless reply carries e_Vmb");
    if (!flag.out.contains("e_V")) { fail(tag + ": --emit-v reply has no e_V"); return flag.out; }
    // --emit-v adds covariance keys only: e_V, and e_Vmb (e(V_modelbased)) after a
    // one-way robust/cluster fit since BUG-1043 (checked in the regress cluster arm).
    ojson stripped = flag.out;
    stripped.erase("e_V");
    stripped.erase("e_Vmb");
    if (drop_volatile(stripped) != drop_volatile(plain.out))
        fail(tag + ": --emit-v changed a key other than adding e_V / e_Vmb");

    const ojson& ev = flag.out["e_V"];
    std::vector<std::string> names;
    for (const auto& s : ev["names"]) names.push_back(s.get<std::string>());
    const ReplyCoefs rc = reply_coefs(flag.out);
    if (names != want_names) fail(tag + ": e_V.names is not the expected coefficient order");
    if (names != rc.names) fail(tag + ": e_V.names differs from the reply's coefficient order");
    const size_t k = names.size();
    if (ev["matrix"].size() != k) { fail(tag + ": e_V.matrix row count"); return flag.out; }
    for (size_t i = 0; i < k; ++i)
        if (ev["matrix"][i].size() != k) { fail(tag + ": e_V.matrix is not square"); return flag.out; }
    if (rc.se.size() != k) { fail(tag + ": reply se count != e_V size"); return flag.out; }
    double worst_sym = 0;
    for (size_t i = 0; i < k; ++i) {
        const double d = ev["matrix"][i][i].get<double>();
        close_to(d, rc.se[i] * rc.se[i], 1e-12, tag + " diag(e_V)==se^2 [" + names[i] + "]");
        for (size_t j = 0; j < i; ++j) {
            const double a = ev["matrix"][i][j].get<double>(), b = ev["matrix"][j][i].get<double>();
            worst_sym = std::max(worst_sym, std::fabs(a - b) / std::max(1e-300, std::fabs(a)));
        }
    }
    if (worst_sym > 1e-12) fail(tag + ": e_V asymmetric " + std::to_string(worst_sym));
    if (want_sandwich) {
        double moved = 0;
        for (size_t i = 0; i < k; ++i) {
            const double d = ev["matrix"][i][i].get<double>();
            moved = std::max(moved, std::fabs(d - rc.se_iid[i] * rc.se_iid[i]) / d);
        }
        if (!(moved > 1e-3)) fail(tag + ": diag(e_V) equals the iid/OIM se^2 - not the sandwich");
    }
    return flag.out;
}

static void cli_arms(const char* argv0, const char* argv1) {
    std::printf(" T-E1 CLI arms (--emit-v):\n");
    if (argv1) g_cobra = argv1;
    else {
#if defined(_WIN32)
        g_cobra = (fs::absolute(argv0).parent_path() / "cobra.exe").string();
#else
        g_cobra = (fs::absolute(argv0).parent_path() / "cobra").string();
#endif
    }
    if (!fs::exists(g_cobra)) {
        fail("cobra binary not found at " + g_cobra + " (build the cobra target or pass it as argv[1])");
        return;
    }
#if defined(_WIN32)
    _putenv_s("COBRA_OPTIONS_STRICT", "1");
#else
    setenv("COBRA_OPTIONS_STRICT", "1", 1);
#endif
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / ("cobra_emit_v_" + std::to_string(cecon::native_process_id()));
    fs::create_directories(g_dir, ec);
    const fs::path csv = g_dir / "fix.csv", hbin = g_dir / "fix.hbin";
    {
        std::ofstream f(csv, std::ios::binary);
        f << "y,x1,x2,g,id,yb,one\n";
        char buf[256];
        for (int i = 0; i < 20; ++i) {
            std::snprintf(buf, sizeof buf, "%.17g,%.17g,%.17g,%d,%d,%d,1\n", fy[i],
                          static_cast<double>(static_cast<float>(fx1[i])),
                          static_cast<double>(static_cast<float>(fx2[i])), fg[i], fid[i], fyb[i]);
            f << buf;
        }
    }
    std::string cmd = qt(g_cobra) + " convert " + qt(csv.string()) + " " + qt(hbin.string()) + " > " +
                      qt((g_dir / "cvt.txt").string()) + " 2>&1";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) { fail("cobra convert of the fixture"); return; }
    const std::string D = "--data " + qt(hbin.string());
    const std::vector<std::string> ixx = {"(intercept)", "x1", "x2"};

    // Oracle w2g_ebmat.log:350 / :365 at the engine CLI (reg y x1 x2): e1_V11 / e1_V12.
    ojson reg = emit_arm("regress iid", "feols " + D + " --y y --x x1,x2", ixx);
    if (reg.contains("e_V")) {
        close_to(reg["e_V"]["matrix"][1][1].get<double>(), 0x1.4da4eb0036878p-7, 1e-9,
                 "CLI e1_V11 (w2g_ebmat.log:350)");
        close_to(reg["e_V"]["matrix"][1][2].get<double>(), -0x1.f7d0ccaeed4b1p-10, 1e-9,
                 "CLI e1_V12 (w2g_ebmat.log:365)");
    }
    // Cluster: the sandwich the reported se came from, so V moved off the iid matrix.
    ojson clu = emit_arm("regress cluster", "feols " + D + " --y y --x x1,x2 --vcov cluster --cluster id", ixx);
    if (reg.contains("e_V") && clu.contains("e_V") &&
        std::fabs(clu["e_V"]["matrix"][1][1].get<double>() - reg["e_V"]["matrix"][1][1].get<double>()) <
            1e-9 * reg["e_V"]["matrix"][1][1].get<double>())
        fail("regress cluster: e_V equals the iid matrix");
    // BUG-1043: e(V_modelbased) after vce(cluster) is the bread inv(X'X), so e_Vmb * sigma2
    // is the iid fit's e_V (StataBE: e(V_modelbased) == V / e(rmse)^2 bit for bit).
    if (!clu.contains("e_Vmb")) {
        fail("regress cluster: --emit-v reply has no e_Vmb (BUG-1043)");
    } else if (!reg.contains("e_V") || !reg.contains("sigma2")) {
        fail("regress iid: no e_V / sigma2 to check e_Vmb against");
    } else {
        const double s2 = reg["sigma2"].get<double>();
        for (size_t i = 0; i < ixx.size(); ++i)
            for (size_t j = 0; j < ixx.size(); ++j)
                close_to(clu["e_Vmb"]["matrix"][i][j].get<double>() * s2,
                         reg["e_V"]["matrix"][i][j].get<double>(), 1e-12,
                         "e_Vmb*sigma2 == iid e_V [" + std::to_string(i) + "," + std::to_string(j) + "]");
    }
    emit_arm("regress hc1", "feols " + D + " --y y --x x1,x2 --vcov hc1", ixx);
    emit_arm("reghdfe fe", "feols " + D + " --y y --x x1 --fe id", {"x1", "_cons"});

    // IV: x1 exogenous, x2 endogenous -> display order puts x2 before x1, unlike the fit's.
    const std::vector<std::string> ivn = {"(intercept)", "x2", "x1"};
    emit_arm("iv iid", "iv " + D + " --y y --x x1 --endog x2 --instruments g", ivn);
    emit_arm("iv cluster", "iv " + D + " --y y --x x1 --endog x2 --instruments g --vcov cluster --cluster id", ivn);

    // logit / probit: the OIM bread without --vce, the sandwich under robust / cluster.
    const std::vector<std::string> lxx = {"x1", "x2", "(intercept)"};
    emit_arm("logit oim", "logit " + D + " --y yb --x x1,x2", lxx);
    emit_arm("logit robust", "logit " + D + " --y yb --x x1,x2 --vce robust", lxx, true);
    emit_arm("logit cluster", "logit " + D + " --y yb --x x1,x2 --vce cluster --cluster id", lxx, true);
    emit_arm("probit oim", "probit " + D + " --y yb --x x1,x2", lxx);
    emit_arm("probit robust", "probit " + D + " --y yb --x x1,x2 --vce robust", lxx, true);

    // FU-859: fepois (PIET's poisson) under --emit-v, the same identities (diag == se^2, symmetric,
    // flagless reply unchanged), and a covariance with real off-diagonals -- the diag(se^2) PIET
    // used to synthesise answered every one of them with 0 (w2v_estpost_ematrix.log
    // eb_po_rob_V_shape / eb_po_cl_V_shape: Stata's V[1,2], V[1,3], V[2,3] are all nonzero).
    for (const char* vce : {"", " --vcov hc1", " --vcov cluster --cluster id"}) {
        const std::string tag = std::string("poisson") + (*vce ? vce : " iid");
        ojson po = emit_arm(tag, "fepois " + D + " --y g --x x1,x2" + vce, ixx);
        if (po.contains("e_V")) {
            const ojson& M = po["e_V"]["matrix"];
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j)
                    if (!(std::fabs(M[i][j].get<double>()) > 0))
                        fail(tag + ": e_V off-diagonal (" + std::to_string(i) + "," + std::to_string(j) +
                             ") is 0");
        }
    }
    {
        // Absorbed fits now retain the complete covariance of the recovered
        // constant, including slope cross-covariances. emit_arm checks its
        // dimensions, symmetry and every diagonal against the reported SE.
        for (const char* vce : {"", " --vcov hc1", " --vcov cluster --cluster id"}) {
            const std::string tag = std::string("ppml absorbed") + (*vce ? vce : " iid");
            const ojson r = emit_arm(tag, "fepois " + D + " --y g --x x1,x2 --fe id" + vce,
                                     {"x1", "x2", "_cons"});
            if (r.contains("e_V") &&
                !(std::fabs(r["e_V"]["matrix"][0][2].get<double>()) > 0))
                fail(tag + ": missing recovered-constant cross-covariance");
        }
    }

    // Refusals keep a nonzero status and name the flag: --svy and a multi-outcome batch have no e_V slot.
    {
        CliRun r = run_cobra("feols " + D + " --y y,yb --x x1 --emit-v", "ref_batch");
        if (r.status == 0 || r.out.contains("e_V") || r.err.find("--emit-v") == std::string::npos)
            fail("feols --y a,b --emit-v must refuse naming --emit-v");
        CliRun r2 = run_cobra("iv " + D + " --y y,yb --x x1 --endog x2 --instruments g --emit-v", "ref_ivbatch");
        if (r2.status == 0 || r2.out.contains("e_V") || r2.err.find("--emit-v") == std::string::npos)
            fail("iv --y a,b --emit-v must refuse naming --emit-v");
    }

    // svy without strata(): `svyset id [pweight=w]` is ONE stratum (w2t_boottest_family.log:2455
    // H1, "Number of strata = 1"), so the reply equals the one with a constant --strata column
    // apart from the stratum's name, and the design df is #PSU - 1 = 4. The old engine refused
    // a missing --strata outright.
    {
        const std::string svy = "feols " + D + " --y y --x x1,x2 --weight g --wtype pw --svy --psu id";
        CliRun none = run_cobra(svy, "svy_nostrata");
        CliRun cst = run_cobra(svy + " --strata one", "svy_onestratum");
        if (none.status != 0 || !none.out.is_object() || !none.out.contains("svy")) {
            fail("feols --svy without --strata must fit as one stratum: " + none.err);
        } else if (cst.status != 0 || !cst.out.is_object()) {
            fail("feols --svy --strata one: " + cst.err);
        } else {
            if (none.out["svy"].value("n_strata", -1) != 1) fail("svy no-strata: n_strata != 1");
            if (none.out["svy"].value("design_df", -1) != 4) fail("svy no-strata: design_df != 4");
            ojson a = none.out, b = cst.out;
            a["svy"].erase("strata_var");
            b["svy"].erase("strata_var");
            if (drop_volatile(a) != drop_volatile(b))
                fail("svy no-strata reply differs from the constant-stratum reply");
        }
    }
    fs::remove_all(g_dir, ec);
}

int main(int argc, char** argv) {
    std::printf("test_estim_w2b:\n");
    const int64_t n = 60;
    const int k = 3;
    std::vector<double> X(static_cast<size_t>(n) * k), y(n), w(n);
    for (int64_t i = 0; i < n; ++i) {
        const double obs = static_cast<double>(i + 1);
        const double id = std::ceil(obs / 6.0);
        const double x1 = static_cast<float>(std::fmod(obs * 7, 11) + std::fmod(obs, 5) / 10);
        const double x2 = static_cast<float>(std::fmod(obs * 3, 5) + std::fmod(obs * 13, 7) / 10);
        y[i] = static_cast<float>(1 + 2 * x1 - x2 + id / 3 + std::fmod(obs * 11, 17) / 5);
        w[i] = 1 + std::fmod(obs, 4);
        X[i] = x1;
        X[n + i] = x2;
        X[2 * n + i] = 1.0;
    }
    const std::vector<std::string> names = {"x1", "x2", "(intercept)"};
    const double kT2 = 1e-9;

    // Controls: the default vce(iid) path, unchanged by this TU.
    QregResult ctl = qreg_fit(X.data(), n, k, y.data(), 0.5, names, 95.0);
    close_to(ctl.beta[0], 2.010232206078236, kT2, "qreg_ctl _b[x1]");
    close_to(ctl.se[0], .0843024787277759, kT2, "qreg_ctl _se[x1]");
    QregResult q25 = qreg_fit(X.data(), n, k, y.data(), 0.25, names, 95.0);
    close_to(q25.beta[0], 2.040846062804042, kT2, "qreg_q25 _b[x1]");

    // vce(robust), unweighted: same b as the iid fit, Hendricks-Koenker se.
    QregResult rob = qreg_fit_robust(X.data(), n, k, y.data(), nullptr, 0.5, names, 95.0);
    if (!rob.converged) { std::printf("  qreg_vce_robust <<FAIL not converged\n"); g_fail = 1; }
    close_to(rob.beta[0], 2.010232206078236, kT2, "qreg_vce_robust _b[x1]");
    close_to(rob.se[0], .0805316862193467, kT2, "qreg_vce_robust _se[x1]");
    if (rob.n_crossing != 0) { std::printf("  qreg_vce_robust <<FAIL crossing %lld\n", (long long)rob.n_crossing); g_fail = 1; }
    // e(df_r) 57 = N - k: the t(57) the oracle's CI uses (w2b_estim.log:810
    // 2.010232 - 1.84897 = .161262 = invttail(57,.025) * .0805317).
    if (rob.dof_resid != 57.0) { std::printf("  qreg_vce_robust <<FAIL df_r %g\n", rob.dof_resid); g_fail = 1; }

    // [pw=w] vce(robust): b of the weighted check loss, h from N = 60 observations.
    QregResult pw = qreg_fit_robust(X.data(), n, k, y.data(), w.data(), 0.5, names, 95.0);
    if (!pw.converged) { std::printf("  qreg_pw_vce_robust <<FAIL not converged\n"); g_fail = 1; }
    close_to(pw.beta[0], 2.017204446266646, kT2, "qreg_pw_vce_robust _b[x1]");
    close_to(pw.se[0], .0854069206829948, kT2, "qreg_pw_vce_robust _se[x1]");
    // "Min sum of deviations 79.88322" (w2b_estim.log:777) is the weighted objective.
    close_to(pw.sum_adev, 79.88322, 1e-6, "qreg_pw_vce_robust sum_adev (7 digits shown)");
    if (pw.dof_resid != 57.0) { std::printf("  qreg_pw_vce_robust <<FAIL df_r %g\n", pw.dof_resid); g_fail = 1; }

    // ---- T-E1: the full fitted e(V) behind `cobra feols --emit-v` -------------------------
    // FeolsResult::vcov_matrix is what add_e_v (apps/cmds/common.cpp) emits, so the
    // identities the reply must satisfy are checked on it here. Oracle: w2g_ebmat.log:350
    // e1_V11 = +1.4da4eb0036878X-007 and :365 e1_V12 = -1.f7d0ccaeed4b1X-00a after
    // `regress y x1 x2` on the 20-row `input double` fixture of w2g_ebmat.do:48-69. Stata's
    // V[1,1] is x1's variance and V[1,2] cov(x1,x2); the engine keeps (intercept) first
    // (SPEC section B.1), so those are engine cells (1,1) and (1,2).
    {
        const int64_t m = 20;
        ModelData md;
        md.has_intercept = true;
        md.dm.n = m; md.dm.k = 3;
        md.dm.X.assign(static_cast<size_t>(m) * 3, 0.0);
        md.dm.y.assign(fy, fy + m);
        md.dm.xnames = {"(intercept)", "x1", "x2"};
        for (int64_t i = 0; i < m; ++i) {
            md.dm.X[i] = 1.0;
            md.dm.X[m + i] = static_cast<float>(fx1[i]);  // float replica, see above
            md.dm.X[2 * m + i] = static_cast<float>(fx2[i]);
        }
        Factor cl;
        cl.name = "id"; cl.n_levels = 5; cl.count.assign(5, 4.0);
        for (int64_t i = 0; i < m; ++i) cl.codes.push_back(fid[i] - 1);
        md.cluster.push_back(cl);

        auto check_v = [&](const FeolsResult& r, const char* tag) {
            const int kk = r.k;
            if (static_cast<int>(r.vcov_matrix.size()) != kk * kk) {
                std::printf("  %s <<FAIL vcov_matrix size %zu\n", tag, r.vcov_matrix.size());
                g_fail = 1;
                return;
            }
            double worst_sym = 0;
            for (int a = 0; a < kk; ++a) {
                close_to(r.vcov_matrix[a + a * kk], r.se[a] * r.se[a], 1e-12,
                         std::string(tag) + " diag(V)==se^2 [" + std::to_string(a) + "]");
                for (int b = 0; b < a; ++b)
                    worst_sym = std::max(worst_sym, std::fabs(r.vcov_matrix[a + b * kk] - r.vcov_matrix[b + a * kk]) /
                                                        std::max(1e-300, std::fabs(r.vcov_matrix[a + b * kk])));
            }
            if (worst_sym > 1e-12) { std::printf("  %s <<FAIL asymmetric %.2e\n", tag, worst_sym); g_fail = 1; }
        };
        FeolsResult iid = feols_fit(md, Vcov::IID);
        check_v(iid, "e_V regress iid");
        close_to(iid.vcov_matrix[1 + 1 * 3], 0x1.4da4eb0036878p-7, kT2, "e1_V11 (w2g_ebmat.log:350)");
        close_to(iid.vcov_matrix[1 + 2 * 3], -0x1.f7d0ccaeed4b1p-10, kT2, "e1_V12 (w2g_ebmat.log:365)");
        FeolsResult clu = feols_fit(md, Vcov::Cluster);
        check_v(clu, "e_V regress cluster");
        // The sandwich is not the iid matrix: a diag taken from the iid bread would pass the
        // identity above only for iid, so pin that cluster really moved V[1,1].
        if (std::fabs(clu.vcov_matrix[1 + 1 * 3] - iid.vcov_matrix[1 + 1 * 3]) < 1e-9 * iid.vcov_matrix[1 + 1 * 3]) {
            std::printf("  e_V regress cluster <<FAIL cluster V equals iid V\n");
            g_fail = 1;
        }
    }

    // ML score bootstrap: fixed deterministic w2t F1-F5 fixture. These z's
    // are null-imposed score statistics, independent of the Philox p draws.
    // An unrestricted Wald/OLS substitution fails these numerical anchors.
    {
        constexpr int64_t n = 180;
        constexpr int k = 3;
        std::vector<double> X(n * k), binary(n), count(n), continuous(n);
        Factor cluster; cluster.n_levels = 30; cluster.codes.resize(n);
        for (int row = 0; row < n; ++row) {
            const int i = row + 1, id = (i + 5) / 6;
            const double x1 = (i * 7 % 11) + (i % 5) / 8.0;
            const double z1 = (i * 5 % 13) + x1 / 2 + (i * 17 % 19) / 16.0;
            const double x2 = .5 * z1 + (i * 3 % 5) + (i * 13 % 7) / 8.0 + (i * 23 % 29) / 32.0;
            const double y = 1 + 2*x1 - x2 + id/4.0 + (i*11%17)/4.0 +
                ((i*37%41)/64.0 + (i*53%59)/64.0 + (i*29%31)/64.0 - 1)*3;
            const double yw = y - 31*x1/16;
            X[row] = x1; X[n + row] = x2; X[2*n + row] = 1;
            binary[row] = yw > 0 ? 1 : 0;
            count[row] = std::trunc((yw + 11)/2);
            continuous[row] = yw;
            cluster.codes[row] = id - 1;
        }
        const char* models[] = {"logit", "probit", "poisson", "glm-poisson", "glm-gaussian"};
        const double want[] = {.7252469823312174, .7667790745332531, 1.357027901426593,
                               1.357027905221908, 1.021453477161604};
        for (int m = 0; m < 5; ++m) {
            const auto& outcome = m < 2 ? binary : m < 4 ? count : continuous;
            const auto got = wild_score_bootstrap(X, n, k, outcome, cluster, 0, "x1", models[m], 0, 99, 1,
                                                  BootWeights::Rademacher);
            close_to(got.t0, want[m], 1e-9, std::string("score-z ") + models[m]);
            const auto again = wild_score_bootstrap(X, n, k, outcome, cluster, 0, "x1", models[m], 0, 99, 1,
                                                    BootWeights::Rademacher);
            if (got.tstars != again.tstars || got.reps != 99 || got.p_symmetric < 0 || got.p_symmetric > 1) {
                std::printf("  score bootstrap repeatability/grid <<FAIL\n"); g_fail = 1;
            }
        }
        bool rejected = false;
        try { (void)wild_score_bootstrap(X, n, k, binary, cluster, 0, "x1", "logit", 1, 99, 1,
                                         BootWeights::Rademacher); }
        catch (const EngineError&) { rejected = true; }
        if (!rejected) { std::printf("  score bootstrap nonzero null refusal <<FAIL\n"); g_fail = 1; }
    }

    cli_arms(argv[0], argc > 1 ? argv[1] : nullptr);

    std::printf("%s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail;
}
