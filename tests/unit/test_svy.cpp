// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — svy linearized (Taylor) variance.
//
// Two layers, deliberately:
//   1. MECHANISM, on synthetic data with a hand-computable answer — the
//      stratified PSU-total meat, the degenerate reduction to the ordinary
//      one-stratum formula, and the singleton-PSU refusal. These run everywhere,
//      always.
//   2. ORACLE, against the P7 answerbook fixture (Stata 19.5,
//      internal capture p7_stata.log). That fixture is a
//      Stata-produced artifact and is NOT in git, so this layer SKIPS with a loud
//      note when it is absent rather than passing quietly — a skipped oracle must
//      never read as a satisfied one.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>
#include <vector>

#include "io/dta_reader.h"
#include "vcov/svy_vcov.h"

using cecon::Factor;
using cecon::SvyMeatResult;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    std::printf("[test_svy] %s  %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

static void close_to(double got, double want, double rtol, const std::string& what) {
    const double denom = std::fabs(want) > 0 ? std::fabs(want) : 1.0;
    const bool ok = std::fabs(got - want) / denom <= rtol;
    std::printf("[test_svy] %s  %s (got %.10g, want %.10g)\n", ok ? "PASS" : "FAIL", what.c_str(),
                got, want);
    if (!ok) ++failures;
}

static Factor make_factor(const std::string& name, const std::vector<int>& codes) {
    Factor f;
    f.name = name;
    f.codes.assign(codes.begin(), codes.end());
    int mx = -1;
    for (int c : codes) mx = c > mx ? c : mx;
    f.n_levels = mx + 1;
    f.count.assign(static_cast<size_t>(f.n_levels), 0.0);
    for (int c : codes) f.count[static_cast<size_t>(c)] += 1.0;
    return f;
}

// --- 1. mechanism ------------------------------------------------------------

static void test_hand_computed_meat() {
    // Two strata, two PSUs each, two observations per PSU; p = 1, so the entire
    // meat is one number that can be written out by hand.
    //   PSU totals z = [3, 7, 11, 15]      (strata {0,1} and {2,3})
    //   stratum 0: mean 5,  deviations ∓2 → Σd² = 8, factor 2/(2−1) = 2 → 16
    //   stratum 1: mean 13, deviations ∓2 → Σd² = 8, factor 2           → 16
    //   meat = 32
    std::vector<double> S = {1, 2, 3, 4, 5, 6, 7, 8};
    Factor psu = make_factor("psu", {0, 0, 1, 1, 2, 2, 3, 3});
    Factor str = make_factor("stratum", {0, 0, 0, 0, 1, 1, 1, 1});
    SvyMeatResult r = cecon::svy_linearized_meat(S.data(), 8, 1, psu, str, nullptr);
    check(r.ok, "hand case: result ok");
    check(r.singleton_stratum.empty(), "hand case: no singleton stratum");
    close_to(r.meat[0], 32.0, 1e-12, "hand case: meat = 32");
    check(r.n_psu == 4 && r.n_strata == 2, "hand case: 4 PSUs, 2 strata");

    // The within-stratum centering IS the difference from an ordinary cluster
    // meat: uncentered over the same PSU totals would be 9+49+121+225 = 404, more
    // than twelve times larger. A check that cannot separate those two is not
    // testing the thing this file exists to test.
    check(std::fabs(r.meat[0] - 404.0) > 1.0, "hand case: centered, not the uncentered 404");
}

static void test_degenerate_reduces_to_simple_formula() {
    // One stratum whose PSUs are the observations. The formula must collapse onto
    // the ordinary n/(n−1)·Σ(u−ū)² that `mean` used before svy existed.
    const int n = 5;
    std::vector<double> S = {2.0, -1.0, 0.5, 3.0, -4.5};
    std::vector<int> psu_codes(static_cast<size_t>(n)), str_codes(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) psu_codes[static_cast<size_t>(i)] = i;
    Factor psu = make_factor("psu", psu_codes);
    Factor str = make_factor("stratum", str_codes);
    SvyMeatResult r = cecon::svy_linearized_meat(S.data(), n, 1, psu, str, nullptr);

    double mean = 0.0;
    for (double v : S) mean += v;
    mean /= n;
    double ss = 0.0;
    for (double v : S) ss += (v - mean) * (v - mean);
    const double want = static_cast<double>(n) / (n - 1) * ss;
    close_to(r.meat[0], want, 1e-12, "degenerate case reduces to n/(n-1)*sum (u-ubar)^2");
}

static void test_singleton_stratum_is_refused() {
    std::vector<double> S = {1, 2, 3};
    Factor psu = make_factor("psu", {0, 1, 2});
    Factor str = make_factor("stratum", {0, 0, 1});  // stratum 1 holds ONE PSU
    SvyMeatResult r = cecon::svy_linearized_meat(S.data(), 3, 1, psu, str, nullptr);
    check(!r.singleton_stratum.empty(),
          "a stratum with a single PSU is reported, not silently divided by zero");
}

static void test_symmetry_multi_column() {
    std::vector<double> S = {1, 2, 3, 4, 5, 6, 7, 8, /*col 2*/ 2, 1, 4, 3, 6, 5, 8, 7};
    Factor psu = make_factor("psu", {0, 0, 1, 1, 2, 2, 3, 3});
    Factor str = make_factor("stratum", {0, 0, 0, 0, 1, 1, 1, 1});
    SvyMeatResult r2 = cecon::svy_linearized_meat(S.data(), 8, 2, psu, str, nullptr);
    close_to(r2.meat[1], r2.meat[2], 1e-12, "meat is symmetric");
    close_to(r2.meat[0], 32.0, 1e-12, "column 0 of the p=2 meat equals the p=1 answer");
}

// --- 2. oracle ---------------------------------------------------------------

static std::string fixture_path() {
    if (const char* e = std::getenv("P7_WAVE_FIXTURE")) return e;
#ifdef COBRA_VALIDATE_DIR
    return std::string(COBRA_VALIDATE_DIR) +
           "/../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
#else
    return "tests/validate/../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
#endif
}

static void test_oracle_svy_mean() {
    const std::string path = fixture_path();
    cecon::DtaResult d;
    try {
        d = cecon::read_dta(path);
    } catch (const std::exception&) {
        std::printf("[test_svy] SKIP  oracle legs: wave fixture not present at %s\n"
                    "[test_svy]       (Stata-produced artifact, not in git; run the P7 answerbook\n"
                    "[test_svy]        capture or set P7_WAVE_FIXTURE to enable these legs)\n",
                    path.c_str());
        return;
    }

    auto col = [&](const std::string& nm) -> const double* {
        for (const auto& c : d.frame.columns())
            if (c.name == nm) return c.f64.data();
        return nullptr;
    };
    const double* y = col("y");
    const double* w = col("pw");
    const double* psu_raw = col("cluster");
    const double* str_raw = col("stratum");
    if (!y || !w || !psu_raw || !str_raw) {
        std::printf("[test_svy] SKIP  oracle legs: fixture lacks an expected column\n");
        return;
    }
    const int64_t N = static_cast<int64_t>(d.frame.n_rows());

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < N; ++i)
        if (!std::isnan(y[i]) && !std::isnan(w[i]) && !std::isnan(psu_raw[i]) &&
            !std::isnan(str_raw[i]))
            keep.push_back(i);
    const int64_t n = static_cast<int64_t>(keep.size());

    auto densify = [&](const double* src, const std::string& nm) {
        std::vector<int> codes(static_cast<size_t>(n));
        std::map<double, int> lev;
        for (int64_t r = 0; r < n; ++r) {
            double v = src[keep[static_cast<size_t>(r)]];
            auto it = lev.find(v);
            if (it == lev.end()) it = lev.emplace(v, static_cast<int>(lev.size())).first;
            codes[static_cast<size_t>(r)] = it->second;
        }
        return make_factor(nm, codes);
    };
    Factor f_psu = densify(psu_raw, "cluster");
    Factor f_str = densify(str_raw, "stratum");

    double W = 0.0, swy = 0.0;
    for (int64_t r = 0; r < n; ++r) {
        int64_t i = keep[static_cast<size_t>(r)];
        W += w[i];
        swy += w[i] * y[i];
    }
    const double mu = swy / W;

    std::vector<double> u(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) {
        int64_t i = keep[static_cast<size_t>(r)];
        u[static_cast<size_t>(r)] = w[i] * (y[i] - mu) / W;
    }
    SvyMeatResult sm = cecon::svy_linearized_meat(u.data(), n, 1, f_psu, f_str, nullptr);

    // Stata 19.5: `svyset cluster [pweight=pw], strata(stratum)` then `svy: mean y`.
    check(n == 4778, "oracle svy: mean — N = 4,778");
    check(f_psu.n_levels == 50 && f_str.n_levels == 4, "oracle — 50 PSUs, 4 strata");
    check(f_psu.n_levels - f_str.n_levels == 46, "oracle — design df = 46");
    close_to(W, 7214.68, 1e-6, "oracle — population size 7,214.68");
    close_to(mu, 3.170481, 1e-6, "oracle — mean 3.170481");
    close_to(std::sqrt(sm.meat[0]), .0355585, 1e-5, "oracle — linearized SE .0355585");
}

int test_svy_main() {
    test_hand_computed_meat();
    test_degenerate_reduces_to_simple_formula();
    test_singleton_stratum_is_refused();
    test_symmetry_multi_column();
    test_oracle_svy_mean();
    if (failures) {
        std::printf("[test_svy] %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("[test_svy] all checks passed\n");
    return 0;
}
