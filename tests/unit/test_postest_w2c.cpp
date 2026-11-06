// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// w2c_postest engine numerics: the shared post-estimation name resolver
// (src/estimators/postest_names.h) and linear grammar driving test / lincom /
// nlcom on factor-level coefficients, against StataBE 19.5
// (tests/eval/golden/aer_w2/w2c_postest.log). Arms: test_2c_eq_avg_base (the
// omitted base level `0.c` is a zero coefficient with zero variance),
// testparm_i24_g, lincom_bexpr (`_b[2.c] + _b[x1]`), nlcom_multi_post (`A`, `B`).
//
// FU-131 (cli_noconstant_*): the engine's post-estimation refits honour the fit's
// `noconstant` (--no-intercept) -- lincom/test/nlcom/margins/predict through the
// cobra CLI on w2p_estim_ev.do's double-stored frame, against
// tests/eval/golden/aer_w2/w2p_estim_ev.log section G. The cobra binary is the one
// built next to this test; an engine without the flag rejects it (exit 3).
//
// FU-426 / FU-435 (cli_maxopts_arms): the logit/probit refits take the fit's
// maximize options (--tolerance/--ltolerance/--nrtolerance/--iterate), so after the
// tight fits of tests/eval/golden/aer_w2/w2e_probes.log (lp_tight,
// lp_probit_tight_ctl) lincom/test/margins answer from the posted b and V; an
// engine without the flags on those verbs rejects them (exit 3).
//
// Fixture: the .do's own generate lines, stored as FLOAT the way `generate` stores
// them (double arithmetic on the stored floats, then a cast to float) — TOLERANCE.md
// §3.3. Confirmed before any comparison against the oracle's grouped-notation
// control arm expanded_ic_ig_x1_ctl (`regress y i.c i.g x1`: N 60, e(r2)
// .956489519262575, _b[2.c] 1.206806960142391).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/model.h"
#include "data/frame.h"
#include "estimators/feols.h"
#include "estimators/nlcom.h"
#include "estimators/posthest.h"
#include "estimators/postest_names.h"
#include "inference/wildboot.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what, double got, double want) {
    std::printf("  %-44s got=%.16g want=%.16g %s\n", what, got, want, ok ? "" : "<<FAIL");
    if (!ok) g_fail = 1;
}
bool rel(double got, double want, double rtol) {
    return std::isfinite(got) && std::fabs(got - want) <= rtol * std::fabs(want);
}

double smod(double a, double b) { return a - b * std::floor(a / b); }
double f32(double v) { return static_cast<float>(v); }

Dataset make_fixture(const std::string& path) {
    std::vector<double> id, x1, x2, c, g, y;
    for (int n = 1; n <= 60; ++n) {
        const double i = f32(std::ceil(n / 6.0));
        const double a = f32(smod(n * 7, 11) + smod(n, 5) / 10.0);
        const double b = f32(smod(n * 3, 5) + smod(n * 13, 7) / 10.0);
        const double cc = smod(n, 4), gg = smod(n, 5);
        id.push_back(i); x1.push_back(a); x2.push_back(b); c.push_back(cc); g.push_back(gg);
        y.push_back(f32(1 + 2 * a - b + 0.7 * cc + 0.4 * gg + i / 3.0 + smod(n * 11, 17) / 5.0));
    }
    Frame f;
    f.set_n_rows_hint(60);
    f.append_f64_column("id", id);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("c", c);
    f.append_f64_column("g", g);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

postest::FactorLevels levels_of(const Dataset& ds, const ModelData& md,
                                const std::vector<std::string>& vars) {
    postest::FactorLevels out;
    for (const auto& v : vars) {
        const double* col = ds.col(v).f64();
        std::vector<double> lv;
        for (int64_t r : md.keep) lv.push_back(col[r]);
        std::sort(lv.begin(), lv.end());
        lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
        out[v] = lv;
    }
    return out;
}

double f_test(const FeolsResult& r, const std::vector<postest::LinearForm>& rows, int* J) {
    const int k = static_cast<int>(r.names.size());
    *J = static_cast<int>(rows.size());
    std::vector<double> R(static_cast<size_t>(*J) * k), q(static_cast<size_t>(*J));
    for (int i = 0; i < *J; ++i) {
        for (int j = 0; j < k; ++j) R[static_cast<size_t>(i) * k + j] = rows[i].w[j];
        q[i] = -rows[i].konst;
    }
    WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(), k, R.data(), q.data(), *J);
    return w.ok ? w.F : std::nan("");
}

