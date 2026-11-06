// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-1067: `feols --collinear-forward` keeps the EARLIER member of a collinear set
// (reghdfe, areg and xtreg,fe), where the default keeps the norm-pivoted basis of
// Stata `regress`; the post-estimation refits (lincom/test/predict/margins) take
// the same flag so they answer for the coefficients the fit posted.
//
// Oracle: StataBE 19.5, scratch-r150/c1067/m1.do (the r1.do fixture rebuilt
// below: 60 rows, g = mod(_n,5), x = mod(_n*7,13), z = 2*x, y = 1 + 0.5*x +
// mod(_n*3,7), u = mod(_n*11,17), w = x + u, t = mod(_n,3), d = (t==2), a = 1 +
// mod(_n,3), b = 1 + mod(_n*5,4) recoded so that 3.b == 3.a):
//   A_reghdfe_fe   reghdfe y x z, absorb(g)        x o.z _cons   _b[x] .527368857557537  se .0733801298402088
//   A_reghdfe_noabs reghdfe y x z, noabsorb        x o.z _cons   _b[x] .522503285151117  se .0709984002046116
//   A_regress      regress y x z                  o.x z _cons   (the default here)
//   C_reghdfe_wxu  reghdfe y w x u, absorb(g)     w x o.u       _b[x] .493545792374032
//   D_reghdfe_ab   reghdfe y x i.a i.b, absorb(g) ... 3o.b ...  _b[2.b] -.282981335694036
//   E_reghdfe_d_first reghdfe y d c.x##i.t, absorb(g)  2o.t     _b[d] -.606974414215032
//   E_reghdfe_d_last  reghdfe y c.x##i.t d, absorb(g)  o.d      _b[2.t] -.606974414215035
// Repeated terms (m2.do, m3.do, m4.do; s = mod(_n*5,2)): a factor term repeated across
// the varlist is one term at its first position (`i.t x i.t`: `0b.t 1.t 2.t x`), a
// continuous one repeats as `o.x` (`x x`: `x o.x`, _b[x] .527368857557537).
// The cobra binary is the one built next to this test (or argv[1]).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "data/frame.h"
#include "estimators/feols.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;

