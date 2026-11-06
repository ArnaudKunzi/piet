// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ring-1 synthetic exact-answer test for the Stata factor-variable OPERATORS
// ib#. / ibn. / i(numlist). / o. as expanded by build_model -> fvterm_columns.
//
// Fixture (group cell means are integers so every contrast is exact):
//   g=1 : y = {10, 12}            mean 11
//   g=2 : y = {20, 22, 24}        mean 22
//   g=3 : y = {30, 33, 36}        mean 33
//   g=4 : y = {40, 44}            mean 42
//   grand mean over all 10 rows = 271/10 = 27.1
//   x is a continuous covariate (used only for the o. "controlled-for" cases).
//
// Each expectation below was independently cross-checked against StataNow BE 19.5
// on auto.dta in internal note factor-operators-parity.md (the base-selection,
// last-level collinearity drop, absent-base fallback, numlist-folds-into-reference,
// and omit semantics are identical); here the cell means are integers so the
// contrasts come out to round numbers we can assert at 1e-9.
//
// Column names are Stata's factor-term display form `level.varname` (e.g. `2.g`),
// per BUG-101 (f6c323e) — the same names the eval goldens re-pinned at 4005e0d.
// (This file originally asserted the pre-BUG-101 legacy `varname==level` names;
// a stale cached test binary masked the mismatch until the clang-21 CLT update
// forced the first honest recompile — the names below are the aligned ones.)
//
// What this guards (the parity gotchas that no static fixture can fake):
//   * ib2. : base = level 2 (NOT lowest); _cons = group-2 mean.
//   * b2.  : the 'i' is optional and yields the identical model.
//   * ibn. : no base requested -> Stata drops the LAST level for collinearity;
//            _cons = last-level cell mean.
//   * ib9. : a base value ABSENT from the data does NOT error; it falls back to the
//            last-level collinearity drop -> numerically identical to ibn.
//   * i(2/4)/i(3/4) : only the listed levels get indicators (lowest selected = base);
//            rows OUTSIDE the set are NOT dropped — they fold into the reference
//            group (N is unchanged). This is the gotcha that separates the operator
//            from `keepif`.
//   * o.   : the omit operator contributes ZERO design columns and ZERO dof.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "data/frame.h"
#include "estimators/ols.h"
#include "io/columnar.h"
#include "io/win_compat.h"

using namespace cecon;

static int g_fail = 0;

static void check(const char* what, double got, double want, double tol = 1e-9) {
    double err = std::fabs(got - want);
    if (err > tol) {
        std::printf("  FAIL %-22s got=%.12g want=%.12g (err=%.3g > %.3g)\n", what, got,
                    want, err, tol);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s = %.12g\n", what, got);
    }
}
static void check_eq(const char* what, long got, long want) {
    if (got != want) {
        std::printf("  FAIL %-22s got=%ld want=%ld\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s = %ld\n", what, got);
    }
}

// Fit y ~ <terms> on the fixture and return the design column names + beta.
struct Fit {
    std::vector<std::string> names;
    std::vector<double> beta;
    int64_t n = 0;
    int k = 0;
    double coef(const char* nm) const {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == nm) return beta[i];
        std::printf("  FAIL coef '%s' MISSING from design\n", nm);
        ++g_fail;
        return std::nan("");
    }
    bool has(const char* nm) const {
        for (const auto& s : names) if (s == nm) return true;
        return false;
    }
};

static Fit fit(const Dataset& ds, const std::vector<std::string>& xterms) {
    ModelData md = build_model(ds, "y", xterms, {}, {}, "", /*drop_singleton=*/false);
    OlsResult r = ols_fit(md.dm);
    Fit f;
    f.names = md.dm.xnames;
    f.beta = r.beta;
    f.n = md.dm.n;
    f.k = md.dm.k;
    return f;
}

