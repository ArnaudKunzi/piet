// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtile + ipolate parity test vs Stata. Decile/quartile bins via pctile cutpoints;
// linear interpolation + epolate extrapolation. Golden from xtipo_golden.do.
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

static double cell(const std::string& s) { return (s == "." || s.empty()) ? std::nan("") : std::stod(s); }

int test_xtipo_main() {
    std::printf("test_xtipo:\n");
    const cobra_test::ValidateBase base;
    // golden: t,xt10,xt4,yi,ye
    std::vector<std::vector<double>> g;
    {
        std::ifstream f(base + "xtipo_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string c;
            std::vector<double> row;
            while (std::getline(ss, c, ',')) row.push_back(cell(c));
            g.push_back(row);
        }
    }
    Dataset ds = Dataset::open(base + "xtipo.hbin");
    Frame fr = Frame::from_dataset(ds);
    fr.xtile("xt10", "x", 10);
    fr.xtile("xt4", "x", 4);
    fr.ipolate("yi", "y", "t", false, {});
    fr.ipolate("ye", "y", "t", true, {});
    int64_t n = fr.n_rows();
    const auto& xt10 = fr.col("xt10").f64;
    const auto& xt4 = fr.col("xt4").f64;
    const auto& yi = fr.col("yi").f64;
    const auto& ye = fr.col("ye").f64;

    auto eq = [](double a, double b, double tol) {
        if (std::isnan(a) && std::isnan(b)) return true;
        if (std::isnan(a) != std::isnan(b)) return false;
        return std::fabs(a - b) <= tol * (std::fabs(b) + 1.0);
    };
    int fail = 0, bad10 = 0, bad4 = 0, badi = 0, bade = 0;
    for (int64_t r = 0; r < n; ++r) {
        if (!eq(xt10[r], g[r][1], 0)) { bad10++; fail = 1; }
        if (!eq(xt4[r], g[r][2], 0)) { bad4++; fail = 1; }
        if (!eq(yi[r], g[r][3], 1e-10)) { badi++; fail = 1; }
        if (!eq(ye[r], g[r][4], 1e-10)) { bade++; fail = 1; }
    }
    std::printf("  xtile nq10 mismatches=%d  nq4=%d  ipolate=%d  epolate=%d  (of %lld rows)\n",
                bad10, bad4, badi, bade, (long long)n);
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
