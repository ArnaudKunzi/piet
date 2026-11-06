// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER scope-2 bundle B2 -- `oaxaca ..., by(g) pooled relax` (Jann 2008), pinned
// against live StataBE 19.5 + oaxaca 4.1.1 answer keys captured BEFORE the
// kernel was written:
//
//   tests/validate/b2_oracle.{do,json}            the twelve-paper fixture, %21x
//   internal capture           the SE investigation: 16 cases,
//                                                  full e(V)/e(V0), 48 falsified arms
//
// Every value below is an ORACLE value. The point of this file is the order of
// work: the previous pass tried five SE formulas against ONE number and all
// five were wrong by >= 5.5e-4, because `pooled` implies robust `suest` (the
// help file says so) and nobody had read that line. The pins here would have
// rejected every one of those five.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/error.h"
#include "estimators/oaxaca.h"

using namespace cecon;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool close_rel(double a, double b, double rtol) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= rtol * std::max(1.0, std::fabs(b));
}

// Stata's %21x ("+1.dfd3aa03c6bd1X+000") is a C hex-float with X for p.
double stata_hex(const char* s) {
    std::string t(s);
    const bool neg = t[0] == '-';
    t = t.substr(1);
    const auto x = t.find('X');
    const std::string mant = t.substr(0, x);
    const int exp = static_cast<int>(std::strtol(t.substr(x + 1).c_str(), nullptr, 16));
    const double v = std::strtod(("0x" + mant + "p0").c_str(), nullptr) * std::ldexp(1.0, exp);
    return neg ? -v : v;
}

// The b2 fixture, EXACTLY as tests/validate/b2_oracle.do builds it: 200 rows,
// no RNG, every term a double. `t` is part of y but NOT a regressor.
DesignMatrix b2_fixture(std::vector<int>& group, bool with_dup) {
    const int n = 200;
    DesignMatrix dm;
    dm.n = n;
    dm.k = with_dup ? 4 : 3;
    dm.xnames = with_dup ? std::vector<std::string>{"x1", "x2", "dup", "_cons"}
                         : std::vector<std::string>{"x1", "x2", "_cons"};
    dm.X.assign(static_cast<size_t>(n) * dm.k, 0.0);
    dm.y.resize(n);
    group.resize(n);
    for (int i = 0; i < n; ++i) {
        const double nn = i + 1;
        const double x1 = std::fmod(nn * 7, 23) / 23;
        const double x2 = std::fmod(nn * 13, 17) / 17;
        const int g = (i + 1) % 2;
        const double t = static_cast<double>(((i) / 3) % 2);
        double y;
        if (with_dup) {
            // rank_capture.do: y = 1+2*x1-.5*x2+.3*g+mod(_n*11,31)/31-.5
            y = 1 + 2 * x1 - .5 * x2 + .3 * g + std::fmod(nn * 11, 31) / 31 - .5;
        } else {
            // b2_oracle.do: y = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n*11,31)/31 - 0.5
            y = 1 + 2 * x1 - 0.5 * x2 + 0.3 * g + 0.25 * t + std::fmod(nn * 11, 31) / 31 - 0.5;
        }
        dm.X[i + 0 * n] = x1;
        dm.X[i + 1 * n] = x2;
        if (with_dup) {
            dm.X[i + 2 * n] = 2 * x1;
            dm.X[i + 3 * n] = 1.0;
        } else {
            dm.X[i + 2 * n] = 1.0;
        }
        dm.y[i] = y;
        group[i] = g;
    }
    return dm;
}

// ---------------------------------------------------------------------------