namespace {

int g_fail = 0;
std::string g_cobra;
std::string g_data;

void check(bool ok, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}
bool rel(double got, double want, double rtol = 1e-9) {
    return std::isfinite(got) && std::fabs(got - want) <= rtol * std::fabs(want);
}

void write_fixture(const std::string& path) {
    std::vector<double> id, g, x, z, y, u, w, t, d, a, b, sv;
    for (int n = 1; n <= 60; ++n) {
        const double xv = (n * 7) % 13;
        const double av = 1 + n % 3;
        double bv = 1 + (n * 5) % 4;
        if (av == 3) bv = 3;
        else if (bv == 3) bv = 1;
        id.push_back(std::ceil(n / 6.0));
        g.push_back(n % 5);
        x.push_back(xv);
        z.push_back(2 * xv);
        y.push_back(1 + 0.5 * xv + (n * 3) % 7);
        u.push_back((n * 11) % 17);
        w.push_back(xv + (n * 11) % 17);
        t.push_back(n % 3);
        d.push_back(n % 3 == 2 ? 1.0 : 0.0);
        a.push_back(av);
        b.push_back(bv);
        sv.push_back((n * 5) % 2);
    }
    Frame f;
    f.set_n_rows_hint(60);
    f.append_f64_column("id", id);
    f.append_f64_column("g", g);
    f.append_f64_column("x", x);
    f.append_f64_column("z", z);
    f.append_f64_column("y", y);
    f.append_f64_column("u", u);
    f.append_f64_column("w", w);
    f.append_f64_column("t", t);
    f.append_f64_column("d", d);
    f.append_f64_column("a", a);
    f.append_f64_column("b", b);
    f.append_f64_column("s", sv);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
}

// `cobra <verb> --data <fixture> <args> --no-provenance`: parsed stdout, or null
// JSON on a non-zero exit (*rc gets the exit status).
nlohmann::json cli(const std::string& verb, const std::string& args, int* rc_out = nullptr) {
    const std::string outp = native_temp_path("cecon_test_collinear_forward.json");
    const std::string errp = native_temp_path("cecon_test_collinear_forward.err");
    native_unlink(outp);
    std::string cmd = "\"" + g_cobra + "\" " + verb + " --data \"" + g_data + "\" " + args +
                      " --no-provenance > \"" + outp + "\" 2> \"" + errp + "\"";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());
    if (rc_out) *rc_out = rc;
    std::ifstream in(outp, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    native_unlink(outp);
    native_unlink(errp);
    if (rc != 0) return nlohmann::json();
    try { return nlohmann::json::parse(ss.str()); } catch (const std::exception&) { return nlohmann::json(); }
}

double coef(const nlohmann::json& j, const std::string& name, const char* key = "coef") {
    if (!j.is_object() || !j.contains(key) || !j[key].contains(name) || !j[key][name].is_number())
        return std::nan("");
    return j[key][name].get<double>();
}
std::string omitted(const nlohmann::json& j) {
    std::string s;
    if (j.is_object() && j.contains("omitted"))
        for (const auto& o : j["omitted"]) s += (s.empty() ? "" : ",") + o.get<std::string>();
    return s;
}

void direct_arms() {
    std::printf(" feols_fit md.collinear_forward (direct):\n");
    const Dataset ds = Dataset::open(g_data);
    for (int fwd = 0; fwd < 2; ++fwd) {
        ModelData md = build_model(ds, "y", {"x", "z"}, {"g"}, {});
        md.collinear_forward = fwd != 0;
        const FeolsResult r = feols_fit(md, Vcov::IID);
        const std::string om = r.omitted.empty() ? "" : r.omitted[0];
        check(r.omitted.size() == 1 && om == (fwd ? "z" : "x"),
              std::string(fwd ? "forward" : "default") + ": omits " + (fwd ? "z" : "x") + " (got " + om + ")");
        if (fwd)
            check(r.names.size() >= 1 && r.names[0] == "x" && rel(r.beta[0], .527368857557537) &&
                      rel(r.se[0], .0733801298402088),
                  "forward: _b[x] .527368857557537 _se .0733801298402088 (StataBE A_reghdfe_fe)");
    }
    // feols_fit_multi reads the same switch.
    ModelData md = build_model(ds, "y", {"x", "z"}, {"g"}, {});
    md.collinear_forward = true;
    std::vector<std::vector<double>> ys{md.dm.y};
    const std::vector<FeolsResult> rs = feols_fit_multi(md, ys, Vcov::IID, 0, false);
    check(rs.size() == 1 && rs[0].omitted.size() == 1 && rs[0].omitted[0] == "z" &&
              rel(rs[0].beta[0], .527368857557537),
          "feols_fit_multi forward: omits z, _b[x] .527368857557537");
}

void cli_arms() {
    std::printf(" cobra CLI --collinear-forward (%s):\n", g_cobra.c_str());
    if (g_cobra.empty() || !std::filesystem::exists(g_cobra)) {
        check(false, "cobra binary next to this test");
        return;
    }
    nlohmann::json j = cli("feols", "--y y --x x,z --fe g");
    check(omitted(j) == "x", "feols default omits x (regress's pivoted basis), got " + omitted(j));
    j = cli("feols", "--y y --x x,z --fe g --collinear-forward");
    check(omitted(j) == "z" && rel(coef(j, "x"), .527368857557537) &&
              rel(coef(j, "x", "se"), .0733801298402088),
          "feols --fe g forward: omits z, _b[x]/_se as StataBE A_reghdfe_fe");
    j = cli("feols", "--y y --x x,z --collinear-forward");
    check(omitted(j) == "z" && rel(coef(j, "x"), .522503285151117) &&
              rel(coef(j, "x", "se"), .0709984002046116),
          "feols (no FE) forward: omits z, _b[x] .522503285151117 (A_reghdfe_noabs)");
    j = cli("feols", "--y y --x w,x,u --fe g --collinear-forward");
    check(omitted(j) == "u" && rel(coef(j, "x"), .493545792374032, 1e-8),
          "triple w = x + u: omits u, _b[x] .493545792374032 (C_reghdfe_wxu)");
    j = cli("feols", "--y y --x \"x,i(a),i(b)\" --fe g --collinear-forward");
    check(omitted(j) == "3.b" && rel(coef(j, "2.b"), -.282981335694036, 1e-8),
          "factor sets 3.a == 3.b: omits 3.b, _b[2.b] -.282981335694036 (D_reghdfe_ab)");
    j = cli("feols", "--y y --x \"d,c(x)##i(t)\" --fe g --collinear-forward");
    check(omitted(j) == "2.t" && rel(coef(j, "d"), -.606974414215032, 1e-8),
          "d == 2.t, d first: omits 2.t, _b[d] -.606974414215032 (E_reghdfe_d_first)");
    j = cli("feols", "--y y --x \"c(x)##i(t),d\" --fe g --collinear-forward");
    check(omitted(j) == "d" && rel(coef(j, "2.t"), -.606974414215035, 1e-8),
          "d == 2.t, d last: omits d, _b[2.t] -.606974414215035 (E_reghdfe_d_last)");
    j = cli("feols", "--y y,u --x x,z --fe g --collinear-forward");
    check(j.is_object() && j.contains("outcomes") && j["outcomes"].size() == 2 &&
              rel(coef(j["outcomes"][0], "x"), .527368857557537),
          "multi-outcome batch forward: outcome y _b[x] .527368857557537");
    // A factor term repeated across the varlist is one term (StataBE m2.do/m3.do/m4.do,
    // every command alike); a repeated continuous term is a kept column plus an
    // omitted copy whose shared name must not zero the kept coefficient.
    j = cli("feols", "--y y --x \"i(t),x,i(t)\" --fe g --collinear-forward");
    check(omitted(j).empty() && rel(coef(j, "1.t"), -.0401345897819939, 1e-8) &&
              rel(coef(j, "x"), .528186886337161, 1e-8),
          "i.t x i.t is one i.t: _b[1.t] -.0401345897819939 (m2.do H_tt)");
    j = cli("feols", "--y y --x \"c(x)##i(t),i(s)##i(t)\" --fe g --collinear-forward");
    check(omitted(j).empty() && rel(coef(j, "x"), .436534717165232, 1e-8) &&
              rel(coef(j, "1.s#1.t"), -.869931074251519, 1e-8),
          "c.x##i.t s##i.t carries i.t once: _b[x] .436534717165232 (m2.do H_rep)");
    j = cli("feols", "--y y --x x,x --fe g --collinear-forward");
    check(rel(coef(j, "x"), .527368857557537), "x x: the kept x is .527368857557537, not 0 (m2.do H_xx)");
    j = cli("feols", "--y y --x \"c(x)##i(t),c(x)##i(s)\" --fe g --collinear-forward");
    check(rel(coef(j, "x"), .449690422800161, 1e-8),
          "c.x##i.t c.x##i.s: the kept x is .449690422800161, not 0 (m4.do)");
    // The refits replay the rule: without it `lincom x` finds x omitted.
    j = cli("lincom", "--y y --x x,z --fe g --collinear-forward --exp x");
    check(j.is_object() && j.contains("coefs") && rel(j["coefs"][0].value("b", 0.0), .527368857557537) &&
              rel(j["coefs"][0].value("se", 0.0), .0733801298402088),
          "lincom x refit forward: b/se = the fit's _b[x]/_se[x]");
    const double tt = .527368857557537 / .0733801298402088;
    j = cli("test", "--y y --x x,z --fe g --collinear-forward --joint x");
    check(j.is_object() && rel(j.value("F", 0.0), tt * tt, 1e-8), "test x refit forward: F = t^2");
    j = cli("nlcom", "--y y --x x,z --fe g --collinear-forward --expr 2*_b[x]");
    check(j.is_object() && !j.is_null(), "nlcom refit accepts --collinear-forward");
    const std::string outp = native_temp_path("cecon_test_collinear_forward_pred.hbin");
    int rc = 0;
    j = cli("predict", "--y y --x x,z --fe g --collinear-forward --xb xb --out \"" + outp + "\"", &rc);
    check(rc == 0, "predict refit accepts --collinear-forward");
    native_unlink(outp);
    j = cli("margins", "--y y --x x,z --collinear-forward --at x=1", &rc);
    check(rc == 0, "margins --at refit accepts --collinear-forward");
    // Refused where it would be ignored.
    cli("feols", "--y y --x x,z --collinear-forward --regress-exact-fit", &rc);
    check(rc != 0, "feols refuses --collinear-forward with --regress-exact-fit");
    cli("margins", "--y y --x x,z --collinear-forward --dydx x", &rc);
    check(rc != 0, "margins refuses --collinear-forward off the linear --at path");
    cli("lincom", "--y d --x x --model logit --collinear-forward --exp x", &rc);
    check(rc != 0, "lincom --model logit refuses --collinear-forward");
}

}  // namespace

int main(int argc, char** argv) {
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
    g_data = native_temp_path("cecon_test_collinear_forward.hbin");
    write_fixture(g_data);
    std::printf("test_collinear_forward (BUG-1067):\n");
    direct_arms();
    cli_arms();
    native_unlink(g_data);
    std::printf(g_fail ? "FAILED\n" : "PASSED\n");
    return g_fail;
}
