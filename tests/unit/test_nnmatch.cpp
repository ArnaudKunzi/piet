// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER scope-2 bundle B2-4 -- `teffects nnmatch` (StataCorp [CAUSAL]), the last
// statistical blocker of paper 076. Every value below is an ORACLE value,
// captured from live StataBE 19.5 BEFORE src/estimators/nnmatch.cpp existed:
//
//   tests/validate/b2_oracle.{do,json}       te_* keys (the B2 answer key)
//   internal capture oracle.txt + data_1..14.csv
//   .../REFUTE-0.md                          the H2 match-set rule that
//                                            REPLACES REPORT.md §4.3 step 2
//   tests/validate/nnmatch_f{2,12,13}_golden.csv  committed copies of data_{2,12,13}
//
// The ids are what the paper consumes, so they are asserted in FULL and IN
// ORDER, not just counted: F2 is 60 rows of exact-cell ties (ascending), F12 is
// the tolerance ladder that brackets dtolerance at relative 1e-8 vs 3e-8, F13
// is `atet nneighbor(2)` on indicator cells with control rows filled.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/error.h"
#include "estimators/nnmatch.h"
#include "validate_dir.h"

using namespace cecon;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool close_rel(double a, double b, double rtol) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= rtol * std::max(1.0, std::fabs(b)) ||
           std::fabs(a - b) <= rtol * std::fabs(b);
}

// Stata's %21x ("+1.a3491f36cbd54X-006") is a C hex-float with X for p.
double stata_hex(const std::string& t0) {
    std::string t = t0;
    if (t.empty() || t == ".") return std::nan("");
    const bool neg = t[0] == '-';
    if (t[0] == '+' || t[0] == '-') t = t.substr(1);
    const auto x = t.find('X');
    if (x == std::string::npos) return std::atof(t0.c_str());
    const std::string mant = t.substr(0, x);
    const int exp = static_cast<int>(std::strtol(t.substr(x + 1).c_str(), nullptr, 16));
    const double v = std::strtod(("0x" + mant + "p0").c_str(), nullptr) * std::ldexp(1.0, exp);
    return neg ? -v : v;
}

struct Csv {
    std::vector<std::string> names;
    std::map<std::string, std::vector<double>> cols;
    const double* c(const std::string& n) const { return cols.at(n).data(); }
    std::size_t n() const { return cols.at(names[0]).size(); }
};

Csv read_csv(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::printf("  FAIL  cannot open %s\n", path.c_str()); ++failures; return {}; }
    Csv out;
    std::string line;
    std::getline(f, line);
    { std::stringstream ss(line); std::string tok;
      while (std::getline(ss, tok, ',')) { out.names.push_back(tok); out.cols[tok] = {}; } }
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line); std::string tok; std::size_t j = 0;
        while (std::getline(ss, tok, ',') && j < out.names.size())
            out.cols[out.names[j++]].push_back(stata_hex(tok));
        while (j < out.names.size()) out.cols[out.names[j++]].push_back(std::nan(""));
    }
    return out;
}

// The ids Stata wrote, per row, from the mm1..mmK columns of the capture.
std::vector<std::vector<int64_t>> oracle_ids(const Csv& d, const std::vector<std::string>& gen) {
    std::vector<std::vector<int64_t>> out(d.n());
    for (std::size_t i = 0; i < d.n(); ++i)
        for (const auto& g : gen) {
            const double v = d.c(g)[i];
            if (!std::isnan(v)) out[i].push_back(static_cast<int64_t>(v));
        }
    return out;
}

void check_ids(const NnmatchResult& R, const std::vector<std::vector<int64_t>>& want,
               const std::string& label) {
    bool ok = R.omega.size() == want.size();
    std::size_t bad = 0;
    for (std::size_t i = 0; ok && i < want.size(); ++i)
        if (R.omega[i] != want[i]) { ok = false; bad = i + 1; }
    check(ok, label + (ok ? "" : " (first mismatch at row " + std::to_string(bad) + ")"));
}