void test_b2_oracle() {
    std::printf("test_oaxaca / b2 fixture vs StataBE oaxaca 4.1.1 (%%21x answer key):\n");
    std::vector<int> group;
    DesignMatrix dm = b2_fixture(group, false);
    OaxacaResult r = oaxaca_fit(dm, group, true);
    check(r.n[0] == 100 && r.n[1] == 100, "e(N_1) == e(N_2) == 100");
    check(!r.omitted, "full-rank fixture: nothing omitted");
    const std::vector<std::string> names = {
        "overall:group_1", "overall:group_2", "overall:difference", "overall:explained",
        "overall:unexplained", "explained:x1",  "explained:x2",       "unexplained:x1",
        "unexplained:x2",  "unexplained:_cons"};
    check(r.names == names, "e(b) column names are Stata's eq:name spellings, in Stata's order");
    // tests/validate/b2_oracle.json  ox_<j>_<eq>_<name>__hex / oxse_<j>__hex
    static const char* kB[] = {"+1.dfd3aa03c6bd1X+000", "+1.0ce1d6da3a3c8X+001",
                               "-1.cf801d856ddf8X-003", "-1.ad4bc50474b80X-007",
                               "-1.b4ab613526940X-003", "-1.761b9ac38454fX-007",
                               "-1.b9815207831cdX-00a", "-1.0dfa20054dbdeX-004",
                               "-1.ca4ee40937313X-009", "-1.268515a25ad88X-003"};
    static const char* kSE[] = {"+1.089c453b41d7eX-004", "+1.1ae1712d9c1c8X-004",
                                "+1.8359a2f3a0017X-004", "+1.5793bde6fb7eaX-004",
                                "+1.65e987068d21dX-005", "+1.52b3bc2055e8cX-004",
                                "+1.e074e83d805a5X-007", "+1.32b7b2176a43fX-004",
                                "+1.21cd60a22be15X-004", "+1.b346909f30b5cX-004"};
    const int m = static_cast<int>(r.beta.size());
    check(m == 10, "ten columns (twofold: 5 overall + 2 explained + 3 unexplained)");
    for (int j = 0; j < m && j < 10; ++j) {
        const double b = stata_hex(kB[j]);
        const double se = stata_hex(kSE[j]);
        const double got_se = std::sqrt(r.V[j + j * m]);
        check(close_rel(r.beta[j], b, 1e-10), names[j] + " b == " + std::to_string(b));
        // The five wrong SE formulas the previous pass tried were off by 5.5e-4
        // .. 4.5e-3 on group_1; the verified construction reproduces to 1e-14.
        check(close_rel(got_se, se, 1e-10), names[j] + " se == " + std::to_string(se));
    }
    // difference == group_1 - group_2 == explained + unexplained, to the bit
    // pattern the delta rows imply (an identity, not an oracle).
    check(close_rel(r.beta[2], r.beta[0] - r.beta[1], 1e-12),
          "identity: difference == group_1 - group_2");
    check(close_rel(r.beta[2], r.beta[3] + r.beta[4], 1e-12),
          "identity: difference == explained + unexplained");
}

void test_rank_relax() {
    std::printf("test_oaxaca / duplicate regressor: relax keeps the LATER duplicate (oracle CASE 13):\n");
    std::vector<int> group;
    DesignMatrix dm = b2_fixture(group, true);
    bool threw = false;
    try {
        (void)oaxaca_fit(dm, group, false);
    } catch (const EngineError&) {
        threw = true;
    }
    check(threw, "without relax a dropped coefficient is r(499) (edges.log: RC 499)");
    OaxacaResult r = oaxaca_fit(dm, group, true);
    check(r.omitted, "relax: the omission is reported, not hidden");
    // rank_oracle.txt CASE 13: Stata drops x1 and keeps dup -- the OPPOSITE of
    // `regress`'s first-wins convention. Pinned because a pivot that keeps the
    // first duplicate reproduces overall:* exactly and gets every detailed
    // row wrong.
    const int m = static_cast<int>(r.beta.size());
    check(m == 12, "twelve columns with the duplicate present");
    check(r.beta[5] == 0.0 && r.V[5 + 5 * m] == 0.0, "explained:x1 == 0 with SE 0 (dropped)");
    check(close_rel(r.beta[7], -1.14551288790293755e-02, 1e-10), "explained:dup == -0.0114551288790294");
    check(close_rel(std::sqrt(r.V[7 + 7 * m]), 0.0829678674059004, 1e-10),
          "se(explained:dup) == 0.0829678674059004");
    check(close_rel(r.beta[0], 1.70932348816104285e+00, 1e-10), "overall:group_1 == 1.70932348816104");
    check(close_rel(std::sqrt(r.V[0]), 0.06376194159771761, 1e-10), "se(overall:group_1) == 0.0637619415977176");
    check(close_rel(r.beta[11], -2.16206589365277813e-01, 1e-10), "unexplained:_cons == -0.216206589365278");
    check(close_rel(std::sqrt(r.V[11 + 11 * m]), 0.10218531754656238, 1e-10),
          "se(unexplained:_cons) == 0.102185317546562");
}

}  // namespace

int main() {
    test_b2_oracle();
    test_rank_relax();
    if (failures) {
        std::printf("test_oaxaca: %d FAILED\n", failures);
        return 1;
    }
    std::printf("test_oaxaca: all passed\n");
    return 0;
}