// The w2_esample.do fixture (tests/eval/golden/aer_w2/w2_esample.do:12-29): 61
// rows, id = ceil(_n/6) with row 61 the singleton id 11, t = mod(_n-1,6)+1, y
// missing in row 7 and x1 in row 13. Stored as the .do stores them (double).
Dataset make_esample_fixture(const std::string& path) {
    std::vector<double> id, t, x1, x2, y;
    for (int n = 1; n <= 61; ++n) {
        const double a = smod(n * 7, 11) + smod(n, 5) / 10.0;
        const double b = smod(n * 3, 5) + smod(n * 13, 7) / 10.0;
        const double i = n == 61 ? 11 : std::ceil(n / 6.0);
        id.push_back(i);
        t.push_back(n == 61 ? 1 : smod(n - 1, 6) + 1);
        x1.push_back(n == 13 ? std::nan("") : a);
        x2.push_back(b);
        y.push_back(n == 7 ? std::nan("") : 1 + 2 * a - b + i / 3.0 + smod(n * 11, 17) / 5.0);
    }
    Frame f;
    f.set_n_rows_hint(61);
    f.append_f64_column("id", id);
    f.append_f64_column("t", t);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

// ---- FU-131: the CLI refit after a no-constant fit -------------------------
std::string g_cobra;

// w2p_estim_ev.do:15-22 (x1, x2, y double; d = (y > 12)).
void write_w2p_fixture(const std::string& path) {
    std::vector<double> x1, x2, y, d;
    for (int n = 1; n <= 60; ++n) {
        const double id = std::ceil(n / 6.0);
        const double a = smod(n * 7, 11) + smod(n, 5) / 10;
        const double b = smod(n * 3, 5) + smod(n * 13, 7) / 10;
        const double yy = 1 + 2 * a - b + id / 3 + smod(n * 11, 17) / 5;
        x1.push_back(a); x2.push_back(b); y.push_back(yy); d.push_back(yy > 12 ? 1.0 : 0.0);
    }
    Frame f;
    f.set_n_rows_hint(60);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.append_f64_column("d", d);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
}

// `cobra <args> --no-provenance`, stdout parsed (null JSON on a non-zero exit).
nlohmann::json cli(const std::string& args, const std::string& tag) {
    const std::string outp = native_temp_path("cecon_test_postest_w2c_" + tag + ".json");
    const std::string errp = native_temp_path("cecon_test_postest_w2c_" + tag + ".err");
    native_unlink(outp);
    std::string cmd = "\"" + g_cobra + "\" " + args + " --no-provenance > \"" + outp + "\" 2> \"" +
                      errp + "\"";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());
    std::ifstream in(outp, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    native_unlink(outp);
    std::ifstream ein(errp, std::ios::binary);
    std::stringstream es;
    es << ein.rdbuf();
    native_unlink(errp);
    if (rc != 0) {
        std::printf("  cobra %s -> exit %d: %s%s\n", args.c_str(), rc, ss.str().c_str(), es.str().c_str());
        return nlohmann::json();
    }
    try { return nlohmann::json::parse(ss.str()); } catch (const std::exception&) { return nlohmann::json(); }
}

double jnum(const nlohmann::json& j, const std::vector<std::string>& path, size_t idx = 0) {
    const nlohmann::json* cur = &j;
    for (const auto& k : path) {
        if (!cur->is_object() || !cur->contains(k)) return std::nan("");
        cur = &(*cur)[k];
        if (cur->is_array()) {
            if (cur->size() <= idx) return std::nan("");
            cur = &(*cur)[idx];
        }
    }
    return cur->is_number() ? cur->get<double>() : std::nan("");
}

void cli_noconstant_arms() {
    std::printf(" FU-131 noconstant refit (cobra %s):\n", g_cobra.c_str());
    if (g_cobra.empty() || !std::filesystem::exists(g_cobra)) {
        std::printf("  cobra binary not found next to this test <<FAIL\n");
        g_fail = 1;
        return;
    }
    const std::string hb = native_temp_path("cecon_test_postest_w2c_w2p.hbin");
    write_w2p_fixture(hb);
    const std::string base = "--data \"" + hb + "\" --y y --x x1,x2 ";
    const std::string nc = base + "--no-intercept ";

    nlohmann::json j = cli("lincom " + nc + "--exp x1", "lin");
    expect(rel(jnum(j, {"coefs", "b"}), 2.356553697480766, 1e-9), "G_nc_lincom_x1 estimate",
           jnum(j, {"coefs", "b"}), 2.356553697480766);
    expect(rel(jnum(j, {"coefs", "se"}), .0783607746432686, 1e-9), "G_nc_lincom_x1 se",
           jnum(j, {"coefs", "se"}), .0783607746432686);
    expect(jnum(j, {"df"}) == 58, "G_nc_lincom_x1 df (N - 2)", jnum(j, {"df"}), 58);
    j = cli("lincom " + nc + "--exp \"x1 + x2\"", "sum");
    expect(rel(jnum(j, {"coefs", "b"}), 2.133177809016356, 1e-9), "G_nc_lincom_sum estimate",
           jnum(j, {"coefs", "b"}), 2.133177809016356);
    expect(rel(jnum(j, {"coefs", "se"}), .1283009449957656, 1e-9), "G_nc_lincom_sum se",
           jnum(j, {"coefs", "se"}), .1283009449957656);
    j = cli("test " + nc + "--joint x1", "test");
    expect(rel(jnum(j, {"F"}), 904.3930979008784, 1e-9), "G_nc_test F", jnum(j, {"F"}),
           904.3930979008784);
    expect(jnum(j, {"df2"}) == 58, "G_nc_test df_r", jnum(j, {"df2"}), 58);
    j = cli("nlcom " + nc + "--expr \"2*_b[x1]\"", "nl");
    const double nlse = jnum(j, {"coefs", "se"});
    expect(rel(jnum(j, {"coefs", "b"}), 4.713107394961533, 1e-9), "G_nc_nlcom b",
           jnum(j, {"coefs", "b"}), 4.713107394961533);
    expect(rel(nlse * nlse, .0245616440111472, 1e-9), "G_nc_nlcom V", nlse * nlse, .0245616440111472);
    j = cli("margins " + nc + "--at x1=5", "mat");
    const double mse = jnum(j, {"margins", "se"});
    expect(rel(jnum(j, {"margins", "margin"}), 11.26677018505104, 1e-9), "G_nc_margins_at b",
           jnum(j, {"margins", "margin"}), 11.26677018505104);
    expect(rel(mse * mse, .0790691558659889, 1e-9), "G_nc_margins_at V", mse * mse, .0790691558659889);
    {
        const std::string out = native_temp_path("cecon_test_postest_w2c_pred.hbin");
        native_unlink(out);
        j = cli("predict " + nc + "--xb xb_nc --resid res_nc --out \"" + out + "\"", "pred");
        double sxb = 0, sxb2 = 0, sres = 0;
        int n = 0;
        if (!j.is_null() && std::filesystem::exists(out)) {
            Dataset p = Dataset::open(out);
            const double* xb = p.col("xb_nc").f64();
            const double* rs = p.col("res_nc").f64();
            for (int64_t i = 0; i < p.n_rows(); ++i) {
                if (std::isnan(xb[i])) continue;
                ++n; sxb += xb[i]; sxb2 += xb[i] * xb[i]; sres += rs[i];
            }
        }
        const double mean = n ? sxb / n : std::nan("");
        const double sd = n > 1 ? std::sqrt((sxb2 - n * mean * mean) / (n - 1)) : std::nan("");
        expect(n == 60 && rel(mean, 11.85590860942123, 1e-9), "G_nc_predict_xb mean", mean,
               11.85590860942123);
        expect(rel(sd, 7.468862389658671, 1e-9), "G_nc_predict_xb sd", sd, 7.468862389658671);
        expect(n == 60 && rel(sres / n, .8174247239120984, 1e-9), "G_nc_predict_res mean",
               n ? sres / n : std::nan(""), .8174247239120984);
        native_unlink(out);
    }
    // Control: no flag is the with-constant refit, as before (G_c_lincom_x1).
    j = cli("lincom " + base + "--exp x1", "ctl");
    expect(rel(jnum(j, {"coefs", "b"}), 1.959601560760043, 1e-9), "G_c_lincom_x1 control",
           jnum(j, {"coefs", "b"}), 1.959601560760043);

    // probit, noconstant: the refit IS the fit (1e-12), and the fit sits on the oracle (T3).
    const std::string pnc = "--data \"" + hb + "\" --y d --x x1,x2 --no-intercept ";
    nlohmann::json fit = cli("probit " + pnc, "pfit");
    const double fb = jnum(fit, {"coefs", "beta"}), fse = jnum(fit, {"coefs", "se"});
    j = cli("lincom " + pnc + "--model probit --exp x1", "plin");
    expect(rel(jnum(j, {"coefs", "b"}), fb, 1e-12) && rel(jnum(j, {"coefs", "se"}), fse, 1e-12),
           "G_pnc_lincom_x1 = the fit's _b[x1] (and _se)", jnum(j, {"coefs", "b"}), fb);
    expect(rel(jnum(j, {"coefs", "b"}), .4484790061399406, 1e-7) &&
               rel(jnum(j, {"coefs", "se"}), .1036892925039664, 1e-7),
           "G_pnc_lincom_x1 vs oracle (T3)", jnum(j, {"coefs", "b"}), .4484790061399406);
    j = cli("margins " + pnc + "--model probit --dydx x1", "pmarg");
    const double pdse = jnum(j, {"coefs", "se"});
    expect(rel(jnum(j, {"coefs", "dydx"}), .0826720054343255, 1e-7), "G_pnc_margins_dydx b (T3)",
           jnum(j, {"coefs", "dydx"}), .0826720054343255);
    expect(rel(pdse * pdse, .0000196420060519, 2e-7), "G_pnc_margins_dydx V (T3)", pdse * pdse,
           .0000196420060519);
    // The poisson refit has no no-constant form: refused, never the with-constant answer.
    j = cli("lincom --data \"" + hb + "\" --y y --x x1,x2 --no-intercept --model poisson --exp x1",
            "pois");
    expect(j.is_null(), "poisson --no-intercept refused", j.is_null() ? 1 : 0, 1);
    native_unlink(hb);
}

// ---- FU-426 / FU-435: the refit carries the fit's maximize options ----------
//
// w2e_probes.do:41-55's fixture (make_fixture's float-stored columns plus
// `generate byte d = (y > 12)`), fitted `logit/probit d x1 x2 i.c, tolerance(1e-12)
// ltolerance(1e-15) nrtolerance(1e-12) iterate(300)`. Oracle: w2e_probes.log:249
// lp_tight (_b[x1] +1.0c0301ffafbfeX+003, e(ll) -1.77239f16196c7X+001, e(ic) 10,
// `test 2.c` r(chi2) +1.4dfd97e5fa7aaX-008) and :412 lp_probit_tight_ctl
// (+1.2a415c2c689b2X+002, -1.6df18b771fe08X+001, 9, +1.7283a7951b3fbX-009). The
// default-tolerance controls are :141 lp_default and :358 lp_probit_ctl.
// The fit has completely determined rows (logit e(N_cds) 27 / e(N_cdf) 9, probit
// 32 / 13), which the posted V drops (logit.h rule 4): the refit's se must be the
// posted _se (FU-435).
void write_w2e_fixture(const std::string& path) {
    std::vector<double> x1, x2, c, d;
    for (int n = 1; n <= 60; ++n) {
        const double i = f32(std::ceil(n / 6.0));
        const double a = f32(smod(n * 7, 11) + smod(n, 5) / 10.0);
        const double b = f32(smod(n * 3, 5) + smod(n * 13, 7) / 10.0);
        const double cc = smod(n, 4), gg = smod(n, 5);
        const double yy = f32(1 + 2 * a - b + 0.7 * cc + 0.4 * gg + i / 3.0 + smod(n * 11, 17) / 5.0);
        x1.push_back(a); x2.push_back(b); c.push_back(cc); d.push_back(yy > 12 ? 1.0 : 0.0);
    }
    Frame f;
    f.set_n_rows_hint(60);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("c", c);
    f.append_f64_column("d", d);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
}

// coefs[] row `name`, field `key`, from a cobra reply (NaN when absent).
double coef_field(const nlohmann::json& j, const std::string& name, const char* key) {
    if (!j.is_object() || !j.contains("coefs") || !j["coefs"].is_array()) return std::nan("");
    for (const auto& r : j["coefs"])
        if (r.is_object() && r.value("name", std::string()) == name && r.contains(key) && r[key].is_number())
            return r[key].get<double>();
    return std::nan("");
}

void cli_maxopts_arms() {
    std::printf(" FU-426 maximize options on the refit (cobra %s):\n", g_cobra.c_str());
    if (g_cobra.empty() || !std::filesystem::exists(g_cobra)) {
        std::printf("  cobra binary not found next to this test <<FAIL\n");
        g_fail = 1;
        return;
    }
    const std::string hb = native_temp_path("cecon_test_postest_w2c_w2e.hbin");
    write_w2e_fixture(hb);
    const std::string base = "--data \"" + hb + "\" --y d --x x1,x2,i.c ";
    const std::string tight =
        " --tolerance 1e-12 --ltolerance 1e-15 --nrtolerance 1e-12 --iterate 300";
    struct Arm {
        const char* model;
        double b, ll, chi2_2c, ctl_chi2_2c;
        int ic;
    };
    for (const Arm& A : {Arm{"logit", 0x1.0c0301ffafbfep+3, -0x1.77239f16196c7p+1, 0x1.4dfd97e5fa7aap-8,
                             0x1.4dfdeed2ce2a4p-8, 10},
                         Arm{"probit", 0x1.2a415c2c689b2p+2, -0x1.6df18b771fe08p+1, 0x1.7283a7951b3fbp-9,
                             0x1.7283a81518e54p-9, 9}}) {
        const std::string m = A.model, tag = m.substr(0, 1);
        const std::string fitarg = base + "--model " + m + " ";
        nlohmann::json fit = cli(m + " " + base + tight, tag + "fit");
        const double fb = coef_field(fit, "x1", "beta"), fse = coef_field(fit, "x1", "se");
        expect(rel(fb, A.b, 1e-12) && fit.value("ic", -1) == A.ic && rel(jnum(fit, {"ll"}), A.ll, 1e-12),
               (m + " tight fit _b[x1]/e(ic)/e(ll) (lp_tight)").c_str(), fb, A.b);
        const bool echoed = fit.is_object() && fit.contains("ml_max_argv") &&
                            fit["ml_max_argv"] == nlohmann::json::array({"--tolerance", "1e-12", "--ltolerance",
                                                                         "1e-15", "--nrtolerance", "1e-12",
                                                                         "--iterate", "300"});
        expect(echoed, (m + " fit echoes ml_max_argv").c_str(), echoed ? 1 : 0, 1);
        // lincom x1 after the tight fit: the posted _b[x1] and _se[x1], bit for bit.
        nlohmann::json j = cli("lincom " + fitarg + "--exp x1" + tight, tag + "lin");
        expect(jnum(j, {"coefs", "b"}) == fb && jnum(j, {"coefs", "se"}) == fse,
               (m + " lincom x1 == posted _b[x1] (bit)").c_str(), jnum(j, {"coefs", "b"}), fb);
        // FU-435: the posted V drops the completely determined rows; so does the refit's.
        const double ndet = jnum(fit, {"N_cds"}) + jnum(fit, {"N_cdf"});
        expect(ndet > 0 && rel(jnum(j, {"coefs", "se"}), fse, 1e-12),
               (m + " lincom x1 se == posted _se[x1], determined rows").c_str(), jnum(j, {"coefs", "se"}), fse);
        j = cli("test " + fitarg + "--joint x1" + tight, tag + "tx1");
        expect(rel(jnum(j, {"chi2"}), (fb / fse) * (fb / fse), 1e-10),
               (m + " test x1 chi2 == (_b/_se)^2").c_str(), jnum(j, {"chi2"}), (fb / fse) * (fb / fse));
        j = cli("test " + fitarg + "--joint 2.c" + tight, tag + "t2c");
        expect(rel(jnum(j, {"chi2"}), A.chi2_2c, 1e-9), (m + " test 2.c r(chi2) (lp_tight)").c_str(),
               jnum(j, {"chi2"}), A.chi2_2c);
        // margins, dydx(x1): the AME of the POSTED b, b_x1 * mean f(x'b) over the
        // design rows (f the logistic or normal density).
        double ame = 0.0;
        {
            Dataset ds = Dataset::open(hb);
            const double* cx1 = ds.col("x1").f64();
            const double* cx2 = ds.col("x2").f64();
            const double* cc = ds.col("c").f64();
            const double b2 = coef_field(fit, "x2", "beta"), b0 = coef_field(fit, "(intercept)", "beta");
            const double bc[4] = {0.0, coef_field(fit, "1.c", "beta"), coef_field(fit, "2.c", "beta"),
                                  coef_field(fit, "3.c", "beta")};
            for (int64_t i = 0; i < ds.n_rows(); ++i) {
                const double xb = fb * cx1[i] + b2 * cx2[i] + bc[static_cast<int>(cc[i])] + b0;
                const double f = m == "logit" ? std::exp(-xb) / ((1 + std::exp(-xb)) * (1 + std::exp(-xb)))
                                              : std::exp(-0.5 * xb * xb) * 0.3989422804014327;
                ame += f;
            }
            ame = fb * ame / static_cast<double>(ds.n_rows());
        }
        j = cli("margins " + fitarg + "--dydx x1" + tight, tag + "mfx");
        nlohmann::json jd = cli("margins " + fitarg + "--dydx x1", tag + "mfx0");
        const double dydx = coef_field(j, "x1", "dydx"), dydx0 = coef_field(jd, "x1", "dydx");
        std::printf("  %s margins dydx(x1): refit %.17g (posted-b AME %.17g; default refit %.17g)\n",
                    m.c_str(), dydx, ame, dydx0);
        expect(rel(dydx, ame, 1e-12) && !rel(dydx0, ame, 1e-12),
               (m + " margins dydx(x1) = AME of the posted b").c_str(), dydx, ame);
        // Control (lp_default / lp_probit_ctl): no flags, no echo, the default iterate.
        nlohmann::json f0 = cli(m + " " + base, tag + "fit0");
        j = cli("test " + fitarg + "--joint 2.c", tag + "t2c0");
        expect(f0.is_object() && !f0.contains("ml_max_argv") && rel(jnum(j, {"chi2"}), A.ctl_chi2_2c, 1e-9),
               (m + " default control: no echo, test 2.c r(chi2) (lp_default)").c_str(), jnum(j, {"chi2"}),
               A.ctl_chi2_2c);
    }
    // A refit that is not logit/probit has no maximize options: refused, never ignored.
    nlohmann::json j = cli("lincom --data \"" + hb + "\" --y x2 --x x1 --exp x1 --tolerance 1e-12", "olstol");
    expect(j.is_null(), "--tolerance on a least-squares refit refused", j.is_null() ? 1 : 0, 1);
    j = cli("lincom " + base + "--model poisson --exp x1 --iterate 50", "poistol");
    expect(j.is_null(), "--iterate on the poisson refit refused", j.is_null() ? 1 : 0, 1);
    // BUG-872: a value is consumed whole or refused. std::stoi/std::stod stopped at the
    // first character they could not read, so `--iterate 1e3` ran ONE iteration (rc 0,
    // converged false) and `--tolerance 1e-12x` ran as 1e-12; on the fit and on every refit.
    for (const std::string& bad : {std::string("--iterate 1e3"), std::string("--iterate 300x"),
                                   std::string("--iterate 2.5"), std::string("--tolerance 1e-12x")}) {
        j = cli("probit " + base + bad, "badfit");
        expect(j.is_null(), ("probit fit " + bad + " refused").c_str(), j.is_null() ? 1 : 0, 1);
        j = cli("lincom " + base + "--model logit --exp x1 " + bad, "badref");
        expect(j.is_null(), ("logit lincom refit " + bad + " refused").c_str(), j.is_null() ? 1 : 0, 1);
    }
    native_unlink(hb);

}

}  // namespace