// ---------------------------------------------------------------------------
// F1 / the B2 answer key: 200 rows, two continuous covariates, defaults.
// Built in-body exactly as tests/validate/b2_oracle.do does (no RNG).
void test_b2_fixture() {
    std::printf("test_nnmatch / B2 answer key (b2_oracle.json te_* keys):\n");
    const int n = 200;
    std::vector<double> x1(n), x2(n), y(n), t(n);
    for (int i = 0; i < n; ++i) {
        const double nn = i + 1;
        x1[i] = std::fmod(nn * 7, 23) / 23;
        x2[i] = std::fmod(nn * 13, 17) / 17;
        const double g = static_cast<double>((i + 1) % 2);
        t[i] = static_cast<double>((i / 3) % 2);
        y[i] = 1 + 2 * x1[i] - 0.5 * x2[i] + 0.3 * g + 0.25 * t[i] +
               std::fmod(nn * 11, 31) / 31 - 0.5;
    }
    std::vector<char> keep(n, 1);
    NnmatchResult R = nnmatch_fit(y.data(), t.data(), {"x1", "x2"}, {x1.data(), x2.data()}, {}, {},
                                 keep, NnmatchOpts{});
    check(R.n == 200 && R.n0 == 101 && R.n1 == 99, "e(N)=200 e(n0)=101 e(n1)=99");
    // b2_oracle.json te_ate__hex / te_se__hex / te_v11__hex
    check(close_rel(R.tau, stata_hex("+1.a3491f36cbd54X-006"), 1e-12), "e(b) == ATE oracle");
    check(close_rel(std::sqrt(R.V), stata_hex("+1.ddc67bbd6f0c2X-005"), 1e-12), "SE == oracle");
    check(close_rel(R.V, stata_hex("+1.bdd6a181ca555X-009"), 1e-12), "e(V)[1,1] == oracle");
    // te_mm_list = "mm1 mm2": TWO gen() columns, i.e. some row has a tie.
    check(R.k_nnmax == 2, "e(k_nnmax) == 2 (te_mm_list = `mm1 mm2')");
    check(R.k_nnmin == 1, "e(k_nnmin) == 1");
    check(R.omega[0][0] == 90, "mm1[1] == 90");
    check(R.omega[1][0] == 130, "mm1[2] == 130");
    check(R.omega[4][0] == 133, "mm1[5] == 133");
    check(R.omega[199][0] == 124, "mm1[200] == 124");
    // The tie at row 104 is at a NONZERO distance, so its ids are DESCENDING
    // (130 then 78). An always-ascending or first-member-only rule flips it --
    // both are falsified arms in REPORT.md §5.
    check(R.omega[103].size() == 2 && R.omega[103][0] == 130 && R.omega[103][1] == 78,
          "row 104's nonzero-distance tie is listed DESCENDING: 130 78");
    check(R.colnames == std::vector<std::string>({"x1", "x2"}) && R.dropped.empty(),
          "no column dropped on a full-rank design");
}

// ---------------------------------------------------------------------------
struct Fixture {
    const char* file;
    const char* label;
    std::vector<std::string> xs, fvs;
    std::vector<std::string> gen;
    NnmatchOpts o;
    const char* b_hex;
    const char* V_hex;
    int kmin, kmax;
    int64_t n0, n1;
};

void run_fixture(const Fixture& F) {
    std::printf("test_nnmatch / %s:\n", F.label);
    Csv d = read_csv(cobra_test::validate_path(F.file));
    if (d.names.empty()) return;
    std::vector<char> keep(d.n(), 1);
    std::vector<const double*> xc, fc;
    for (const auto& v : F.xs) xc.push_back(d.c(v));
    for (const auto& v : F.fvs) fc.push_back(d.c(v));
    NnmatchResult R = nnmatch_fit(d.c("y"), d.c("t"), F.xs, xc, F.fvs, fc, keep, F.o);
    check(R.n0 == F.n0 && R.n1 == F.n1, "group sizes");
    check(close_rel(R.tau, stata_hex(F.b_hex), 1e-12), "e(b) == oracle");
    check(close_rel(R.V, stata_hex(F.V_hex), 1e-12), "e(V) == oracle");
    check(R.k_nnmin == F.kmin && R.k_nnmax == F.kmax, "e(k_nnmin)/e(k_nnmax)");
    check_ids(R, oracle_ids(d, F.gen), "every gen() id, in Stata's order");
    // predict te, on every row
    bool teok = true;
    for (std::size_t i = 0; i < d.n(); ++i)
        if (!close_rel(R.te[i], d.c("te_")[i], 1e-9)) teok = false;
    check(teok, "predict te reproduced on every row");
}

