// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// nlcom delta-method parity test: nlcom_apply vs Stata nlcom (after regress).
// Model: visits_w ~ age_z + age_z^2 + edu_hi; combinations: quadratic turning
// point, linear sum, ratio, _cons*2. Golden from nlcom_golden.do. Point estimates
// match to ~1e-12; SEs to ~1e-12 (nlcom_apply takes testnl's adaptive step; the old
// fixed step was recorded here at ~1e-8, under the 1e-7 bound this replaced).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/feols.h"
#include "estimators/nlcom.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

// Stata's own Jacobian (testnl r(G), %21x, oracle nlcom_G_oracle.log) at Stata's own
// e(b)/e(V) on one fit: exact equality, so the step search keeps its operation order
// and stays free of fused multiply-adds. Includes the left start (row 7), the side
// alternating into a missing step (8), a rescale into a missing gap (9), a function
// that does not move (10) and the |f|-scaled band (11).
static int check_jacobian_bits() {
    const std::vector<std::string> names{"x", "(intercept)"};
    const std::vector<double> beta{0x1.3f84cfe133f84p+4, 0x1.5435e50d79460p+2};
    const double v11 = 0x1.af6a493509a17p-9, v12 = -0x1.1b1dc00ace51fp-5, v22 = 0x1.e3a8281275cbfp-2;
    const std::vector<double> V{v11, v12, v12, v22};
    const std::string u = "(_b[x]-19.969924812030072)";
    struct Row { std::string expr; double g0, g1; };
    const std::vector<Row> rows = {
        {"_b[x]/_b[_cons]", 0x1.81446f864ae51p-3, -0x1.69d5d163694e5p-1},
        {"_b[x]^2^3", 0x1.22c5c89ce2e53p+24, 0},
        {"exp(_b[x]/10) - ln(_b[_cons])", 0x1.792efef7d5645p-1, -0x1.81446f894adf6p-3},
        {"_b[x]*_b[_cons]*_b[x]", 0x1.a89fa878a3f78p+7, 0x1.8ecc42fa1bfb3p+8},
        {u + " + " + u + "*" + u + "*" + u, 0x1.fffffffedfa61p-1, 0},
        {u + " + " + u + "*" + u, 0x1.0000000065b54p+0, 0},
        {u + " + sqrt(.1 - " + u + ")", -0x1.298b077575b8ep-1, 0},
        {u + "^2 + 0*ln(.0000001 - " + u + ")", 0, 0},
        {".0011*" + u + " + 0*ln((" + u + "-.002)*(" + u + "-.02))", 0, 0},
        {"5 + 0*_b[x]", 0, 0},
        {"1000 + " + u + " + " + u + "^3", 0x1.0001d296b7accp+0, 0},
        {"-_b[x]^2", -0x1.3f84cfe1175cdp+5, 0},
        {"_b[_cons]*2", 0, 0x1.000000000f162p+1},
    };
    int fail = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::vector<NlcomTerm> t = nlcom_apply(names, beta, V, {"t=" + rows[i].expr});
        if (t[0].grad[0] != rows[i].g0 || t[0].grad[1] != rows[i].g1) {
            std::printf("    <<FAIL G row %zu: %a %a, Stata %a %a\n", i + 1, t[0].grad[0],
                        t[0].grad[1], rows[i].g0, rows[i].g1);
            fail = 1;
        }
    }
    const auto refusal = [&](const std::vector<std::string>& exprs) -> std::string {
        try {
            nlcom_apply(names, beta, V, exprs);
        } catch (const std::exception& e) {
            return e.what();
        }
        return "(no refusal)";
    };
    const std::string no_slope = "sqrt(-(_b[x]-19.969924812030072)^2)";
    struct Refusal { std::vector<std::string> exprs; std::string want; };
    const std::vector<Refusal> refusals = {
        {{no_slope}, "nlcom: could not calculate numerical derivative"},
        {{"(_b[x]-19.969924812030072)^2"}, "nlcom: Maximum number of iterations exceeded."},
        {{"ln(_b[x] - 100)"}, "expression (ln(_b[x] - 100)) evaluates to missing"},
        // Every term is valued before any is differentiated (nlcom_refusal_order.log).
        {{"a=" + no_slope, "b=ln(_b[x]-100)"}, "expression (ln(_b[x]-100)) evaluates to missing"},
    };
    for (const Refusal& r : refusals) {
        const std::string got = refusal(r.exprs);
        if (got != r.want) {
            std::printf("    <<FAIL refusal: got [%s], want [%s]\n", got.c_str(), r.want.c_str());
            fail = 1;
        }
    }
    return fail;
}

int test_nlcom_main() {
    std::printf("test_nlcom:\n");
    const cobra_test::ValidateBase base;

    // golden: name,b,se
    std::ifstream f(base + "nlcom_golden.csv");
    std::string line;
    std::getline(f, line);  // header
    std::vector<std::string> gname;
    std::vector<double> gb, gse;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string nm, b, se;
        std::getline(ss, nm, ',');
        std::getline(ss, b, ',');
        std::getline(ss, se, ',');
        gname.push_back(nm);
        gb.push_back(std::stod(b));
        gse.push_back(std::stod(se));
    }

    Dataset ds = Dataset::open(base + "nlcom.hbin");
    ModelData md = build_model(ds, "visits_w", {"age_z", "age2", "edu_hi"}, {}, {});
    FeolsResult r = feols_fit(md, Vcov::IID);

    std::vector<std::string> exprs = {
        "turn=-_b[age_z]/(2*_b[age2])",
        "sumc=_b[age_z]+_b[edu_hi]",
        "ratio=_b[age_z]/_b[edu_hi]",
        "cc=_b[_cons]*2",
    };
    std::vector<NlcomTerm> t = nlcom_apply(r.names, r.beta, r.vcov_matrix, exprs);

    int fail = 0;
    for (size_t i = 0; i < gname.size(); ++i) {
        double rb = std::fabs(t[i].est - gb[i]) / (std::fabs(gb[i]) + 1e-30);
        double rs = std::fabs(t[i].se - gse[i]) / (std::fabs(gse[i]) + 1e-30);
        std::printf("  %-6s b=%.12g (stata %.12g rel %.1e)  se=%.10g (stata %.10g rel %.1e)\n",
                    t[i].name.c_str(), t[i].est, gb[i], rb, t[i].se, gse[i], rs);
        if (!(rb <= 1e-9)) { std::printf("    <<FAIL b rel %.2e\n", rb); fail = 1; }
        if (!(rs <= 1e-9)) { std::printf("    <<FAIL se rel %.2e\n", rs); fail = 1; }
    }
    fail |= check_jacobian_bits();
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