int test_postest_w2c_main() {
    std::printf("test_postest_w2c:\n");
    const std::string path = native_temp_path("cecon_test_postest_w2c.hbin");
    Dataset ds = make_fixture(path);

    // Fixture confirmation (expanded_ic_ig_x1_ctl).
    {
        ModelData md = build_model(ds, "y", {"i.c", "i.g", "x1"}, {}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        const int j2c = postest::resolve_coef(r.names, "2.c", nullptr).idx;
        expect(md.dm.n == 60, "fixture N", static_cast<double>(md.dm.n), 60);
        expect(rel(r.r2, .956489519262575, 1e-9), "fixture e(r2)", r.r2, .956489519262575);
        expect(j2c >= 0 && rel(r.beta[j2c], 1.206806960142391, 1e-9), "fixture _b[2.c]",
               j2c >= 0 ? r.beta[j2c] : std::nan(""), 1.206806960142391);
    }

    // test_2c_eq_avg_base: `test 2.c = 0.5*(1.c+0.c)` — 0.c is the omitted base.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2", "i.c"}, {}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        const postest::FactorLevels lv = levels_of(ds, md, {"c"});
        postest::LinearForm lhs, rhs;
        std::string err;
        bool needs = false;
        const bool ok = postest::LinearParser("2.c", r.names, &lv).parse(lhs, err, needs) &&
                        postest::LinearParser("0.5*(1.c+0.c)", r.names, &lv).parse(rhs, err, needs);
        postest::LinearForm row = lhs;
        for (size_t j = 0; j < row.w.size(); ++j) row.w[j] -= rhs.w[j];
        row.konst -= rhs.konst;
        int J = 0;
        const double F = ok ? f_test(r, {row}, &J) : std::nan("");
        expect(ok && J == 1 && rel(F, 3.744827017651374, 1e-9), "test_2c_eq_avg_base F", F,
               3.744827017651374);
        const double p = f_pvalue(F, 1, static_cast<double>(r.dof.dof_resid));
        expect(std::fabs(p - .0582156163549912) <= 1e-8, "test_2c_eq_avg_base p", p,
               .0582156163549912);
        // The base resolves to Omitted only because the sample HAS level 0; a level
        // it never had is not found (test_level_not_in_model, r(111)); without the
        // sample the resolver refuses to guess.
        expect(postest::resolve_coef(r.names, "0.c", &lv).kind == postest::CoefKind::Omitted &&
                   postest::resolve_coef(r.names, "0b.c", &lv).kind == postest::CoefKind::Omitted &&
                   postest::resolve_coef(r.names, "9.c", &lv).kind == postest::CoefKind::NotFound &&
                   postest::resolve_coef(r.names, "0.c", nullptr).kind ==
                       postest::CoefKind::NeedsSample,
               "base / absent / unclassified level kinds", 1, 1);
    }

    // testparm_i24_g: 2.g 3.g 4.g jointly, model `x1 x2 i.c i.g`.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2", "i.c", "i.g"}, {}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        std::vector<postest::LinearForm> rows;
        for (const char* nm : {"2.g", "3.g", "4.g"}) {
            postest::LinearForm f;
            f.w.assign(r.names.size(), 0.0);
            const postest::CoefRef ref = postest::resolve_coef(r.names, nm, nullptr);
            if (ref.kind == postest::CoefKind::Column) f.w[ref.idx] = 1.0;
            rows.push_back(f);
        }
        int J = 0;
        const double F = f_test(r, rows, &J);
        expect(J == 3 && rel(F, .9495440012222118, 1e-9), "testparm_i24_g F", F, .9495440012222118);
        expect(r.dof.dof_resid == 50, "testparm_i24_g df_r", static_cast<double>(r.dof.dof_resid), 50);
    }

    // lincom_bexpr: `_b[2.c] + _b[x1]`.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2", "i.c"}, {}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        postest::LinearForm f;
        std::string err;
        bool needs = false;
        const bool ok = postest::LinearParser("_b[2.c] + _b[x1]", r.names, nullptr).parse(f, err, needs);
        const int k = static_cast<int>(r.names.size());
        double est = f.konst, var = 0;
        for (int p = 0; ok && p < k; ++p) {
            est += f.w[p] * r.beta[p];
            for (int q = 0; q < k; ++q) var += f.w[p] * f.w[q] * r.vcov_matrix[static_cast<size_t>(q) * k + p];
        }
        expect(ok && rel(est, 3.183334592562747, 1e-12), "lincom_bexpr estimate", est, 3.183334592562747);
        expect(ok && rel(std::sqrt(var), .5544680813153977, 1e-9), "lincom_bexpr se", std::sqrt(var),
               .5544680813153977);
    }

    // nlcom_multi_post: (A: _b[x1]/_b[x2]) (B: _b[2.c]) — _b[A] and the names A B.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2", "i.c"}, {}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        const postest::FactorLevels lv = levels_of(ds, md, {"c"});
        std::vector<NlcomTerm> t =
            nlcom_apply(r.names, r.beta, r.vcov_matrix, {"A=_b[x1]/_b[x2]", "B=_b[2.c]"}, &lv);
        expect(t.size() == 2 && t[0].name == "A" && t[1].name == "B", "nlcom_multi_post names A B", 1, 1);
        expect(t.size() == 2 && rel(t[0].est, -2.262364835627125, 1e-9), "nlcom_multi_post _b[A]",
               t.empty() ? std::nan("") : t[0].est, -2.262364835627125);
        // The base level inside nlcom is the same zero it is for test/lincom.
        std::vector<NlcomTerm> z = nlcom_apply(r.names, r.beta, r.vcov_matrix, {"Z=_b[_cons]+_b[0.c]"}, &lv);
        std::vector<NlcomTerm> c0 = nlcom_apply(r.names, r.beta, r.vcov_matrix, {"C=_b[_cons]"}, &lv);
        expect(z.size() == 1 && c0.size() == 1 && z[0].est == c0[0].est && z[0].se == c0[0].se,
               "nlcom _b[0.c] is 0 with zero variance", z.empty() ? std::nan("") : z[0].est,
               c0.empty() ? std::nan("") : c0[0].est);
    }

    // Weighted bootstrap uses the same weighted model and clustered variance
    // as the estimator. Integer-frequency data additionally agree with a
    // literal row-expanded dataset, an independent check of score aggregation.
    {
        Frame fw, expanded;
        std::vector<double> y, x, w, cl, ey, ex, ecl;
        for (int i = 0; i < 40; ++i) {
            const double a = (i % 7) + 0.13 * i;
            const double b = 1.0 + 0.4 * a + (i % 5) * 0.31;
            const int f = 1 + i % 3;
            y.push_back(b); x.push_back(a); w.push_back(f); cl.push_back(i / 5);
            for (int r = 0; r < f; ++r) { ey.push_back(b); ex.push_back(a); ecl.push_back(i / 5); }
        }
        fw.set_n_rows_hint(static_cast<int64_t>(y.size()));
        expanded.set_n_rows_hint(static_cast<int64_t>(ey.size()));
        fw.append_f64_column("y", y); fw.append_f64_column("x", x);
        fw.append_f64_column("w", w); fw.append_f64_column("cl", cl); fw.finalize_after_bulk_load();
        expanded.append_f64_column("y", ey); expanded.append_f64_column("x", ex);
        expanded.append_f64_column("cl", ecl); expanded.finalize_after_bulk_load();
        const std::string wp = native_temp_path("wildboot_weighted_check.hbin");
        const std::string ep = native_temp_path("wildboot_expanded_check.hbin");
        fw.write_hbin(wp); expanded.write_hbin(ep);
        Dataset wd = Dataset::open(wp), ed = Dataset::open(ep);
        for (WeightKind kind : {WeightKind::Analytic, WeightKind::Probability,
                                WeightKind::Frequency, WeightKind::Importance}) {
            ModelData bm = build_model(wd, "y", {"x"}, {}, {"cl"}, "", true, nullptr, nullptr, "w", kind);
            ModelData fm = bm;
            // boottest normalizes importance weights under clustering, as it
            // does analytic/probability weights (w2t C6_st equals C3/C4_st).
            if (kind == WeightKind::Importance) fm.weight_kind = WeightKind::Analytic;
            const WildBootResult wb = wild_cluster_bootstrap(bm, 1, 0.0, 256, 5);
            const FeolsResult fit = feols_fit(fm, Vcov::Cluster);
            expect(rel(wb.beta, fit.beta[1], 1e-11), "weighted bootstrap coefficient", wb.beta, fit.beta[1]);
            expect(rel(wb.se, fit.se[1], 1e-11), "weighted bootstrap clustered SE", wb.se, fit.se[1]);
            if (kind == WeightKind::Frequency) {
                ModelData em = build_model(ed, "y", {"x"}, {}, {"cl"});
                const WildBootResult eb = wild_cluster_bootstrap(em, 1, 0.0, 256, 5);
                expect(rel(wb.t0, eb.t0, 1e-11), "fweight bootstrap equals expanded t", wb.t0, eb.t0);
                expect(wb.p_symmetric == eb.p_symmetric, "fweight bootstrap equals expanded p", wb.p_symmetric, eb.p_symmetric);
            }
        }
        native_unlink(wp); native_unlink(ep);
    }

    // e(sample) T2: the two inputs cmds_postest.cpp's refit (postest_fit_spec)
    // now takes from the fit, on the w2_esample.do fixture.
    {
        const std::string epath = native_temp_path("cecon_test_postest_w2c_esample.hbin");
        Dataset es = make_esample_fixture(epath);
        // --keep-singletons: areg and xtreg,fe keep the singleton id 11 (row 61);
        // w2_esample.log:925 D4 e(N) 59, :897 D3 59. The default drop answers 58.
        ModelData keep = build_model(es, "y", {"x1", "x2"}, {"id"}, {}, "",
                                     /*drop_singleton=*/false);
        ModelData drop = build_model(es, "y", {"x1", "x2"}, {"id"}, {});
        expect(keep.dm.n == 59, "refit --keep-singletons N (D4)", static_cast<double>(keep.dm.n), 59);
        expect(drop.dm.n == 58, "refit default singleton drop N", static_cast<double>(drop.dm.n), 58);
        // FU-495: cmd_wildboot --keep-singletons builds the bootstrap's model the
        // way the areg/xtreg,fe fit did, so after `areg y x1 x2, absorb(id) vce(cluster
        // id)` it resamples 59 rows in 11 clusters (id 11 is a one-row cluster); the
        // default drop bootstraps the fit without it: 10 clusters.
        {
            ModelData bk = build_model(es, "y", {"x1", "x2"}, {"id"}, {"id"}, "", false);
            ModelData bd = build_model(es, "y", {"x1", "x2"}, {"id"}, {"id"});
            WildBootResult wk = wild_cluster_bootstrap(bk, 0, 0.0, 99, 1);
            WildBootResult wd = wild_cluster_bootstrap(bd, 0, 0.0, 99, 1);
            expect(bk.dm.n == 59 && wk.n_clusters == 11, "wildboot --keep-singletons clusters",
                   static_cast<double>(wk.n_clusters), 11);
            expect(bd.dm.n == 58 && wd.n_clusters == 10, "wildboot default drop clusters",
                   static_cast<double>(wd.n_clusters), 10);
        }
        // --keepif under a time-series operator: the restriction as a row mask
        // after L. read the full series keeps the t==3 rows (w2_esample.log:751-775
        // C6 `regress y L.x1 x2 if t>=3` e(N) 40); the physically filtered copy
        // (maybe_keepif) loses their lags: 30.
        TsMeta ts;
        ts.timevar = "t";
        ts.panelvar = "id";
        std::vector<char> mask(61, 0);
        const double* tc = es.col("t").f64();
        for (int64_t i = 0; i < 61; ++i) mask[static_cast<size_t>(i)] = tc[i] >= 3 ? 1 : 0;
        ModelData masked = build_model(es, "y", {"L.x1", "x2"}, {}, {}, "", true, &ts, &mask);
        expect(masked.dm.n == 40, "refit TS --keepif row mask N (C6)",
               static_cast<double>(masked.dm.n), 40);
        // FU-759: cmd_predict --fe under the same row mask (`xtreg y L.x1 x2 if
        // t>=3, fe`, w2_esample_tsrefit.log:251-263 T6/T7): the masked FE fit is
        // the 40-row fit (_b[L.x1] -.79536988614 = T6's %21x), it reports the
        // reconstructed _cons the prediction x b + _cons reads, and its within
        // residual over the first md.dm.k columns sums to zero on those rows.
        {
            ModelData fe = build_model(es, "y", {"L.x1", "x2"}, {"id"}, {}, "", false, &ts, &mask);
            FeolsResult rf = feols_fit(fe, Vcov::IID);
            bool hasCons = false;
            double bL = std::nan("");
            for (size_t j = 0; j < rf.names.size(); ++j) {
                if (rf.names[j] == "_cons") hasCons = true;
                if (rf.names[j] == "L.x1") bL = rf.beta[j];
            }
            double sumE = 0.0;
            for (int64_t i = 0; i < fe.dm.n; ++i) {
                double fit = 0.0;
                for (int c = 0; c < fe.dm.k; ++c)
                    fit += fe.dm.X[static_cast<size_t>(c) * fe.dm.n + i] * rf.beta[c];
                sumE += fe.dm.y[static_cast<size_t>(i)] - fit;
            }
            expect(fe.dm.n == 40 && hasCons, "masked FE predict refit N with _cons (T7)",
                   static_cast<double>(fe.dm.n), 40);
            expect(std::fabs(bL - (-0.79536988614447)) < 1e-9, "masked FE _b[L.x1] (T6)", bL,
                   -0.79536988614447);
            expect(std::fabs(sumE) < 1e-9, "masked FE within residual sums to zero", sumE, 0.0);
        }
        const std::string fpath = native_temp_path("cecon_test_postest_w2c_esample_if.hbin");
        {
            Frame f = Frame::from_dataset_shared(std::make_shared<Dataset>(Dataset::open(epath)));
            f.keep_if("t>=3");
            native_unlink(fpath);
            f.write_hbin(fpath);
        }
        Dataset filtered = Dataset::open(fpath);
        ModelData phys = build_model(filtered, "y", {"L.x1", "x2"}, {}, {}, "", true, &ts);
        expect(phys.dm.n == 30, "physically filtered TS refit N (the old answer)",
               static_cast<double>(phys.dm.n), 30);
        native_unlink(fpath);
        native_unlink(epath);
    }

    // Interaction names in any atom order, with or without `c.` (StataBE, AER 007
    // Joint_test: `test c.age_BL#1.treatment` constrains 1.treatment#c.age_BL; the
    // engine stores the cell as age_BL#1.treatment).
    {
        const std::vector<std::string> nm = {"age_BL", "1.treatment", "age_BL#1.treatment",
                                             "1.single#1.treatment", "_cons"};
        const auto a = postest::resolve_coef(nm, "c.age_BL#1.treatment", nullptr);
        const auto b = postest::resolve_coef(nm, "1.treatment#c.age_BL", nullptr);
        const auto c = postest::resolve_coef(nm, "1.treatment#1.single", nullptr);
        expect(a.kind == postest::CoefKind::Column && a.idx == 2, "c.x#1.t resolves to x#1.t",
               static_cast<double>(a.idx), 2);
        expect(b.kind == postest::CoefKind::Column && b.idx == 2, "1.t#c.x resolves to x#1.t",
               static_cast<double>(b.idx), 2);
        expect(c.kind == postest::CoefKind::Column && c.idx == 3, "factor#factor in the other order",
               static_cast<double>(c.idx), 3);
        expect(postest::resolve_coef(nm, "c.nosuch#1.treatment", nullptr).kind ==
                   postest::CoefKind::NotFound,
               "an unknown interaction stays not found", 0, 0);
    }

    // Abbreviated variables inside a coefficient name resolve against the model's
    // variables (StataBE uni-oracle/hz.do Z1/Z2: `1.treatment#1.husb`,
    // `_b[weight]`); two model candidates is not found (Z5).
    {
        const std::vector<std::string> nm = {"1.treatment", "1.husb_influence_kids_original_BL",
                                             "1.treatment#1.husb_influence_kids_original_BL",
                                             "weight_a", "_cons"};
        const auto a = postest::resolve_coef(nm, "1.treatment#1.husb_influence_kids", nullptr);
        const auto b = postest::resolve_coef(nm, "_b[weight]", nullptr);
        const auto c = postest::resolve_coef(nm, "1.husb#1.treat", nullptr);
        expect(a.kind == postest::CoefKind::Column && a.idx == 2, "1.t#1.<abbrev> resolves",
               static_cast<double>(a.idx), 2);
        expect(b.kind == postest::CoefKind::Column && b.idx == 3, "_b[<abbrev>] resolves",
               static_cast<double>(b.idx), 3);
        expect(c.kind == postest::CoefKind::Column && c.idx == 2, "abbreviated atoms in the other order",
               static_cast<double>(c.idx), 2);
        const std::vector<std::string> two = {"weight_a", "weight_b", "_cons"};
        expect(postest::resolve_coef(two, "weight", nullptr).kind == postest::CoefKind::NotFound,
               "two model candidates stay not found", 0, 0);
    }

    native_unlink(path);
    cli_noconstant_arms();
    cli_maxopts_arms();
    std::printf("%s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail;
}

int main(int argc, char** argv) {
    // The cobra built alongside this test (same build directory), or argv[1].
    if (argc > 1) {
        g_cobra = argv[1];
    } else if (argc > 0) {
        const std::filesystem::path self(argv[0]);
#if defined(_WIN32)
        g_cobra = (self.parent_path() / "cobra.exe").string();
#else
        g_cobra = (self.parent_path() / "cobra").string();
#endif
    }
    return test_postest_w2c_main();
}
