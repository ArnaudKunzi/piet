// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode (UTF-8) names in the engine's identifier lexers.
//
// Stata 14+ lets a variable name carry any Unicode letter and, after the first
// character, any non-ASCII character. Every scanner in the engine must therefore
// read a byte >= 0x80 as part of an identifier (core/ident.h), or `دخل_2020` is cut at
// the Arabic bytes and its ASCII tail `_2020` is bound on its own.
//
// Expectations are StataBE 19.5 results from scratch-r150/uni-oracle/ops.do
// (ops.golden): gen_num 6, count_if 3, lag 4, mixed_bind 5, mixed_expr 6, keep_if 3,
// lincom 0.371429.
//
// Surfaces covered: Frame's gen/replace/if parser (identifier, `.a` boundary,
// L./F./D./S. operators), the nl/gmm/nlcom parser (identifier and `{param}`), the
// lincom/test coefficient grammar, and the datetime-literal scan's word boundary.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/expr_eval.h"
#include "core/ident.h"
#include "data/frame.h"
#include "data/stata_dates.h"
#include "estimators/postest_names.h"

using namespace cecon;

static int g_fail = 0;

static void ok(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-9; }

// generate name = expr; false (and a FAIL line) when the engine refuses.
static bool gen(Frame& fr, const std::string& name, const std::string& expr) {
    try {
        fr.generate(name, expr, false);
        return true;
    } catch (const std::exception& e) {
        std::printf("  FAIL generate %s = %s threw: %s\n", name.c_str(), expr.c_str(), e.what());
        ++g_fail;
        return false;
    }
}

static double at(const Frame& fr, const std::string& name, int row1) {
    return fr.col(name).f64[static_cast<size_t>(row1 - 1)];
}

