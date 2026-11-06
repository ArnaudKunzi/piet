// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// DR-IPW 2x2 cell validated against standalone Stata `drdid ..., dripw` on the
// 300-unit synthetic panel (tests/validate/drdid2x2_cell.csv).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/csdid.h"
#include "validate_dir.h"

using namespace cecon;

int test_drdid_main() {
    std::printf("test_drdid:\n");
    const std::string path = cobra_test::validate_path("drdid2x2_cell.csv");
    std::ifstream f(path);
    if (!f) { std::printf("  SKIP (missing %s)\n", path.c_str()); return 0; }
    std::string line;
    std::getline(f, line);  // header: id,trt,x1,x2,dy
    std::vector<double> trt, x1, x2, dy;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string c;
        double v[5];
        int j = 0;
        while (std::getline(ss, c, ',') && j < 5) v[j++] = std::stod(c);
        trt.push_back(v[1]); x1.push_back(v[2]); x2.push_back(v[3]); dy.push_back(v[4]);
    }
    int64_t n = static_cast<int64_t>(dy.size());
    std::printf("  n=%lld\n", static_cast<long long>(n));
    std::vector<double> X(static_cast<size_t>(n) * 2);
    for (int64_t i = 0; i < n; ++i) { X[i] = x1[i]; X[n + i] = x2[i]; }

    DrdidCell c = drdid_dripw_cell(dy.data(), X.data(), n, 2, trt.data());

    double att_exp = 2.05610834185971, se_exp = 0.124005088944043;
    double ra = std::fabs(c.att - att_exp) / (std::fabs(att_exp) + 1e-30);
    double rs = std::fabs(c.se - se_exp) / (std::fabs(se_exp) + 1e-30);
    std::printf("  att   cecon %.12g  stata %.12g  rel %.2e\n", c.att, att_exp, ra);
    std::printf("  se    cecon %.12g  stata %.12g  rel %.2e\n", c.se, se_exp, rs);
    int fail = (ra > 1e-7 || rs > 1e-7) ? 1 : 0;

    // ---- Outcome-regression cell (csdid/drdid method reg; csdid-drdid
    // LAYOUT.md 1.5). (a) Without a covariate it is the difference of mean dy
    // between treated and comparison units, and the same estimator as dripw
    // with a constant-only propensity and outcome model. (b) With covariates
    // its ATT is mean over treated of dy - X'b, b = least squares of dy on
    // [X, 1] over the comparison units (recomputed here by normal equations),
    // and its influence function averages to the ATT.
    {
        DrdidCell r0 = drdid_reg_cell(dy.data(), nullptr, n, 0, trt.data());
        DrdidCell d0 = drdid_dripw_cell(dy.data(), nullptr, n, 0, trt.data());
        double s1 = 0, s0 = 0, n1 = 0, n0 = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (trt[i] != 0) { s1 += dy[i]; ++n1; } else { s0 += dy[i]; ++n0; }
        }
        const double dm = s1 / n1 - s0 / n0;
        const double ra0 = std::fabs(r0.att - dm) / std::fabs(dm);
        const double req = std::fabs(r0.att - d0.att) / std::fabs(dm) +
                           std::fabs(r0.se - d0.se) / d0.se;
        std::printf("  reg, no covariate: att %.12g (mean diff %.12g, rel %.2e); vs dripw rel %.2e\n",
                    r0.att, dm, ra0, req);
        if (!r0.ok || !(ra0 <= 1e-12) || !(req <= 1e-12)) { std::printf("    <<FAIL\n"); fail = 1; }

        DrdidCell r2 = drdid_reg_cell(dy.data(), X.data(), n, 2, trt.data());
        // Normal equations over comparisons for [x1, x2, 1].
        double A[3][4] = {{0}};
        for (int64_t i = 0; i < n; ++i) {
            if (trt[i] != 0) continue;
            const double z[3] = {x1[i], x2[i], 1.0};
            for (int a = 0; a < 3; ++a) {
                for (int c = 0; c < 3; ++c) A[a][c] += z[a] * z[c];
                A[a][3] += z[a] * dy[i];
            }
        }
        for (int c = 0; c < 3; ++c)
            for (int r = c + 1; r < 3; ++r) {
                const double f = A[r][c] / A[c][c];
                for (int k = c; k < 4; ++k) A[r][k] -= f * A[c][k];
            }
        double beta[3];
        for (int r = 2; r >= 0; --r) {
            double v = A[r][3];
            for (int k = r + 1; k < 3; ++k) v -= A[r][k] * beta[k];
            beta[r] = v / A[r][r];
        }
        double se1 = 0;
        for (int64_t i = 0; i < n; ++i)
            if (trt[i] != 0) se1 += dy[i] - (beta[0] * x1[i] + beta[1] * x2[i] + beta[2]);
        const double att_ref = se1 / n1;
        double infmean = 0;
        for (double v : r2.inf) infmean += v;
        infmean /= static_cast<double>(n);
        const double ra2 = std::fabs(r2.att - att_ref) / std::fabs(att_ref);
        const double rm2 = std::fabs(infmean - r2.att) / std::fabs(att_ref);
        std::printf("  reg, x1 x2: att %.12g (normal equations %.12g, rel %.2e); IF mean rel %.2e; se %.12g\n",
                    r2.att, att_ref, ra2, rm2, r2.se);
        if (!r2.ok || !(ra2 <= 1e-10) || !(rm2 <= 1e-12) || !(r2.se > 0)) {
            std::printf("    <<FAIL\n");
            fail = 1;
        }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
