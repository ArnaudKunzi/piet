// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Heckman two-step (Heckit) parity test vs Stata `heckman y x1, select(S = z1 z2)
// twostep`. Golden from heckman_golden.do. Outcome/selection coefs + corrected SEs,
// lambda, rho, sigma all match Stata to ~1e-6.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/heckman.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_heckman_main() {
    std::printf("test_heckman:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, double> g;
    {
        std::ifstream f(base + "heckman_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ',');
            std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }

    Dataset ds = Dataset::open(base + "heckman.hbin");
    int64_t n = ds.n_rows();
    auto col = [&](const char* nm) { const double* p = ds.col(nm).f64(); return std::vector<double>(p, p + n); };
    std::vector<double> y = col("y"), x1 = col("x1"), z1 = col("z1"), z2 = col("z2"), S = col("S");
    int kz = 3, kx = 2;
    std::vector<double> Z(static_cast<size_t>(n) * kz), X(static_cast<size_t>(n) * kx);
    std::copy(z1.begin(), z1.end(), Z.begin());
    std::copy(z2.begin(), z2.end(), Z.begin() + n);
    for (int64_t i = 0; i < n; ++i) Z[2 * n + i] = 1.0;
    std::copy(x1.begin(), x1.end(), X.begin());
    for (int64_t i = 0; i < n; ++i) X[n + i] = 1.0;
    std::vector<char> sel(n);
    for (int64_t i = 0; i < n; ++i) sel[i] = (S[i] == 1.0 && !std::isnan(y[i])) ? 1 : 0;

    HeckmanResult r = heckman_twostep(Z.data(), n, kz, S.data(), X.data(), kx, y.data(), sel);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-10s got=%.10g  stata=%.10g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    // outcome: beta = [x1, const, lambda]
    chk("oy_x1", r.beta[0], g["oy_x1"], 1e-6);
    chk("oy_x1se", r.se[0], g["oy_x1se"], 1e-5);
    chk("oy_c", r.beta[1], g["oy_c"], 1e-6);
    chk("oy_cse", r.se[1], g["oy_cse"], 1e-5);
    chk("lambda", r.lambda, g["lam"], 1e-6);
    chk("se_lambda", r.se_lambda, g["lamse"], 1e-5);
    // selection: [z1, z2, const]
    chk("s_z1", r.sel_beta[0], g["s_z1"], 1e-6);
    chk("s_z1se", r.sel_se[0], g["s_z1se"], 1e-5);
    chk("s_z2", r.sel_beta[1], g["s_z2"], 1e-6);
    chk("s_c", r.sel_beta[2], g["s_c"], 1e-6);
    chk("rho", r.rho, g["rho_e"], 1e-6);
    chk("sigma", r.sigma, g["sig_e"], 1e-6);
    chk("n_selected", static_cast<double>(r.n_selected), g["Nsel"], 1e-12);

    // ---- r150: heckman's first step is `probit ..., asis doopt` ----
    // Fixture: w2u_binary_ml_edges.do section (D), 200 rows. Oracle: StataBE 19.5
    //   probit s <spec>, asis doopt nocoef iter(1000)
    // (r150 diag g02_ml_d012 stata_probe/probe.log, %21x): e(ic) and every b / se. d2's zp
    // column ends with every row completely determined, so H's zp row/column is 0 and
    // invsym leaves se(zp) = 0. The heckman lambda pins are w2u_binary_ml_edges.log's
    // bm_h_d{0,1,2}_fit (e(lambda) %21x).
    {
        const int64_t nn = 200;
        std::vector<double> hx1(nn), hz(nn), hzd(nn), hzp(nn), hs(nn), hyh(nn), hu(nn);
        for (int64_t i = 0; i < nn; ++i) {
            const double r = static_cast<double>(i + 1);
            hx1[i] = std::fmod(r * 7, 23) / 4 - 2.75 + std::fmod(r, 3) / 8;
            hu[i] = (std::fmod(r * 17, 19) / 19 + std::fmod(r * 43, 47) / 47 - 1) * 2;
            hz[i] = std::fmod(r * 13, 29) / 7 - 2;
            hs[i] = (0.7 * hx1[i] + 0.9 * hz[i] + 0.6 * hu[i] + 0.2 > 0) ? 1.0 : 0.0;
            hyh[i] = hs[i] == 1.0 ? 1 + 0.8 * hx1[i] + 0.7 * hu[i] + (std::fmod(r * 61, 67) / 67 - 0.5) * 1.5
                                  : std::nan("");
            hzd[i] = hz[i] + (std::fmod(r, 8) == 0 ? 40 * (2 * hs[i] - 1) : 0);
            hzp[i] = (std::fmod(r, 6) == 0 && hs[i] == 0) ? 1.0 : 0.0;
        }
        struct Spec { const char* tag; std::vector<const std::vector<double>*> cols; int ic;
                      std::vector<const char*> b, se; const char* lambda; };
        const std::vector<Spec> specs = {
            {"d0", {&hx1, &hz}, 6,
             {"0x1.3cf6c167169d0p+0", "0x1.8af7e984f2c91p+0", "0x1.3abf9aa79a2bep-2"},
             {"0x1.76aa8beb2a30ep-3", "0x1.e7fa70e31cdbap-3", "0x1.34b91e1fc548ap-3"},
             "0x1.4cee48fba3601p-1"},
            {"d1", {&hx1, &hzd}, 9,
             {"0x1.3a8af17a902edp+0", "0x1.883a54d413922p+0", "0x1.3df3395dbb9cdp-2"},
             {"0x1.8a9af113f2604p-3", "0x1.024bad1a6b32fp-2", "0x1.4566370a106ddp-3"},
             "0x1.3151c31791863p-1"},
            {"d2", {&hx1, &hz, &hzp}, 21,
             {"0x1.35852b77ecd88p+0", "0x1.858d670238d39p+0", "-0x1.b52d58397d3b8p+2", "0x1.b497cc5025104p-2"},
             {"0x1.83039dc3a685dp-3", "0x1.f79c0a63179fcp-3", "0x0p+0", "0x1.475d623356b7ep-3"},
             "0x1.66632e8d30f34p-1"},
        };
        for (const Spec& sp : specs) {
            const int kzz = static_cast<int>(sp.cols.size()) + 1;
            std::vector<double> Zh(static_cast<size_t>(nn) * kzz, 1.0);
            for (int c = 0; c + 1 < kzz; ++c)
                std::copy(sp.cols[static_cast<size_t>(c)]->begin(), sp.cols[static_cast<size_t>(c)]->end(),
                          Zh.begin() + static_cast<std::ptrdiff_t>(c * nn));
            MlMaxOptions o;
            o.tol = 1e-4; o.ltol = 0.0; o.doopt = true; o.step_forward = false; o.stata_invsym = true;
            ProbitResult pr = probit_fit_ml(Zh.data(), nn, kzz, hs.data(), o, nullptr, true);
            std::printf("  [r150 doopt %s] ic=%d (stata %d)\n", sp.tag, pr.ic, sp.ic);
            if (pr.ic != sp.ic) { std::printf("    <<FAIL ic\n"); fail = 1; }
            for (int c = 0; c < kzz; ++c) {
                const double wb = std::strtod(sp.b[static_cast<size_t>(c)], nullptr);
                const double ws = std::strtod(sp.se[static_cast<size_t>(c)], nullptr);
                const double gs = std::sqrt(std::max(0.0, pr.bread_oim[static_cast<size_t>(c) * kzz + c]));
                const double rb = std::fabs(pr.beta[c] - wb) / std::fabs(wb);
                const double rs = ws == 0.0 ? std::fabs(gs) : std::fabs(gs - ws) / ws;
                std::printf("    b%d %a (rel %.1e)  se %a (rel %.1e)\n", c, pr.beta[c], rb, gs, rs);
                if (!(rb <= 1e-12) || !(rs <= 1e-12)) { std::printf("    <<FAIL %s col %d\n", sp.tag, c); fail = 1; }
            }
            std::vector<double> Xh(static_cast<size_t>(nn) * 2, 1.0);
            std::copy(hx1.begin(), hx1.end(), Xh.begin());
            std::vector<char> selh(nn);
            for (int64_t i = 0; i < nn; ++i) selh[i] = hs[i] == 1.0 ? 1 : 0;
            HeckmanResult hr = heckman_twostep(Zh.data(), nn, kzz, hs.data(), Xh.data(), 2, hyh.data(), selh);
            const double wl = std::strtod(sp.lambda, nullptr);
            const double rl = std::fabs(hr.lambda - wl) / std::fabs(wl);
            std::printf("    heckman lambda %.16g (stata %.16g, rel %.1e)\n", hr.lambda, wl, rl);
            if (!(rl <= 1e-12)) { std::printf("    <<FAIL %s lambda\n", sp.tag); fail = 1; }
        }
    }

    // ---- r150: the doopt first step on a SEPARATED selection (not concave) ----
    // piet/packaging/sample/fatalities.csv, `gen jailb = (unemp > 7)`,
    //   heckman income beertax, select(jailb = spirits unemp) twostep level(90)
    // (piet_selftest's BUG-274 heckman leg). unemp separates jailb, so the probit
    // diverges; StataBE 19.5 (r150 verify_B stata_probe/probe.log) logs `(not
    // concave)` from iteration 27 to 300, stops unconverged (e(ic) 300) and posts
    //   e(b)  .5149750221225655178  119.0995614739077695  -840.4600207060869934
    //   e(V)  spirits 93606658.96, (unemp,spirits) -19704078.16, unemp 4254204.523,
    //         every _cons entry 0 (invsym drops it): se 9675.053, 2062.572, 0.
    // heckman posts the same b/V for jailb (rc 0). Before r150's stata_invsym path
    // this fit ran on to an all-determined sample and posted V = 0 for every column.
    {
        const std::string csv = cobra_test::validate_dir() + "../../piet/packaging/sample/fatalities.csv";
        std::ifstream f(csv);
        if (!f) {
            std::printf("  [r150 separated] SKIP: %s not found\n", csv.c_str());
        } else {
            std::string line;
            std::getline(f, line);
            std::vector<std::string> hdr;
            { std::stringstream ss(line); std::string t; while (std::getline(ss, t, ',')) hdr.push_back(t); }
            auto idx = [&](const char* nm) {
                for (size_t j = 0; j < hdr.size(); ++j) if (hdr[j] == nm) return static_cast<int>(j);
                return -1;
            };
            const int isp = idx("spirits"), iun = idx("unemp");
            std::vector<double> sp, un;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                std::vector<std::string> v;
                std::stringstream ss(line); std::string t;
                while (std::getline(ss, t, ',')) v.push_back(t);
                sp.push_back(std::strtod(v[static_cast<size_t>(isp)].c_str(), nullptr));
                un.push_back(std::strtod(v[static_cast<size_t>(iun)].c_str(), nullptr));
            }
            const int64_t ns = static_cast<int64_t>(sp.size());
            std::vector<double> Zs(static_cast<size_t>(ns) * 3, 1.0), js(ns);
            for (int64_t i = 0; i < ns; ++i) {
                Zs[i] = sp[i];
                Zs[ns + i] = un[i];
                js[i] = un[i] > 7 ? 1.0 : 0.0;
            }
            MlMaxOptions o;
            o.tol = 1e-4; o.ltol = 0.0; o.doopt = true; o.step_forward = false; o.stata_invsym = true;
            const ProbitResult pr = probit_fit_ml(Zs.data(), ns, 3, js.data(), o, nullptr, true);
            const double wb[3] = {.5149750221225655178, 119.0995614739077695, -840.4600207060869934};
            const double wv[3][3] = {{93606658.96, -19704078.16, 0.0},
                                     {-19704078.16, 4254204.523, 0.0},
                                     {0.0, 0.0, 0.0}};
            std::printf("  [r150 separated] n=%lld ic=%d (stata 300) converged=%d (stata 0)\n",
                        static_cast<long long>(ns), pr.ic, pr.converged ? 1 : 0);
            if (ns != 336 || pr.ic != 300 || pr.converged) { std::printf("    <<FAIL ic/converged\n"); fail = 1; }
            for (int c = 0; c < 3; ++c) {
                const double rb = std::fabs(pr.beta[c] - wb[c]) / std::fabs(wb[c]);
                std::printf("    b%d %.17g (rel %.1e)\n", c, pr.beta[c], rb);
                if (!(rb <= 1e-9)) { std::printf("    <<FAIL b%d\n", c); fail = 1; }
                for (int d = 0; d < 3; ++d) {
                    const double gv = pr.bread_oim.size() == 9 ? pr.bread_oim[static_cast<size_t>(c) * 3 + d] : std::nan("");
                    // the probe printed V to 10 digits (%12.0g); zeros are exact
                    const bool ok = wv[c][d] == 0.0 ? gv == 0.0
                                                    : std::fabs(gv - wv[c][d]) / std::fabs(wv[c][d]) <= 1e-8;
                    if (!ok) { std::printf("    <<FAIL V[%d,%d] %.12g stata %.12g\n", c, d, gv, wv[c][d]); fail = 1; }
                }
            }
            std::vector<double> Xs(static_cast<size_t>(ns) * 2, 1.0);  // outcome design only shapes step 2
            const std::vector<double> ys(static_cast<size_t>(ns), std::nan(""));
            std::vector<char> sels(static_cast<size_t>(ns), 0);
            const HeckmanResult hr = heckman_twostep(Zs.data(), ns, 3, js.data(), Xs.data(), 2, ys.data(), sels);
            const double wse[3] = {9675.053, 2062.572, 0.0};
            for (int c = 0; c < 3; ++c) {
                const bool ok = wse[c] == 0.0 ? hr.sel_se[c] == 0.0
                                              : std::fabs(hr.sel_se[c] - wse[c]) / wse[c] <= 1e-6;
                std::printf("    heckman sel se%d %.10g (stata %g)\n", c, hr.sel_se[c], wse[c]);
                if (!ok || std::fabs(hr.sel_beta[c] - wb[c]) / std::fabs(wb[c]) > 1e-9) {
                    std::printf("    <<FAIL heckman sel col %d\n", c); fail = 1;
                }
            }
        }
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