int test_fvops_main() {
    std::printf("test_fvops:\n");

    const std::string csv = native_temp_path("cobra_fvops.csv");
    const std::string hbin = native_temp_path("cobra_fvops.hbin");
    {
        // binary: write the fixture bytes verbatim on every platform. No-op on POSIX.
        std::ofstream f(to_native_path(csv), std::ios::binary);
        f << "y,g,x\n";
        f << "10,1,1\n12,1,2\n";
        f << "20,2,1\n22,2,3\n24,2,2\n";
        f << "30,3,1\n33,3,2\n36,3,3\n";
        f << "40,4,1\n44,4,2\n";
    }
    // g must stay NUMERIC (the factor operators read an f64 column, like Stata i.).
    Frame fr = Frame::from_csv(csv, ',', {});
    fr.write_hbin(hbin);
    Dataset ds = Dataset::open(hbin);
    check_eq("fixture n_rows", static_cast<long>(ds.n_rows()), 10);

    // ── default i.g : base = lowest (1); _cons = group-1 mean ────────────────
    {
        Fit f = fit(ds, {"i.g"});
        check_eq("i.g k", f.k, 4);                  // intercept + g2,g3,g4
        check("i.g cons", f.coef("(intercept)"), 11.0);
        check("i.g 2.g", f.coef("2.g"), 11.0);      // 22 - 11
        check("i.g 3.g", f.coef("3.g"), 22.0);      // 33 - 11
        check("i.g 4.g", f.coef("4.g"), 31.0);      // 42 - 11
        // i(g) legacy wrapper must agree with i.g exactly.
        Fit w = fit(ds, {"i(g)"});
        check("i(g)==i.g cons", w.coef("(intercept)"), 11.0);
        check("i(g)==i.g 4.g", w.coef("4.g"), 31.0);
    }

    // ── ① ib2.g : base = level 2; _cons = group-2 mean ───────────────────────
    {
        Fit f = fit(ds, {"ib2.g"});
        check_eq("ib2.g k", f.k, 4);
        check("ib2.g cons", f.coef("(intercept)"), 22.0);
        check("ib2.g 1.g", f.coef("1.g"), -11.0);    // 11 - 22
        check("ib2.g 3.g", f.coef("3.g"), 11.0);     // 33 - 22
        check("ib2.g 4.g", f.coef("4.g"), 20.0);     // 42 - 22
        if (f.has("2.g")) { std::printf("  FAIL ib2.g emitted base 2.g\n"); ++g_fail; }
    }

    // ── ① b2.g : the 'i' is optional — identical model to ib2.g ───────────────
    {
        Fit f = fit(ds, {"b2.g"});
        check("b2.g cons", f.coef("(intercept)"), 22.0);
        check("b2.g 1.g", f.coef("1.g"), -11.0);
        check("b2.g 4.g", f.coef("4.g"), 20.0);
    }

    // ── ① ibn.g : no base -> Stata drops the LAST level (4) for collinearity ──
    {
        Fit f = fit(ds, {"ibn.g"});
        check_eq("ibn.g k", f.k, 4);                 // intercept + g1,g2,g3 (g4 dropped)
        check("ibn.g cons", f.coef("(intercept)"), 42.0);  // group-4 mean
        check("ibn.g 1.g", f.coef("1.g"), -31.0);    // 11 - 42
        check("ibn.g 2.g", f.coef("2.g"), -20.0);    // 22 - 42
        check("ibn.g 3.g", f.coef("3.g"), -9.0);     // 33 - 42
        if (f.has("4.g")) { std::printf("  FAIL ibn.g kept collinear 4.g\n"); ++g_fail; }
    }

    // ── ① ib9.g : base value ABSENT from data -> fallback == ibn.g ────────────
    {
        Fit f = fit(ds, {"ib9.g"});
        check("ib9.g cons", f.coef("(intercept)"), 42.0);
        check("ib9.g 1.g", f.coef("1.g"), -31.0);
        check("ib9.g 3.g", f.coef("3.g"), -9.0);
    }

    // ── ② i(2/4).g : levels {2,3,4}, base=2; rows g=1 FOLD INTO reference ─────
    //    reference group = pooled {g=1, g=2} = {10,12,20,22,24} mean 17.6.
    {
        Fit f = fit(ds, {"i(2/4).g"});
        check_eq("i(2/4) k", f.k, 3);                // intercept + g3,g4
        check_eq("i(2/4) n (not restricted)", static_cast<long>(f.n), 10);
        check("i(2/4) cons", f.coef("(intercept)"), 17.6);  // pooled {g1,g2} mean
        check("i(2/4) 3.g", f.coef("3.g"), 15.4);    // 33 - 17.6
        check("i(2/4) 4.g", f.coef("4.g"), 24.4);    // 42 - 17.6
        if (f.has("1.g")) { std::printf("  FAIL i(2/4) emitted out-of-set 1.g\n"); ++g_fail; }
        if (f.has("2.g")) { std::printf("  FAIL i(2/4) emitted base 2.g\n"); ++g_fail; }
    }

    // ── ② i(3/4).g : base=3; pooled reference {g1,g2,g3} = {10,12,20,22,24,30,33,36}
    //    mean 187.../8 = 23.375; g4 = 42 - 23.375 = 18.625.
    {
        Fit f = fit(ds, {"i(3/4).g"});
        check_eq("i(3/4) k", f.k, 2);                // intercept + g4
        check("i(3/4) cons", f.coef("(intercept)"), 23.375);
        check("i(3/4) 4.g", f.coef("4.g"), 18.625);
    }

    // ── ② i(2 4).g : space-separated numlist {2,4}, base=2; g4 indicator only.
    //    reference = pooled {g1,g2,g3} (g1 out-of-set + g2 base + g3 out-of-set)
    //    = {10,12,20,22,24,30,33,36} mean 23.375; g4 = 42 - 23.375 = 18.625.
    {
        Fit f = fit(ds, {"i(2 4).g"});
        check_eq("i(2 4) k", f.k, 2);
        check("i(2 4) cons", f.coef("(intercept)"), 23.375);
        check("i(2 4) 4.g", f.coef("4.g"), 18.625);
        if (f.has("3.g")) { std::printf("  FAIL i(2 4) emitted out-of-set 3.g\n"); ++g_fail; }
    }

    // ── ③ o.g,x : g omitted (zero columns/dof); identical to fitting x alone ──
    {
        Fit f = fit(ds, {"o.g", "x"});
        Fit base = fit(ds, {"x"});
        check_eq("o.g,x k (no g cols)", f.k, 2);     // intercept + x only
        check("o.g,x cons", f.coef("(intercept)"), base.coef("(intercept)"));
        check("o.g,x x", f.coef("x"), base.coef("x"));
        if (f.has("2.g") || f.has("3.g") || f.has("4.g")) {
            std::printf("  FAIL o.g emitted indicator columns\n"); ++g_fail;
        }
    }

    // ── ③ o.g alone : only the intercept survives -> grand mean 27.1 ──────────
    {
        Fit f = fit(ds, {"o.g"});
        check_eq("o.g k (intercept only)", f.k, 1);
        check("o.g cons (grand mean)", f.coef("(intercept)"), 27.1);
    }

    if (g_fail) {
        std::printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