void test_refusals() {
    std::printf("test_nnmatch / honest refusals:\n");
    const int n = 20;
    std::vector<double> x(n), y(n), t(n);
    for (int i = 0; i < n; ++i) { x[i] = i % 7; y[i] = i * 0.5; t[i] = i % 2; }
    std::vector<char> keep(n, 1);
    auto expect = [&](NnmatchOpts o, const char* rc, const std::string& label) {
        try {
            nnmatch_fit(y.data(), t.data(), {"x"}, {x.data()}, {}, {}, keep, o);
            check(false, label + " (no throw)");
        } catch (const EngineError& e) {
            check(e.detail == rc, label + " -> " + e.detail);
        }
    };
    { NnmatchOpts o; o.nneighbor = 11; expect(o, "r(198)", "nneighbor > smallest group"); }
    { NnmatchOpts o; o.vce_nn = 1; expect(o, "r(198)", "vce(robust, nn(1))"); }
    // The vce() nn(#) bound is TWO-SIDED and the two sides carry DIFFERENT rc:
    // above the smallest group it is ParseVCE's r(198) (measured: nn(100) and
    // nn(101) on the 200-row golden fixture, n0=101/n1=99); AT the smallest
    // group Stata accepts the parse and dies at RUNTIME with r(459), because
    // Psi(i) draws from the nmin-1 OTHER members of i's own group. Before this
    // check, nn == nmin came back rc 0 with a silently truncated h -- a wrong
    // number under a name the user typed. Here n0 == n1 == 10.
    { NnmatchOpts o; o.vce_nn = 11; expect(o, "r(198)", "vce(robust, nn(nmin+1))"); }
    { NnmatchOpts o; o.vce_nn = 10; expect(o, "r(459)", "vce(robust, nn(nmin))"); }
    { NnmatchOpts o; o.vce_nn = 10;
      try {
          nnmatch_fit(y.data(), t.data(), {"x"}, {x.data()}, {}, {}, keep, o);
          check(false, "vce nn(nmin): Stata's own text (no throw)");
      } catch (const EngineError& e) {
          check(std::string(e.what()) ==
                    "fewer than 10 nearest-neighbor matches for observation 1 when in-group "
                    "matching for the robust() option; use option osample() to identify all "
                    "observations with deficient matches",
                "vce nn(nmin) carries Stata's r(459) text, naming the FIRST deficient row"); } }
    { NnmatchOpts o; o.vce_nn = 9;
      try {
          NnmatchResult R = nnmatch_fit(y.data(), t.data(), {"x"}, {x.data()}, {}, {}, keep, o);
          check(R.n0 == 10 && R.n1 == 10, "vce(robust, nn(nmin-1)) is LEGAL and still runs");
      } catch (const EngineError& e) {
          check(false, std::string("vce(robust, nn(nmin-1)) threw ") + e.detail); } }
    { NnmatchOpts o; o.metric = "matrix"; expect(o, "r(198)", "metric(matrix ...)"); }
    { std::vector<char> none(n, 0);
      try {
          nnmatch_fit(y.data(), t.data(), {"x"}, {x.data()}, {}, {}, none, NnmatchOpts{});
          check(false, "empty sample (no throw)");
      } catch (const EngineError& e) { check(e.detail == "r(2000)", "empty sample -> r(2000)"); } }
    { std::vector<double> t3(n);
      for (int i = 0; i < n; ++i) t3[i] = i % 3;
      try {
          nnmatch_fit(y.data(), t3.data(), {"x"}, {x.data()}, {}, {}, keep, NnmatchOpts{});
          check(false, "three treatment levels (no throw)");
      } catch (const EngineError& e) {
          check(e.detail == "r(198)", "three treatment levels -> r(198)"); } }
}

}  // namespace

int main() {
    test_b2_fixture();
    // F2: 60 rows, ONE covariate with repeated values -- 14 rows tie at
    // distance EXACTLY zero, where the order is ASCENDING observation number.
    run_fixture({"nnmatch_f2_golden.csv", "F2 (60 rows, exact-cell ties, ascending)",
                 {"x1"}, {}, {"mm1", "mm2"}, NnmatchOpts{},
                 "+1.1418394183941X-002", "+1.6e0b742d57e11X-008", 1, 2, 30, 30});
    // F12: the tolerance ladder. Twenty treated units each face two controls at
    // relative gaps 1e-9 .. 1e-4 on both sides of dtolerance; it is the fixture
    // that proves the tolerance is RELATIVE and on the distance itself.
    run_fixture({"nnmatch_f12_golden.csv", "F12 (dtolerance ladder, relative 1e-8 ties / 3e-8 does not)",
                 {"x1"}, {}, {"mm1", "mm2"}, NnmatchOpts{},
                 "-1.a6d01a6d01a6bX-005", "+1.5f4e7f337bdc6X-008", 1, 2, 40, 20});
    // F13: atet nneighbor(2) on three indicator factors, 4:1 -- the run whose
    // control rows are ALSO filled with ids, and whose variance falsifies the
    // manual's typeset sigma^2_delta.
    Fixture f13{"nnmatch_f13_golden.csv", "F13 (atet nneighbor(2), i.a i.b i.c, control rows filled)",
                {}, {"a", "b", "c"}, {"mm1", "mm2", "mm3"}, NnmatchOpts{},
                "+1.be9fa7e9fa7edX-003", "+1.ea7be67489e5cX-007", 2, 3, 18, 72};
    f13.o.atet = true;
    f13.o.nneighbor = 2;
    run_fixture(f13);
    test_refusals();
    std::printf("%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
