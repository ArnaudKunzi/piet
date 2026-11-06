// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// egen row-function parity test: Frame::row_egen vs Stata egen rowmean/rowsd/
// rowtotal/rowmax/rowmin/rowmiss/rownonmiss. Golden from rowegen_golden.do.
// Data tests/validate/rowegen.hbin = s1,s2,s3 with missing values in rows 2 & 5
// (row 5 = all missing: total->0, others->missing, nonmiss->0).
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

static int g_fail = 0;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

static void check_col(const Frame& fr, const std::string& name,
                      const std::vector<double>& gold) {
    const FrameColumn& c = fr.col(name);
    for (size_t i = 0; i < gold.size(); ++i) {
        double got = c.f64[i], want = gold[i];
        bool ok;
        if (std::isnan(want)) ok = std::isnan(got);
        else ok = std::fabs(got - want) / (std::fabs(want) + 1e-30) <= 1e-12;
        if (!ok) {
            std::printf("  FAIL %s[%zu] got=%.12g want=%.12g\n", name.c_str(), i, got, want);
            ++g_fail;
        }
    }
}

int test_rowegen_main() {
    std::printf("test_rowegen:\n");
    const cobra_test::ValidateBase base;

    // read golden columns
    std::ifstream f(base + "rowegen_golden.csv");
    std::string line;
    std::getline(f, line);  // header
    std::vector<double> rm, rsd, rt, rmax, rmin, rmiss, rnm;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> v;
        while (std::getline(ss, cell, ',')) v.push_back(cell);
        // s1,s2,s3,rm,rsd,rt,rmax,rmin,rmiss,rnm
        rm.push_back(parse_stata(v[3]));
        rsd.push_back(parse_stata(v[4]));
        rt.push_back(parse_stata(v[5]));
        rmax.push_back(parse_stata(v[6]));
        rmin.push_back(parse_stata(v[7]));
        rmiss.push_back(parse_stata(v[8]));
        rnm.push_back(parse_stata(v[9]));
    }

    Dataset ds = Dataset::open(base + "rowegen.hbin");
    Frame fr = Frame::from_dataset(ds);
    std::vector<std::string> cols = {"s1", "s2", "s3"};
    fr.row_egen("rm", Frame::RowFn::Mean, cols);
    fr.row_egen("rsd", Frame::RowFn::Sd, cols);
    fr.row_egen("rt", Frame::RowFn::Total, cols);
    fr.row_egen("rmax", Frame::RowFn::Max, cols);
    fr.row_egen("rmin", Frame::RowFn::Min, cols);
    fr.row_egen("rmiss", Frame::RowFn::Miss, cols);
    fr.row_egen("rnm", Frame::RowFn::NonMiss, cols);

    check_col(fr, "rm", rm);
    check_col(fr, "rsd", rsd);
    check_col(fr, "rt", rt);
    check_col(fr, "rmax", rmax);
    check_col(fr, "rmin", rmin);
    check_col(fr, "rmiss", rmiss);
    check_col(fr, "rnm", rnm);

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