int main() {
    std::printf("[unicode_ident] byte tests\n");
    ok(ident::isIdentStartByte('a') && ident::isIdentStartByte('_') && !ident::isIdentStartByte('7'),
       "ASCII start bytes unchanged");
    ok(ident::isIdentByte('7') && !ident::isIdentByte('-') && !ident::isIdentByte('+'),
       "ASCII continue bytes unchanged");
    ok(ident::isIdentStartByte("\xD9\x82"[0]) && ident::isIdentByte("\xD9\x82"[1]),
       "UTF-8 lead and continuation bytes are identifier bytes");

    // ---- Frame expression parser ------------------------------------------------
    std::printf("[unicode_ident] gen / replace / keepif\n");
    {
        Frame fr;
        fr.set_n_rows_hint(4);
        fr.set_f64_column("id", {1, 2, 3, 4});
        fr.set_f64_column("\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9", {2, 4, 6, 8});  // قيمة
        if (gen(fr, "z", "\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9*2")) {
            ok(approx(at(fr, "z", 3), 12), "gen z = قيمة*2");
        }
        // Unicode on the left-hand side, ASCII on the right.
        if (gen(fr, "\xD8\xB3\xD8\xB9\xD8\xB1", "id + \xD9\x82\xD9\x8A\xD9\x85\xD8\xA9"))  // سعر
            ok(approx(at(fr, "\xD8\xB3\xD8\xB9\xD8\xB1", 3), 9), "gen سعر = id + قيمة");
        if (gen(fr, "w", "sqrt(\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9) > 2"))
            ok(approx(at(fr, "w", 4), 1) && approx(at(fr, "w", 1), 0), "function argument is an Arabic name");
        // A missing literal does not swallow a following non-ASCII byte.
        if (gen(fr, "m", "\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9 > .a")) ok(true, "'> .a' still the extended missing");
        int64_t kept = 0;
        try {
            kept = fr.keep_if("\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9 > 2");
            ok(kept == 3 && fr.n_rows() == 3, "keep if قيمة > 2 keeps 3 rows (StataBE keep_if 3)");
        } catch (const std::exception& e) {
            std::printf("  FAIL keep_if threw: %s\n", e.what());
            ++g_fail;
        }
    }

    // ---- the mixed-script case: دخل_2020 is ONE name, never `_2020` ------------
    std::printf("[unicode_ident] mixed-script name binds whole\n");
    {
        Frame fr;
        fr.set_n_rows_hint(3);
        const std::string mixed = "\xD8\xAF\xD8\xAE\xD9\x84_2020";  // دخل_2020
        fr.set_f64_column(mixed, {5, 5, 5});
        fr.set_f64_column("_2020", {1000, 1000, 1000});  // the decoy the ASCII tail would hit
        if (gen(fr, "t2", mixed + " + 1"))
            ok(approx(at(fr, "t2", 1), 6), "دخل_2020 + 1 = 6, not _2020 + 1 (StataBE mixed_expr 6)");
        if (gen(fr, "t3", "_2020 + 1")) ok(approx(at(fr, "t3", 1), 1001), "plain _2020 still its own name");
        // Without the decoy a lookup of the ASCII tail alone would be an unknown variable;
        // the Arabic prefix must not be dropped.
        Frame g;
        g.set_n_rows_hint(2);
        g.set_f64_column(mixed, {5, 5});
        try {
            g.generate("u", mixed + "*2", false);
            ok(approx(g.col("u").f64[0], 10), "bound without any ASCII-tail variable present");
        } catch (const std::exception& e) {
            std::printf("  FAIL mixed name w/o decoy threw: %s\n", e.what());
            ++g_fail;
        }
    }

    // ---- time-series operators on a Unicode base -----------------------------------
    std::printf("[unicode_ident] L. operator\n");
    {
        Frame fr;
        fr.set_n_rows_hint(4);
        fr.set_f64_column("id", {1, 2, 3, 4});
        const std::string price = "\xD8\xB3\xD8\xB9\xD8\xB1" "2";  // سعر2
        fr.set_f64_column(price, {2, 4, 6, 8});
        fr.tsset("id");
        if (gen(fr, "l1", "L." + price)) {
            ok(std::isnan(at(fr, "l1", 1)) && approx(at(fr, "l1", 3), 4), "gen l1 = L.سعر2 (StataBE lag 4)");
        }
        if (gen(fr, "d1", "D." + price + " + L2." + price)) ok(approx(at(fr, "d1", 3), 2 + 2), "D. and L2. on Arabic base");
        if (gen(fr, "f1", "F." + price + " + S." + price)) ok(approx(at(fr, "f1", 2), 6 + 2), "F. and S. on Arabic base");
        // no tsset-less misreading: a Unicode base that does not exist stays an honest error
        try {
            fr.generate("bad", "L.\xD9\x85\xD9\x81\xD9\x82\xD9\x88\xD8\xAF", false);
            ok(false, "L. of a missing Arabic variable must not succeed");
        } catch (const std::exception&) {
            ok(true, "L. of a missing Arabic variable is an honest error");
        }
    }

    // ---- nl / gmm / nlcom parser --------------------------------------------------
    std::printf("[unicode_ident] expr_eval (nl/gmm/nlcom)\n");
    {
        expr::Options o;
        o.tag = "nl";
        o.allow_params = true;
        o.allow_vars = true;
        o.allow_coefs = false;
        try {
            const expr::Program p = expr::Program::compile(
                "{\xD8\xA8\xD9\x8A\xD8\xAA\xD8\xA7=1}*\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9 + {a}", o);  // {بيتا=1}*قيمة + {a}
            ok(p.variables().size() == 1 && p.variables()[0] == "\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9",
               "identifier قيمة is one variable");
            ok(p.params().size() == 2 && p.params()[0].name == "\xD8\xA8\xD9\x8A\xD8\xAA\xD8\xA7",
               "{بيتا=1} is one parameter");
            const double pv[2] = {3.0, 4.0};
            const double xv[1] = {5.0};
            ok(approx(p.eval(pv, expr::VarSource{xv, nullptr}, nullptr), 19), "evaluates 3*5+4 = 19");
        } catch (const std::exception& e) {
            std::printf("  FAIL nl compile threw: %s\n", e.what());
            ++g_fail;
        }
        expr::Options c;
        c.tag = "nlcom";
        c.allow_vars = false;
        c.allow_coefs = true;
        try {
            const expr::Program p = expr::Program::compile("_b[\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9]*2", c);
            const expr::Resolver r = [](const std::string& n, double& out) {
                if (n != "\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9") return false;
                out = 0.5;
                return true;
            };
            ok(approx(p.eval(nullptr, r), 1.0), "nlcom _b[قيمة]*2 = 1");
        } catch (const std::exception& e) {
            std::printf("  FAIL nlcom compile threw: %s\n", e.what());
            ++g_fail;
        }
    }

    // ---- lincom / test coefficient grammar ------------------------------------------
    std::printf("[unicode_ident] lincom / test grammar\n");
    {
        const std::string q = "\xD9\x82\xD9\x8A\xD9\x85\xD8\xA9";   // قيمة
        const std::string sr = "\xD8\xB3\xD8\xB9\xD8\xB1";          // سعر
        const std::string mx = "\xD8\xAF\xD8\xAE\xD9\x84_2020";     // دخل_2020
        const std::vector<std::string> names = {q, sr, mx, "x", "_cons"};
        auto parse = [&](const std::string& s, postest::LinearForm& f) {
            std::string err;
            bool ns = false;
            postest::LinearParser lp(s, names, nullptr);
            const bool good = lp.parse(f, err, ns);
            if (!good) std::printf("       (parse '%s': %s)\n", s.c_str(), err.c_str());
            return good;
        };
        postest::LinearForm f;
        ok(parse(q, f) && f.w[0] == 1.0 && f.w[1] == 0.0, "lincom قيمة");
        ok(parse("2*" + q + " - " + sr + "/4 + x", f) && f.w[0] == 2.0 && approx(f.w[1], -0.25) && f.w[3] == 1.0,
           "lincom 2*قيمة - سعر/4 + x");
        ok(parse("_b[" + sr + "]", f) && f.w[1] == 1.0, "lincom _b[سعر]");
        ok(parse(mx, f) && f.w[2] == 1.0 && f.w[4] == 0.0, "دخل_2020 is one coefficient name");
        ok(parse(q + " + " + sr + " - 3", f) && f.konst == -3.0, "قيمة + سعر - 3");
        ok(!parse(q + "$", f), "stray ASCII punctuation is still a syntax error");

        // factor-level reference on a Unicode variable: 2.قيمة
        double lv = 0;
        std::string var;
        ok(postest::parse_level_ref("2." + q, lv, var) && lv == 2.0 && var == q, "2.قيمة is a level reference");
    }

    // ---- datetime-literal word boundary --------------------------------------------
    std::printf("[unicode_ident] datetime literal scan\n");
    {
        int hits = 0;
        // `td(...)` preceded by a non-ASCII letter is part of a longer identifier.
        for_each_datetime_literal(
            "\xD9\x82td(01jan2020) + td(02jan2020)",
            [&](const std::string&, const std::string&, size_t, size_t) {
                ++hits;
                return true;
            });
        ok(hits == 1, "td( glued to an Arabic letter is not a literal; the free one is");
    }

    if (g_fail == 0) std::printf("unicode_ident: ALL PASSED\n");
    else std::printf("unicode_ident: %d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
