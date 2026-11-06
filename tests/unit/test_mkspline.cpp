// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Restricted cubic spline parity test: Frame::mkspline_cubic vs Stata
// `mkspline ag = age_z, cubic` (default 5 Harrell knots, boundary-clamped, raw x).
// Validates knot placement (Stata centile) + RCS basis formula. Golden = first 200
// rows of ag1..ag4 from mkspline_golden.do (knots computed over the full sample).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

int test_mkspline_main() {
    std::printf("test_mkspline:\n");
    const cobra_test::ValidateBase base;

    // The golden CSV is a private fixture (not shipped in public checkouts):
    // declare a visible skip rather than silently comparing nothing.
    std::ifstream f(base + "mkspline_golden.csv");
    if (!f.is_open()) {
        std::printf("test_mkspline: SKIP — mkspline_golden.csv not present in this checkout (private fixture)\n");
        return 0;
    }

    Dataset ds = Dataset::open(base + "csdid_input.hbin");
    Frame fr = Frame::from_dataset(ds);
    fr.mkspline_cubic("ag", "age_z", 5, {});  // default Harrell 5-knot RCS

    const FrameColumn& a1 = fr.col("ag1");
    const FrameColumn& a2 = fr.col("ag2");
    const FrameColumn& a3 = fr.col("ag3");
    const FrameColumn& a4 = fr.col("ag4");
    const double* cc[4] = {a1.f64.data(), a2.f64.data(), a3.f64.data(), a4.f64.data()};

    std::string line;
    std::getline(f, line);  // header
    int fail = 0, row = 0;
    double worst = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        std::vector<double> g;
        while (std::getline(ss, cell, ',')) g.push_back(parse_stata(cell));
        // g = age_z, ag1, ag2, ag3, ag4
        if (std::isnan(g[0])) { ++row; continue; }  // age_z missing -> skip
        for (int j = 0; j < 4; ++j) {
            double got = cc[j][row], want = g[j + 1];
            double rel = std::fabs(got - want) / (std::fabs(want) + 1e-9);
            worst = std::max(worst, rel);
            if (!(rel <= 1e-9)) {
                std::printf("  FAIL row %d ag%d got=%.12g want=%.12g rel=%.2e\n", row, j + 1, got, want, rel);
                ++fail;
            }
        }
        ++row;
    }
    std::printf("  compared %d rows, worst rel err = %.2e\n", row, worst);
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail ? 1 : 0;
}
