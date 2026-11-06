// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// ts-operators parity test vs Stata L./F./D./S. operators on a 2-panel series with
// a time gap (panel 1 missing t=4). Golden from ts_operators_golden.do. Checks
// gap-aware and panel-boundary missing rules + composition (LD) and S2 != D2.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "data/ts_operators.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_cell(const std::string& s) {
    if (s == "." || s.empty()) return std::nan("");
    return std::stod(s);
}

int test_ts_operators_main() {
    std::printf("test_ts_operators:\n");
    const cobra_test::ValidateBase base;

    // golden: id,t,x,L1x,L2x,F1x,D1x,D2x,S2x,LDx  (row order matches the hbin)
    std::vector<std::vector<double>> g;
    {
        std::ifstream f(base + "ts_operators_golden.csv");
        std::string line;
        std::getline(f, line);  // header
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string cell;
            std::vector<double> row;
            while (std::getline(ss, cell, ',')) row.push_back(parse_cell(cell));
            g.push_back(row);
        }
    }

    Dataset ds = Dataset::open(base + "ts_operators.hbin");
    const double* id = ds.col("id").f64();
    const double* t = ds.col("t").f64();
    const double* x = ds.col("x").f64();
    int64_t n = ds.n_rows();

    struct Case { const char* expr; int gcol; };
    std::vector<Case> cases = {
        {"L.x", 3}, {"L2.x", 4}, {"F.x", 5}, {"D.x", 6},
        {"D2.x", 7}, {"S2.x", 8}, {"L.D.x", 9},
    };

    int fail = 0;
    auto eq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b)) return true;
        if (std::isnan(a) != std::isnan(b)) return false;
        return std::fabs(a - b) <= 1e-12 * (std::fabs(b) + 1.0);
    };

    for (auto& c : cases) {
        TsOpSpec spec = parse_tsop(c.expr);  // handles "L.x", "L2.x", "L.D.x"
        std::vector<double> out;
        std::string err;
        bool ok = spec.ok && ts_apply(x, n, t, id, 1.0, spec, out, err);
        if (!ok) { std::printf("  %-6s <<FAIL parse/apply: %s\n", c.expr, (spec.ok ? err : spec.err).c_str()); fail = 1; continue; }
        int bad = 0;
        for (int64_t r = 0; r < n; ++r) if (!eq(out[r], g[r][c.gcol])) {
            std::printf("  %-6s row %lld got=%g stata=%g  <<MISMATCH\n", c.expr,
                        (long long)r, out[r], g[r][c.gcol]);
            bad = 1; fail = 1;
        }
        if (!bad) std::printf("  %-6s OK (all %lld rows match incl. NaN)\n", c.expr, (long long)n);
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
