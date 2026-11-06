// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// COUNCIL-20260710 pre-wave audit — correctness fixes spanning
// apps/cecon_cli.cpp (CLI orchestration) + src/core/model.cpp (build_model /
// build_iv_model):
//
//   E1 (BUG-054) TS operators (L./F./D./S.) must materialize against the
//       FULL tsset series BEFORE an `if`/keepif sample restriction narrows
//       the rows — Stata resolves L./F./D./S. against the whole dataset in
//       memory; `if` only selects which of those already-computed rows enter
//       the estimation sample. The old cobra flow (apps/cecon_cli.cpp's
//       maybe_keepif) instead wrote a physically row-reduced scratch .hbin
//       BEFORE build_model's internal ts_prepass ever ran, so a boundary
//       row's lag/lead lookup could miss a neighbor that had already been
//       deleted and spuriously came back missing.
//       Fix: build_model / build_iv_model (core/model.h/.cpp) gained an
//       optional trailing `row_mask` parameter, applied AFTER their existing
//       ts_prepass call (which still runs against the untouched, FULL `ds`)
//       — so a caller can express an `if` restriction WITHOUT pre-filtering
//       the dataset ts_prepass will see. apps/cecon_cli.cpp's plan_keepif()
//       / eval_keepif_mask() route `--keepif` through this new parameter
//       exactly when the varlist actually carries a TS token (cecon::
//       split_ts, also exposed here); every other keepif call keeps the old
//       maybe_keepif() physical pre-filter, byte-identical to before.
//       This file exercises build_model's row_mask contract directly (the
//       CLI orchestration functions themselves are apps/cecon_cli.cpp
//       anonymous-namespace internals with no external linkage to test
//       against). Fixture mirrors e1_tsif.csv / oracle_E1_tsif.csv exactly
//       (StataBE 19.5, seed 20260710): t=1..30, x=20+0.8t+e, y=10+0.5x+e.
//       `regress y L.x if t>=15` keeps n=16 in Stata (row t=15's L.x =
//       x[14], a real value sourced from a row the `if` itself excludes
//       from the estimation sample).
//
//   E6 (BUG-060) CLI unknown-flag rejection: the opt()-based argument
//       harness in apps/cecon_cli.cpp silently ignored any --flag a command
//       didn't explicitly look up (e.g. `regress ..., floob` / a typo'd
//       flag). opt()/has()/opt_all() now track which argv indices they
//       actually matched (g_argv_consumed); dispatch_guard() rejects any
//       leftover "--xxx" token as bad_input after a command returns rc==0
//       (cmd_eval is a documented exception — its own hand-rolled, non-opt()
//       argv scanner for --gen/--egen/... is a separate mini-language, not
//       covered by this net). ALL of this lives in apps/cecon_cli.cpp's
//       anonymous namespace (an executable's internals, no external linkage
//       for a unit test to link against), so E6 is covered end-to-end via
//       the CLI instead: see tests/validate/validate_e6_unknown_flag.py
//       (ctest target e6_unknown_flag_cli) — a bogus --floob is rejected
//       (bad_input, naming it) both standalone and appended after an
//       otherwise-valid invocation, while every real flag (including one
//       only read inside a conditional branch, e.g. --cluster under --vcov
//       cluster) keeps working unchanged.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/error.h"
#include "core/model.h"
#include "data/frame.h"
#include "estimators/fepois.h"
#include "estimators/feols.h"
#include "estimators/iv2sls.h"
#include "estimators/nlgmm.h"
#include "estimators/sunab.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;

int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
}
static bool eq(double a, double b, double tol) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) != std::isnan(b)) return false;
    return std::fabs(a - b) <= tol * (std::fabs(b) + 1.0);
}

// e1_tsif.csv (StataBE 19.5, `set seed 20260710`; oracle_prewave.do's
// E1_TSIF block): t=1..30, x=20+0.8*t+rnormal()*3, y=10+0.5*x+rnormal()*2.
static const double kT[30] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30
};
static const double kX[30] = {
    26.03879885899423, 20.04428951026811, 22.56849059797331, 25.7410184914559,
    26.13629775834672, 23.31359435682403, 26.12043498991301, 23.29796216315577,
    29.37354746238571, 27.77442358554669, 31.61946813453336, 29.0821155851406,
    26.39562418619225, 29.20788970406495, 33.89332601844433, 35.36767552834765,
    35.38473773568924, 31.83070317627216, 34.72045829056042, 36.66357254043857,
    32.48947362003297, 39.73656825673168, 35.31930371473963, 35.8820270238709,
    44.60316724872964, 43.37810433949315, 41.74631727994009, 46.40777890491826,
    40.40661577137622, 41.39925782107988
};
static const double kY[30] = {
    22.79116882303877, 17.51112101544541, 22.09094979176145, 21.94894672779726,
    21.24083254862433, 20.8795772460828,  26.27942388160722, 24.53714828038399,
    20.54361809327535, 25.34463537523246, 30.55828327349686, 20.51793168635252,
    22.86827491910109, 23.36233209244502, 30.5495935543353,  30.8784709004164,
    24.54609140279044, 25.15831452326364, 26.99012362649994, 27.38621697238114,
    25.42605717442627, 31.91386461683371, 27.6665362728581,  27.51237830506733,
    33.0853489418549,  32.82534831060268, 33.45011561471951, 33.58042662857235,
    31.43527836344357, 30.52267996192534
};

static Dataset make_e1_dataset(const std::string& path) {
    Frame f;
    f.set_n_rows_hint(30);
    f.append_f64_column("t", std::vector<double>(kT, kT + 30));
    f.append_f64_column("x", std::vector<double>(kX, kX + 30));
    f.append_f64_column("y", std::vector<double>(kY, kY + 30));
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

// E1 — filtered+TS: `regress y L.x if t>=15` must materialize L.x against the
// FULL 30-row series, THEN apply `if`. Oracle (oracle_E1_tsif.csv, StataBE
// 19.5, seed 20260710): n_obs=16, b_Lx=0.268632350019616,
// b_cons=19.5339499764621, se_Lx=0.155700312611587, se_cons=5.856575182963700.
static void test_e1_ts_if_ordering() {
    std::printf("E1 TS operator materializes before `if` (BUG-054):\n");
    const std::string path = native_temp_path("cecon_test_prewave_e1.hbin");
    Dataset ds = make_e1_dataset(path);

    TsMeta tsm;
    tsm.timevar = "t";  // single series, no panel

    // row_mask for `if t>=15`: rows are 0-indexed for t=1..30, so t>=15 is
    // positions 14..29 (16 rows) — hand-built the same way
    // apps/cecon_cli.cpp's eval_keepif_mask would (it reuses Frame::keep_if
    // under the hood), but independent of that code so this test exercises
    // build_model's row_mask contract directly, not the CLI's mask plumbing.
    std::vector<char> mask(30, 0);
    for (int i = 0; i < 30; ++i)
        if (kT[static_cast<size_t>(i)] >= 15) mask[static_cast<size_t>(i)] = 1;

    ModelData md = build_model(ds, "y", {"L.x"}, {}, {}, "", true, &tsm, &mask);
    check(md.dm.n == 16,
          "filtered+TS: n_obs == 16 (materialize-first; filter-first would wrongly give 15)");

    FeolsResult r = feols_fit(md, Vcov::IID);
    int iLx = -1, icons = -1;
    for (int j = 0; j < r.k; ++j) {
        if (r.names[static_cast<size_t>(j)] == "L.x") iLx = j;
        else if (r.names[static_cast<size_t>(j)] == "(intercept)") icons = j;
    }
    check(iLx >= 0 && icons >= 0,
          "filtered+TS: coeftable has L.x and (intercept) (ts_prepass's synthetic name "
          "was restored to the readable 'L.x' via restore_ts_names, unaffected by row_mask)");
    if (iLx >= 0 && icons >= 0) {
        check(eq(r.beta[static_cast<size_t>(iLx)], 0.268632350019616, 1e-8),
              "filtered+TS: b_Lx matches Stata oracle");
        check(eq(r.beta[static_cast<size_t>(icons)], 19.5339499764621, 1e-8),
              "filtered+TS: b_cons matches Stata oracle");
        check(eq(r.se[static_cast<size_t>(iLx)], 0.155700312611587, 1e-7),
              "filtered+TS: se_Lx matches Stata oracle");
        check(eq(r.se[static_cast<size_t>(icons)], 5.856575182963700, 1e-7),
              "filtered+TS: se_cons matches Stata oracle");
    }

    // Unfiltered path must be UNCHANGED: only t=1's L.x is undefined (no t=0
    // predecessor exists in the 30-row series), so n_obs == 29 regardless of
    // whether row_mask is omitted, an explicit null pointer, or an
    // all-rows-true mask — all three must agree EXACTLY (byte-identical
    // design matrix / outcome / kept-row indices), proving the new row_mask
    // parameter is fully inert unless it actually excludes a row (the "0
    // re-pin on existing TS goldens" contract for every no-`if` TS caller).
    ModelData md_default = build_model(ds, "y", {"L.x"}, {}, {}, "", true, &tsm);  // old call shape (row_mask omitted)
    ModelData md_null = build_model(ds, "y", {"L.x"}, {}, {}, "", true, &tsm, nullptr);
    std::vector<char> all_true(30, 1);
    ModelData md_alltrue = build_model(ds, "y", {"L.x"}, {}, {}, "", true, &tsm, &all_true);

    check(md_default.dm.n == 29, "unfiltered: n_obs == 29 (only t=1's lag is undefined)");
    check(md_null.dm.n == md_default.dm.n,
          "unfiltered: explicit row_mask=nullptr matches the row_mask-omitted call");
    check(md_alltrue.dm.n == md_default.dm.n,
          "unfiltered: an all-true row_mask matches no mask at all");
    check(md_default.keep == md_null.keep && md_default.keep == md_alltrue.keep,
          "unfiltered: identical kept-row indices across all three calls");
    check(md_default.dm.X == md_null.dm.X && md_default.dm.X == md_alltrue.dm.X,
          "unfiltered: byte-identical design matrix across all three calls");
    check(md_default.dm.y == md_null.dm.y && md_default.dm.y == md_alltrue.dm.y,
          "unfiltered: byte-identical outcome vector across all three calls");

    native_unlink(path);
}

// split_ts (core/model.h) — the TS-token detector apps/cecon_cli.cpp's
// plan_keepif() uses to decide whether an `if`-bearing estimator call needs
// E1's row_mask routing at all (vs. falling back to the plain maybe_keepif()
// physical pre-filter). Exercises the exact token shapes feols/iv varlists
// can carry: bare, numbered, composed, and factor-prefixed.
static void test_e1_split_ts_detector() {
    std::printf("E1 split_ts token detector:\n");
    check(split_ts("L.x").has_ts, "split_ts: bare L.x detected");
    check(split_ts("L.x").ts_token == "L.x" && split_ts("L.x").fac_prefix.empty(),
          "split_ts: bare L.x token/prefix");
    check(split_ts("L2.x").has_ts, "split_ts: L2.x detected");
    check(split_ts("F.x").has_ts, "split_ts: F.x detected");
    check(split_ts("D.x").has_ts, "split_ts: D.x detected");
    check(split_ts("S2.x").has_ts, "split_ts: S2.x detected");
    check(split_ts("L.D.x").has_ts, "split_ts: composed L.D.x detected");
    TsSplit fac = split_ts("i.L.gdp");
    check(fac.has_ts && fac.fac_prefix == "i." && fac.ts_token == "L.gdp" && fac.base_var == "gdp",
          "split_ts: factor-prefixed i.L.gdp splits into {i., L.gdp, gdp}");
    check(!split_ts("x").has_ts, "split_ts: plain 'x' has no TS operator");
    check(!split_ts("i.group").has_ts, "split_ts: plain factor i.group has no TS operator");
}

// Weighted fixed-effect absorption (feols_fit with md.weights AND md.fe).
// Oracle: tests/eval/golden/aer_w2/w2_pweight.log (StataBE 19.5; do-file beside
// it). PVAL lines are %21x hex floats, pinned here as C++17 hex literals.
// Fixture (all exact in double): 120 rows, id=ceil(_n/8), t=mod(_n-1,8)+1,
// x1=mod(_n*7,11)/3+id/5, x2=mod(_n*13,17)/4-t/3,
// u=(mod(_n*37,23)-11)/6*(1+x1/3), y=1+2*x1-x2+id/4+u, w=1+mod(id*3,5)
// (constant within id), wv=1+mod(_n*3,5) (varies WITHIN id).
// Tolerances (tests/eval/TOLERANCE.md): T1 1e-12 for OLS b, T2 1e-9 for
// se/F/r2/rmse, relative with a unit atol floor (eq() above).
static Dataset make_w2_pweight_dataset(const std::string& path) {
    const int N = 120;
    std::vector<double> id(N), t(N), x1(N), x2(N), y(N), w(N), wv(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;  // Stata _n
        id[i] = std::ceil(n / 8.0);
        t[i] = static_cast<double>((n - 1) % 8 + 1);
        x1[i] = static_cast<double>((n * 7) % 11) / 3.0 + id[i] / 5.0;
        x2[i] = static_cast<double>((n * 13) % 17) / 4.0 - t[i] / 3.0;
        const double u = (static_cast<double>((n * 37) % 23) - 11.0) / 6.0 * (1.0 + x1[i] / 3.0);
        y[i] = 1.0 + 2.0 * x1[i] - x2[i] + id[i] / 4.0 + u;
        w[i] = 1.0 + static_cast<double>((static_cast<int>(id[i]) * 3) % 5);
        wv[i] = 1.0 + static_cast<double>((n * 3) % 5);
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("t", t);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.append_f64_column("w", w);
    f.append_f64_column("wv", wv);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

static int coef_index(const FeolsResult& r, const std::string& name) {
    for (int j = 0; j < r.k; ++j)
        if (r.names[static_cast<size_t>(j)] == name) return j;
    return -1;
}

// One fitted arm vs its oracle PVAL line. NaN = "not pinned for this arm".
struct WfeArm {
    const char* tag;
    double b1, se1, b0, se0, F, r2, rmse;
};
static void check_wfe_arm(const FeolsResult& r, const WfeArm& o, bool r2_is_within) {
    const std::string tag = o.tag;
    // _cons is the GAP-B intercept under FE, the design's "(intercept)" without.
    const int i1 = coef_index(r, "x1");
    const int i0 = coef_index(r, "_cons") >= 0 ? coef_index(r, "_cons") : coef_index(r, "(intercept)");
    check(i1 >= 0 && i0 >= 0, tag + ": coeftable has x1 and the intercept");
    if (i1 < 0 || i0 < 0) return;
    const size_t j1 = static_cast<size_t>(i1), j0 = static_cast<size_t>(i0);
    check(eq(r.beta[j1], o.b1, 1e-12), tag + ": b[x1] (T1)");
    if (!std::isnan(o.se1)) check(eq(r.se[j1], o.se1, 1e-9), tag + ": se[x1] (T2)");
    if (!std::isnan(o.b0)) check(eq(r.beta[j0], o.b0, 1e-12), tag + ": b[_cons] (T1)");
    if (!std::isnan(o.se0)) check(eq(r.se[j0], o.se0, 1e-9), tag + ": se[_cons] (T2)");
    if (!std::isnan(o.F)) check(eq(r.f_stat, o.F, 1e-9), tag + ": F (T2)");
    if (!std::isnan(o.r2))
        check(eq(r2_is_within ? r.r2_within : r.r2, o.r2, 1e-9),
              tag + (r2_is_within ? ": r2_within (xtreg e(r2)) (T2)" : ": full-model r2 (T2)"));
    if (!std::isnan(o.rmse)) check(eq(r.rmse, o.rmse, 1e-9), tag + ": rmse/sigma_e (T2)");
}

static void test_weighted_fe_absorption() {
    std::printf("weighted fixed-effect absorption (w2_pweight.log):\n");
    const std::string path = native_temp_path("cecon_test_prewave_wfe.hbin");
    Dataset ds = make_w2_pweight_dataset(path);
    const double NA = std::nan("");

    // X0_fe (w2_pweight.log:263-266): the UNweighted control, conventional VCE.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {});
        FeolsResult r = feols_fit(md, Vcov::IID);
        check_wfe_arm(r, {"X0_fe", 0x1.02669520b20f0p+1, 0x1.c1a27db214f0cp-3,
                          0x1.72e23f7072902p+1, 0x1.849a0c2f15d1ep-1, 0x1.de8d602b541bbp+5,
                          0x1.132199730c722p-1, 0x1.3fd2fcf82c465p+1}, true);
        check(r.dof.dof_resid == 103, "X0_fe: df_r == 103");
    }
    // X3_aw_fe (349-352): xtreg y x1 x2 [aweight=w], fe — conventional VCE,
    // N=120, df_r=103; xtreg's e(r2) is the within R², e(sigma_e) the rmse.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Analytic);
        FeolsResult r = feols_fit(md, Vcov::IID);
        check_wfe_arm(r, {"X3_aw_fe", 0x1.d0bd0e411b74ap+0, 0x1.c3346b30c1b3dp-3,
                          0x1.bfa2a9248e6e0p+1, 0x1.7e19f804a80fbp-1, 0x1.8f731027c1e90p+5,
                          0x1.f814bbf95f9c2p-2, 0x1.4013601975c1dp+1}, true);
        check(r.n_eff == 120.0 && r.dof.dof_resid == 103, "X3_aw_fe: N == 120, df_r == 103");
    }
    // X4_pw_fe == H1_reghdfe_pw_clusterid_SSC (378-381, 754-761): pweight means
    // cluster on id at the engine level. df_r = G-1 = 14, N_clust = 15, r2 is the
    // FULL-model r2 (H1). Pinned against H1's line.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id"}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Probability);
        FeolsResult r = feols_fit(md, Vcov::Cluster);
        check_wfe_arm(r, {"H1/X4_pw_fe_cluster_id", 0x1.d0bd0e411b748p+0, 0x1.b315e83b84cedp-3,
                          0x1.bfa2a9248e6e0p+1, 0x1.4c1c9eb74cf77p-1, 0x1.416ef8f9a343dp+5,
                          0x1.68d8a360889fcp-1, 0x1.4013601975c1dp+1}, false);
        check(r.df_r == 14 && r.n_clusters == 15, "H1/X4: df_r == 14, N_clust == 15");
    }
    // X14_fw_fe (607): fweight, N = Σw = 360, conventional VCE.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Frequency);
        FeolsResult r = feols_fit(md, Vcov::IID);
        check_wfe_arm(r, {"X14_fw_fe", 0x1.d0bd0e411b752p+0, 0x1.ee829a04bb9cap-4,
                          NA, NA, NA, NA, NA}, false);
        check(r.n_eff == 360.0, "X14_fw_fe: N == 360 (sum of w)");
        check(r.dof.dof_resid == 360 - 2 - 15, "X14_fw_fe: df_r == N - k - k_fe == 343");
    }
    // A1_areg_pw (660-666): areg y x1 x2 [pweight=w], absorb(id) — robust (HC1)
    // with areg's dof: N=120, df_r=103.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Probability);
        FeolsResult r = feols_fit(md, Vcov::HC1);
        check_wfe_arm(r, {"A1_areg_pw", 0x1.d0bd0e411b74ap+0, 0x1.e178af045346fp-3,
                          0x1.bfa2a9248e6e0p+1, 0x1.5fe032792f599p-1, 0x1.461a5dca01f62p+5,
                          0x1.68d8a360889fcp-1, 0x1.4013601975c1dp+1}, false);
        check(r.df_r == 103, "A1_areg_pw: df_r == 103");
    }
    // fweight + robust (BUG-644). A frequency weight is w replicated rows, so its
    // HC1 meat is Σ w e² xx', not the pweight Σ w² e² xx'. No golden carries this
    // arm yet; the numbers are a StataBE 19.5 capture on this same fixture
    // (`areg y x1 x2 [fw=w], absorb(id) vce(robust)` and
    // `regress y x1 x2 [fw=w], vce(robust)`), and Stata reports the same se[x1]
    // after `expand w` with no weight (0x1.e603349ddddadp-4 / 0x1.ae5e0af6def2ep-4).
    // The old pweight meat gives se[x1] 0.2231 (FE) and 0.2030 (no FE), so both
    // pins fail on it.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Frequency);
        FeolsResult r = feols_fit(md, Vcov::HC1);
        check_wfe_arm(r, {"areg_fw_robust", 0x1.d0bd0e411b752p+0, 0x1.e603349ddddfep-4,
                          NA, 0x1.6d41cf09a568fp-2, NA, 0x1.68d8a360889fcp-1,
                          0x1.2fcc4792e9a6ap+1}, false);
        check(r.n_eff == 360.0 && r.df_r == 343, "areg_fw_robust: N == 360, df_r == 343");
    }
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Frequency);
        FeolsResult r = feols_fit(md, Vcov::HC1);
        check_wfe_arm(r, {"regress_fw_robust", 0x1.30d609a55b2d1p+1, 0x1.ae5e0af6def2fp-4,
                          NA, 0x1.3e7d4ee180dd2p-2, 0x1.2a24fb9effad3p+8,
                          0x1.49e2049cf95f2p-1, 0x1.46dc891af001cp+1}, false);
        check(r.n_eff == 360.0 && r.df_r == 357, "regress_fw_robust: N == 360, df_r == 357");
    }
    // A3_areg_pw_VARYING_w_clusterid (719-723): wv varies WITHIN id, so the FE cell
    // means must be the weighted ones. b, r2, rmse only here (default dof); its
    // clustered SE is pinned under areg's convention in test_areg_cluster_dof.
    const double kA3b = 0x1.e84a361e3023bp+0;
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id"}, "", true, nullptr,
                                   nullptr, "wv", WeightKind::Probability);
        FeolsResult r = feols_fit(md, Vcov::Cluster);
        check_wfe_arm(r, {"A3_areg_pw_VARYING_w", kA3b, NA, 0x1.9bad2a00d1620p+1, NA, NA,
                          0x1.72fbdf8a8ea1ep-1, 0x1.3077ca670b855p+1}, false);
    }
    // Inverted arm: the OLD order (scale rows by √w̃, then demean UNweighted) must
    // miss A3's b[x1]. Rebuild it by hand on an unweighted ModelData so this arm
    // keeps failing the old order even after feols_fit no longer contains it.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "wv", WeightKind::Probability);
        std::vector<double> wts = md.weights;
        md.weights.clear();
        md.weight_kind = WeightKind::None;
        const int64_t n = md.dm.n;
        double wsum = 0.0;
        for (int64_t i = 0; i < n; ++i) wsum += wts[static_cast<size_t>(i)];
        for (int64_t i = 0; i < n; ++i) {
            const double s = std::sqrt(wts[static_cast<size_t>(i)] * static_cast<double>(n) / wsum);
            md.dm.y[static_cast<size_t>(i)] *= s;
            for (int j = 0; j < md.dm.k; ++j) md.dm.X[static_cast<size_t>(j) * n + i] *= s;
        }
        FeolsResult old = feols_fit(md, Vcov::IID);
        const int i1 = coef_index(old, "x1");
        const double b_old = i1 >= 0 ? old.beta[static_cast<size_t>(i1)] : NA;
        std::printf("  inverted arm: scale-then-demean b[x1] = %.17g vs oracle %.17g\n", b_old, kA3b);
        check(i1 >= 0 && !eq(b_old, kA3b, 1e-3),
              "A3 inverted: the scale-then-demean order misses the oracle b[x1] "
              "(the arm above discriminates the order)");
    }
    // w constant within id: the old order was only accidentally right there — the
    // new order must agree with it (X3's b), a control that the inverted arm's
    // miss comes from within-cell weight variation, not from the rebuild itself.
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Analytic);
        std::vector<double> wts = md.weights;
        md.weights.clear();
        md.weight_kind = WeightKind::None;
        const int64_t n = md.dm.n;
        for (int64_t i = 0; i < n; ++i) {
            const double s = std::sqrt(wts[static_cast<size_t>(i)] * 120.0 / 360.0);
            md.dm.y[static_cast<size_t>(i)] *= s;
            for (int j = 0; j < md.dm.k; ++j) md.dm.X[static_cast<size_t>(j) * n + i] *= s;
        }
        FeolsResult old = feols_fit(md, Vcov::IID);
        const int i1 = coef_index(old, "x1");
        check(i1 >= 0 && eq(old.beta[static_cast<size_t>(i1)], 0x1.d0bd0e411b74ap+0, 1e-12),
              "inverted-arm control: with w constant within id the old order hits X3's b");
    }
    native_unlink(path);
}

// w2b_estim.do's fixture (tests/eval/golden/aer_w2/w2b_estim.do): 60 rows,
// id=ceil(_n/6), t=mod(_n-1,6)+1. The do-file generates FLOAT (not double)
// columns, so each value is rounded to float storage exactly as Stata's
// `generate` does (id and t are small integers, exact either way).
static Dataset make_w2b_estim_dataset(const std::string& path) {
    const int N = 60;
    auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    std::vector<double> id(N), t(N), x1(N), x2(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        t[i] = static_cast<double>((n - 1) % 6 + 1);
        x1[i] = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
        x2[i] = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
        y[i] = fl(1.0 + 2.0 * x1[i] - x2[i] + id[i] / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("t", t);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

// Control for A1's small-sample factor: the UNweighted `areg y x1 x2, a(id) r`
// (tests/eval/golden/aer_w2/w2b_estim.log:159, se[x1] .0386296057774394, %18.0g).
static void test_areg_robust_unweighted_control() {
    std::printf("areg robust unweighted control (w2b_estim.log areg_a_r):\n");
    const std::string path = native_temp_path("cecon_test_prewave_areg_r.hbin");
    Dataset ds = make_w2b_estim_dataset(path);
    ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {});
    FeolsResult r = feols_fit(md, Vcov::HC1);
    const int i1 = coef_index(r, "x1");
    check(i1 >= 0 && eq(r.se[static_cast<size_t>(i1)], .0386296057774394, 1e-9),
          "areg_a_r: unweighted areg robust se[x1] (T2)");
    native_unlink(path);
}

// BUG-568: areg's clustered dof. feols_fit(..., keep_nested_fe_dof=true) charges
// the FE nested in the cluster (areg); the default exempts it (reghdfe/xtreg,fe).
// Oracles: w2b_estim.log (%18.0g) areg_a_cl / areg_vce_cluster / reghdfe_cluster_ctl
// / areg_cluster_nonnested, and w2_pweight.log (%21x) A0 / A2 / A3 / H1.
static void test_areg_cluster_dof() {
    std::printf("areg clustered dof (w2b_estim.log, w2_pweight.log A0/A2/A3 vs H1):\n");
    {
        const std::string path = native_temp_path("cecon_test_prewave_areg_cl.hbin");
        Dataset ds = make_w2b_estim_dataset(path);
        auto fit = [&](const char* cl, bool areg) {
            ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {cl});
            return feols_fit(md, Vcov::Cluster, 0, false, areg);
        };
        FeolsResult a = fit("id", true);
        const int i1 = coef_index(a, "x1"), i0 = coef_index(a, "_cons");
        check(i1 >= 0 && i0 >= 0, "areg_a_cl: coeftable has x1 and _cons");
        if (i1 >= 0 && i0 >= 0) {
            check(eq(a.se[static_cast<size_t>(i1)], .0722686736191015, 1e-9),
                  "areg_a_cl: se[x1] .0722686736191015 charges the nested id FE (T2)");
            check(eq(a.se[static_cast<size_t>(i0)], .4823829775414377, 1e-9),
                  "areg_vce_cluster: se[_cons] .4823829775414377 (T2)");
        }
        check(a.n_clusters == 10 && a.df_r == 9 && a.nested_fe.empty(),
              "areg_a_cl: N_clust 10, df_r 9 (G-1), nothing exempted");
        FeolsResult h = fit("id", false);
        const int h1 = coef_index(h, "x1");
        check(h1 >= 0 && eq(h.se[static_cast<size_t>(h1)], .066318283176713, 1e-9),
              "reghdfe_cluster_ctl (control): the default still exempts the nested FE, "
              "se[x1] .066318283176713 (T2)");
        check(h.df_r == 9 && h.nested_fe.size() == 1, "reghdfe_cluster_ctl: df_r 9, id exempted");
        for (bool areg : {true, false}) {
            FeolsResult nn = fit("t", areg);
            const int j = coef_index(nn, "x1");
            check(j >= 0 && eq(nn.se[static_cast<size_t>(j)], .0214849883739092, 1e-9) &&
                      nn.df_r == 5,
                  std::string("areg_cluster_nonnested (") + (areg ? "areg" : "default") +
                      "): se[x1] .0214849883739092, df_r 5 -- nothing nested, both agree (T2)");
        }
        native_unlink(path);
    }
    {
        const std::string path = native_temp_path("cecon_test_prewave_areg_clw.hbin");
        Dataset ds = make_w2_pweight_dataset(path);
        const double NA = std::nan("");
        {
            ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id"});
            FeolsResult r = feols_fit(md, Vcov::Cluster, 0, false, true);
            check_wfe_arm(r, {"A0_areg_unweighted_clusterid", 0x1.02669520b20f0p+1,
                              0x1.1d41125f9cb99p-2, 0x1.72e23f7072902p+1, 0x1.c749b407eba18p-1,
                              NA, NA, 0x1.3fd2fcf82c465p+1}, false);
            check(r.df_r == 14 && r.n_clusters == 15, "A0: df_r == 14, N_clust == 15");
        }
        {
            ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id"}, "", true, nullptr,
                                       nullptr, "w", WeightKind::Probability);
            FeolsResult r = feols_fit(md, Vcov::Cluster, 0, false, true);
            check_wfe_arm(r, {"A2_areg_pw_clusterid", 0x1.d0bd0e411b74ap+0, 0x1.cfb6746651353p-3,
                              0x1.bfa2a9248e6e0p+1, 0x1.61f6af6a6958dp-1, NA,
                              0x1.68d8a360889fcp-1, 0x1.4013601975c1dp+1}, false);
            check(r.df_r == 14 && r.n_clusters == 15, "A2: df_r == 14, N_clust == 15");
        }
        {
            ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id"}, "", true, nullptr,
                                       nullptr, "wv", WeightKind::Probability);
            FeolsResult r = feols_fit(md, Vcov::Cluster, 0, false, true);
            check_wfe_arm(r, {"A3_areg_pw_VARYING_w_clusterid", 0x1.e84a361e3023bp+0,
                              0x1.31cf90b884108p-2, 0x1.9bad2a00d1620p+1, 0x1.ed54b6d900724p-1,
                              NA, 0x1.72fbdf8a8ea1ep-1, 0x1.3077ca670b855p+1}, false);
        }
        native_unlink(path);
    }
}

// FU-365: tests/eval/golden/aer_w2/w2l_vce_shape_det.do lines 11-21 (N=180; G_id 30,
// G_id2 7, id x id2 all singletons). Every value is a small dyadic rational, exact in
// double and in Stata's float storage, so no float rounding is applied.
// w2l_vce_shape_det.do:8-21 (w2s_multiway_class.do:12-25 copies it), plus the
// variables w2s_multiway_class.do:31-35 adds: w (varies within id), wid (constant
// within id), cg1 = ceil(id/5) and cg2 = mod(id,5)+1 (both nest id), and t, z1.
// Every value is dyadic, so float and double storage agree.
static Dataset make_w2l_vce_shape_det_dataset(const std::string& path) {
    const int N = 180;
    std::vector<double> id(N), id2(N), x1(N), x2(N), y(N);
    std::vector<double> t(N), z1v(N), w(N), wid(N), cg1(N), cg2(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        id2[i] = static_cast<double>(n % 7 + 1);
        t[i] = static_cast<double>((n - 1) % 6 + 1);
        w[i] = 1.0 + static_cast<double>((n * 3) % 7) / 4.0;
        wid[i] = 1.0 + static_cast<double>(static_cast<int>(id[i]) % 4) / 4.0;
        cg1[i] = std::ceil(id[i] / 5.0);
        cg2[i] = static_cast<double>(static_cast<int>(id[i]) % 5 + 1);
        x1[i] = static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 8.0;
        const double z1 = static_cast<double>((n * 5) % 13) + x1[i] / 2.0 +
                          static_cast<double>((n * 17) % 19) / 16.0;
        z1v[i] = z1;
        x2[i] = 0.5 * z1 + static_cast<double>((n * 3) % 5) +
                static_cast<double>((n * 13) % 7) / 8.0 + static_cast<double>((n * 23) % 29) / 32.0;
        y[i] = 1.0 + 2.0 * x1[i] - x2[i] + id[i] / 4.0 + static_cast<double>((n * 11) % 17) / 4.0 +
               (static_cast<double>((n * 37) % 41) / 64.0 + static_cast<double>((n * 53) % 59) / 64.0 +
                static_cast<double>((n * 29) % 31) / 64.0 - 1.0) * 3.0;
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("id2", id2);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.append_f64_column("t", t);
    f.append_f64_column("z1", z1v);
    f.append_f64_column("w", w);
    f.append_f64_column("wid", wid);
    f.append_f64_column("cg1", cg1);
    f.append_f64_column("cg2", cg2);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    return Dataset::open(path);
}

static bool rel_eq(double a, double b, double rtol) {
    return std::isfinite(a) && std::fabs(a - b) <= rtol * std::fabs(b);
}

// FU-365: Stata's built-in multiway cluster for regress/areg ([R] regress, Methods and
// formulas, Multiway clustering): per-combination G/(G-1)*(N-1)/(N-K), then the negative
// eigenvalues of the symmetrised sum set to 0 -- feols_fit(..., multiway_subset_factors).
// Oracle w2l_vce_shape_det.log (%18.0g): regress_vce_c2_se :258 / areg_vce_c2_se :614,
// the coefficient tables :242-244 / :600-602 (%9.0g) and F(2, 6) :233 / :589; reghdfe's
// Gmin factor (the default) is the control, reghdfe_vce_c2_se :3825.
static void test_multiway_subset_factors() {
    std::printf("multiway cluster, Stata built-in convention (w2l_vce_shape_det.log):\n");
    const std::string path = native_temp_path("cecon_test_prewave_mw_subset.hbin");
    Dataset ds = make_w2l_vce_shape_det_dataset(path);
    auto se_of = [](const FeolsResult& r, const char* nm) {
        const int j = coef_index(r, nm);
        return j >= 0 ? r.se[static_cast<size_t>(j)] : std::nan("");
    };
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {"id", "id2"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, false, true);
        check(rel_eq(se_of(r, "x1"), .0643525900770342, 1e-9),
              "regress_vce_c2_se :258: se[x1] .0643525900770342 (rtol 1e-9; Gmin rule gave "
              ".0638929607)");
        check(std::fabs(se_of(r, "x2") - .0215361) <= 5e-8 &&
                  std::fabs(se_of(r, "(intercept)") - .4470232) <= 5e-8,
              "regress_vce_c2 table :243-244: se[x2] .0215361, se[_cons] .4470232 (%9.0g)");
        check(std::fabs(r.f_stat - 1538.22) <= 5e-3, "regress_vce_c2 :233: F(2, 6) = 1538.22");
        check(r.df_r == 6 && r.vcov_npd_fix,
              "regress_vce_c2: df_r 6 (min over combinations - 1), the clip fired (:228 note)");
    }
    {
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "id2"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, true, true);
        check(rel_eq(se_of(r, "x1"), .0778721308909151, 1e-9),
              "areg_vce_c2_se :614: se[x1] .0778721308909151 (rtol 1e-9; Gmin rule gave "
              ".0777683042)");
        check(std::fabs(se_of(r, "x2") - .0621976) <= 5e-8 &&
                  std::fabs(se_of(r, "_cons") - .4840768) <= 5e-8,
              "areg_vce_c2 table :601-602: se[x2] .0621976, se[_cons] .4840768 -- the clip ran "
              "on the (slopes, _cons) matrix");
        check(std::fabs(r.f_stat - 370.51) <= 5e-3, "areg_vce_c2 :589: F(2, 6) = 370.51");
        check(r.df_r == 6 && r.nested_fe.empty() && r.cluster_dof_k == 32,
              "areg_vce_c2: df_r 6, K = 32 charges every absorbed parameter");
    }
    {
        // Control: reghdfe keeps its Gmin factor (the default), byte-for-byte as before.
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "id2"});
        FeolsResult r = feols_fit(md, Vcov::Multiway);
        check(rel_eq(se_of(r, "x1"), .0711126484294995, 1e-9) && r.n_clusters == 7 && r.df_r == 6,
              "reghdfe_vce_c2_se :3825 (control): se[x1] .0711126484294995, N_clust 7 (:3820)");
    }
    {
        // Control: a one-way cluster never reads the switch.
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {"id"});
        FeolsResult r = feols_fit(md, Vcov::Cluster, 0, false, false, true);
        check(rel_eq(se_of(r, "x1"), .0505291015694893, 1e-9) && r.n_clusters == 30,
              "regress_ctl_cluster_se :107 (control): se[x1] .0505291015694893, N_clust 30");
        check(r.vcov_rank == -1 && r.kcluster.empty(),
              "one-way cluster (control): no multiway rank or combinations");
    }
    // ---- w2s_multiway_class.log (%18.0g) ----
    auto kc_is = [](const FeolsResult& r, const std::vector<std::pair<const char*, int>>& want) {
        if (r.kcluster.size() != want.size()) return false;
        for (size_t i = 0; i < want.size(); ++i)
            if (r.kcluster[i].name != want[i].first || r.kcluster[i].levels != want[i].second)
                return false;
        return true;
    };
    {
        // FU-388: e(rank) 2 from the clipped V (:116), e(kcluster) 30|7|180, cluster1..3
        // id / id2 / id#id2 (:133).
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {"id", "id2"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, false, true);
        check(r.vcov_rank == 2 && kc_is(r, {{"id", 30}, {"id2", 7}, {"id#id2", 180}}),
              "mw_reg_c2_names/kclust :116/:133: rank 2, kcluster 30|7|180 id|id2|id#id2 (got rank " +
                  std::to_string(r.vcov_rank) + ")");
    }
    {
        // FU-406: three-way regress (:1115, :1126, :1131).
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {"id", "id2", "t"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, false, true);
        const bool kc = r.kcluster.size() == 7 && r.kcluster[0].levels == 30 &&
                        r.kcluster[3].levels == 180 && r.kcluster[5].levels == 42 &&
                        r.kcluster[6].levels == 180 && r.kcluster[5].name == "id2#t" &&
                        r.kcluster[6].name == "id#id2#t";
        check(rel_eq(se_of(r, "x1"), .0334325038013226, 1e-9) &&
                  rel_eq(se_of(r, "(intercept)"), .043201813474538, 1e-9) && r.df_r == 5 &&
                  r.vcov_rank == 1 && kc,
              "mw_reg_c3 :1115/:1126/:1131: se[x1] .0334325038013226, se[_cons] .043201813474538, "
              "df_r 5, rank 1, kcluster cols 1/4/6/7 30|180|42|180, cluster6 id2#t, cluster7 "
              "id#id2#t");
    }
    {
        // FU-406: three-way areg (:1165).
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "id2", "t"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, true, true);
        check(rel_eq(se_of(r, "x1"), .0720220388954095, 1e-9) && r.df_r == 5 && r.vcov_rank == 2,
              "mw_areg_c3 :1165: se[x1] .0720220388954095, df_r 5, rank 2");
    }
    {
        // FU-386: xtreg,fe vce(cluster cg1 cg2), minus = rank + dfax (the panel FE nested
        // in the clusters is exempt): :691 df_r 4 rank 2, :696 se[x1] .0532737214421747
        // se[_cons] .7331067440032314. areg's charge (keep_nested_fe_dof) is the control.
        ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"cg1", "cg2"});
        FeolsResult r = feols_fit(md, Vcov::Multiway, 0, false, false, true);
        check(rel_eq(se_of(r, "x1"), .0532737214421747, 1e-9) &&
                  rel_eq(se_of(r, "_cons"), .7331067440032314, 1e-9) && r.df_r == 4 &&
                  r.vcov_rank == 2 && r.cluster_dof_k == 3,
              "mw_xt_cg :691/:696: se[x1] .0532737214421747, se[_cons] .7331067440032314, df_r 4, "
              "rank 2, K = 3 (got se " + std::to_string(se_of(r, "x1")) + ")");
        ModelData mg = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"cg1", "cg2"});
        FeolsResult g = feols_fit(mg, Vcov::Multiway);
        check(rel_eq(se_of(g, "x1"), .0509313674247111, 1e-9),
              "mw_xt_cg control: reghdfe's Gmin factor on the same fit is .0509313674247111");
        // [aw=wid] (constant within id): :1433 se(x1) .0434670310501989.
        ModelData mw = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"cg1", "cg2"}, "", true,
                                   nullptr, nullptr, "wid", WeightKind::Analytic);
        FeolsResult a = feols_fit(mw, Vcov::Multiway, 0, false, false, true);
        check(rel_eq(se_of(a, "x1"), .0434670310501989, 1e-9),
              "mw_xt_cg_aw_se :1433: se[x1] .0434670310501989");
    }
    {
        // FU-404: regress [iw=w] two-way: e(N) 180 and the pweight fit's se (:1320, :1207).
        ModelData mi = build_model(ds, "y", {"x1", "x2"}, {}, {"id", "id2"}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Importance);
        FeolsResult ri = feols_fit(mi, Vcov::Multiway, 0, false, false, true);
        ModelData mp = build_model(ds, "y", {"x1", "x2"}, {}, {"id", "id2"}, "", true, nullptr,
                                   nullptr, "w", WeightKind::Probability);
        FeolsResult rp = feols_fit(mp, Vcov::Multiway, 0, false, false, true);
        check(ri.n_eff == 180.0 && rel_eq(se_of(ri, "x1"), .0708771701955787, 1e-9) &&
                  rel_eq(se_of(rp, "x1"), .0708771701955785, 1e-9),
              "mw_reg_c2_iw :1320 e(N) 180 se[x1] .0708771701955787 = mw_reg_c2_pw :1207 "
              ".0708771701955785 (got N " + std::to_string(ri.n_eff) + ")");
        // BUG-1270: a one-way cluster and vce(robust) normalise the iweight too (StataBE,
        // probe BUG-1270.do: e(N) 180, e(df_r) 29), so the one-way fit is the pweight
        // fit's: mw_reg_c1_pw :1464 e(N) 180, se[x1] .0433599011907774.
        ModelData m1 = build_model(ds, "y", {"x1", "x2"}, {}, {"id"}, "", true, nullptr, nullptr,
                                   "w", WeightKind::Importance);
        FeolsResult r1 = feols_fit(m1, Vcov::Cluster, 0, false, false, true);
        check(r1.n_eff == 180.0 && r1.df_r == 29 && rel_eq(se_of(r1, "x1"), .0433599011907774, 1e-9),
              "iweight one-way cluster: e(N) 180, df_r 29, se[x1] = mw_reg_c1_pw :1464 "
              ".0433599011907774 (got N " + std::to_string(r1.n_eff) + ")");
        ModelData mh = build_model(ds, "y", {"x1", "x2"}, {}, {}, "", true, nullptr, nullptr, "w",
                                   WeightKind::Importance);
        FeolsResult rh = feols_fit(mh, Vcov::HC1);
        check(rh.n_eff == 180.0 && rh.df_r == 177, "iweight vce(robust): e(N) 180, df_r 177");
        // Control: the classic VCE keeps N = sum(w).
        ModelData mc = build_model(ds, "y", {"x1", "x2"}, {}, {}, "", true, nullptr, nullptr, "w",
                                   WeightKind::Importance);
        FeolsResult rc = feols_fit(mc, Vcov::IID);
        check(rc.n_eff == rc.sum_w && rc.n_eff != 180.0,
              "iweight classic VCE (control): N stays sum(w)");
    }
    {
        // FU-389: reghdfe's Gmin multiway clips its _cons row in reghdfe's standardised
        // units; the slopes are unchanged (:960 two-way, :1036 three-way, :1079 absorb(id t)).
        ModelData m2 = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "id2"});
        FeolsResult r2 = feols_fit(m2, Vcov::Multiway);
        check(rel_eq(se_of(r2, "x1"), .0711126484294995, 1e-9) &&
                  rel_eq(se_of(r2, "x2"), .057589001564357, 1e-9) &&
                  rel_eq(se_of(r2, "_cons"), .4540988323750039, 1e-9),
              "mw_hdfe_c2_se :960: se .0711126484294995 .057589001564357 .4540988323750039 (the "
              "data-unit clip gave _cons .4539871108580628)");
        ModelData m3 = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "id2", "t"});
        FeolsResult r3 = feols_fit(m3, Vcov::Multiway);
        check(rel_eq(se_of(r3, "x1"), .0647831770686132, 1e-9) &&
                  rel_eq(se_of(r3, "_cons"), .3268800883326979, 1e-9),
              "mw_hdfe_c3 :1036: se[x1] .0647831770686132, se[_cons] .3268800883326979");
        ModelData m4 = build_model(ds, "y", {"x1", "x2"}, {"id", "t"}, {"id", "id2"});
        FeolsResult r4 = feols_fit(m4, Vcov::Multiway);
        check(rel_eq(se_of(r4, "x1"), .0721577859098546, 1e-9) &&
                  rel_eq(se_of(r4, "_cons"), .4528458569643041, 1e-9),
              "mw_hdfe_abst_c2 :1079: se[x1] .0721577859098546, se[_cons] .4528458569643041");
        check(r2.vcov_rank == -1 && r2.kcluster.empty(),
              "reghdfe (control): no built-in rank or combinations");
    }
    {
        // FU-387: ivregress 2sls two-way: no factor without `small` (:816 se(x1)
        // .0503059336995026), the built-in per-combination factors with it (:880
        // .0585934519606149, :875 df_r 6).
        auto iv_se = [&](bool small, bool subset) {
            IvModelData md = build_iv_model(ds, "y", {"x1"}, {"x2"}, {"z1"}, {}, {"id", "id2"});
            IvResult r = iv2sls_fit(md, Vcov::Multiway, 0, false, small, false, subset);
            for (size_t j = 0; j < r.names.size(); ++j)
                if (r.names[j] == "x1") return r.se[j];
            return std::nan("");
        };
        check(rel_eq(iv_se(false, false), .0503059336995026, 1e-9),
              "mw_iv_c2_se :816: se[x1] .0503059336995026 (no small: no factor)");
        check(rel_eq(iv_se(true, true), .0585934519606149, 1e-9),
              "mw_iv_c2s_se :880: se[x1] .0585934519606149 (small: per-combination factors)");
    }
    native_unlink(path);
}

// BUG-571 / BUG-568 / FU-385: the post-estimation refit (cmds_postest.cpp
// postest_fit_spec) runs under the FIT's weight and clustered small-sample
// convention. postest_fit_spec lives in apps/cmds, which only the cobra target
// links, so these arms run the binary: $COBRA_BIN, else the `cobra` in the working
// directory (ctest runs this from the build directory, where cobra is built). A
// missing binary FAILS. COBRA_OPTIONS_STRICT also checks the option rows.
// Before this change `--weight`/`--wtype`/`--keep-nested-fe-dof`/
// `--multiway-subset-factors` were unrecognized options of lincom/test/nlcom
// (exit nonzero), so every served arm below fails on that binary.
namespace postest_cli {
namespace fs = std::filesystem;
using json = nlohmann::json;
std::string g_cobra;
fs::path g_dir;

std::string qt(const std::string& s) { return "\"" + s + "\""; }

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Run {
    int status = -1;
    json out;
    std::string err;
};

Run cobra(const std::string& args, const std::string& tag) {
    const fs::path outp = g_dir / (tag + ".json"), errp = g_dir / (tag + ".err");
    std::error_code ec;
    fs::remove(outp, ec);
    std::string cmd = qt(g_cobra) + " " + args + " --no-provenance --out " + qt(outp.string()) +
                      " 2> " + qt(errp.string());
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    Run r;
    r.status = std::system(cmd.c_str());
    r.err = slurp(errp);
    if (fs::exists(outp)) {
        try { r.out = json::parse(slurp(outp)); } catch (const std::exception&) { r.out = json(); }
    }
    return r;
}

double num(const json& j, const char* a, const char* b = nullptr) {
    try {
        const json& x = b ? j.at(a).at(b) : j.at(a);
        if (x.is_number()) return x.get<double>();
    } catch (const std::exception&) {
    }
    return std::nan("");
}

// coefs[0].<key> of a lincom / nlcom reply.
double first(const json& j, const char* key) {
    try {
        const json& c = j.at("coefs").at(0).at(key);
        if (c.is_number()) return c.get<double>();
    } catch (const std::exception&) {
    }
    return std::nan("");
}

// One fit shape: the feols table, then lincom x1 / test x1 / nlcom on the SAME
// flags. b and se equal the table (1e-12), F = (b/se)^2 (1e-9), df2 = e(df_r), and
// se equals the oracle's _se[x1] (T2, 1e-9).
struct Shape {
    const char* name;
    std::string data, flags;
    double oracle_se;
    double df2;
    double nl_b, nl_se;  // nlcom _b[x1]/_b[x2]; NaN = not pinned
};

void check_shape(const Shape& s) {
    const std::string spec = "--data " + qt(s.data) + " --y y --x x1,x2 " + s.flags;
    Run f = cobra("feols " + spec, std::string(s.name) + "_fit");
    Run l = cobra("lincom " + spec + " --exp x1", std::string(s.name) + "_lincom");
    Run t = cobra("test " + spec + " --joint x1", std::string(s.name) + "_test");
    const double fb = num(f.out, "coef", "x1"), fse = num(f.out, "se", "x1");
    const double lb = first(l.out, "b"), lse = first(l.out, "se");
    const double F = num(t.out, "F"), df2 = num(t.out, "df2");
    std::printf("  %s: fit se %.17g, lincom se %.17g, test F %.17g df2 %g\n", s.name, fse, lse, F,
                df2);
    check(f.status == 0 && l.status == 0 && t.status == 0,
          std::string(s.name) + ": feols/lincom/test exit 0 (" + f.err + l.err + t.err + ")");
    check(rel_eq(lb, fb, 1e-12) && rel_eq(lse, fse, 1e-12),
          std::string(s.name) + ": lincom x1 b/se equal the feols table (rtol 1e-12)");
    check(rel_eq(fse, s.oracle_se, 1e-9),
          std::string(s.name) + ": the table's se[x1] is the oracle's (T2)");
    check(rel_eq(F, (fb / fse) * (fb / fse), 1e-9) && df2 == s.df2,
          std::string(s.name) + ": test x1 F = (b/se)^2 (rtol 1e-9), df2 = e(df_r)");
    if (!std::isnan(s.nl_b)) {
        Run n = cobra("nlcom " + spec + " --expr \"r=_b[x1]/_b[x2]\"", std::string(s.name) + "_nlcom");
        check(n.status == 0 && rel_eq(first(n.out, "b"), s.nl_b, 1e-12) &&
                  rel_eq(first(n.out, "se"), s.nl_se, 1e-9),
              std::string(s.name) + ": nlcom _b[x1]/_b[x2] estimate (T1) and se (T2) are the "
                                    "oracle's (" + n.err + ")");
    }
}
}  // namespace postest_cli

static void test_postest_refit_cli() {
    using namespace postest_cli;
    std::printf("post-estimation refit under the fit's weight / dof convention (CLI):\n");
    const char* env = std::getenv("COBRA_BIN");
    g_cobra = env ? std::string(env) : (fs::current_path() / "cobra").string();
    if (!fs::exists(g_cobra)) {
        check(false, "cobra binary not found at " + g_cobra + " (run from the build directory or "
                     "set COBRA_BIN)");
        return;
    }
#if defined(_WIN32)
    _putenv_s("COBRA_OPTIONS_STRICT", "1");
#else
    setenv("COBRA_OPTIONS_STRICT", "1", 1);
#endif
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) /
            ("cobra_postest_refit_" + std::to_string(cecon::native_process_id()));
    fs::create_directories(g_dir, ec);
    const std::string pw = (g_dir / "w2_pweight.hbin").string();
    const std::string mw = (g_dir / "w2l_vce_shape_det.hbin").string();
    { Dataset d = make_w2_pweight_dataset(pw); }
    double mean_x2 = 0.0;
    {
        Dataset d = make_w2l_vce_shape_det_dataset(mw);
        const double* x2 = d.col("x2").f64();
        for (int64_t i = 0; i < d.n_rows(); ++i) mean_x2 += x2[i];
        mean_x2 /= static_cast<double>(d.n_rows());
    }
    const double NA = std::nan("");
    // tests/eval/golden/aer_w2/w2h_postest_refit.log, %21x decoded (PVAL _se[x1],
    // PREC <fit>_nlcom estimate / se).
    const std::vector<Shape> shapes = {
        {"R_pw (:274, :403)", pw, "--vcov hc1 --weight w --wtype pw", 0x1.a3421f4182c9fp-3, 117,
         -0x1.4cf19bc1abdc2p+1, 0x1.f39bca9e9a05bp-2},
        {"R_aw (:448)", pw, "--weight w --wtype aw", 0x1.691bde09fd0fep-3, 117, NA, NA},
        {"R_fw (:621)", pw, "--weight w --wtype fw", 0x1.9d742a6d87ee3p-4, 357, NA, NA},
        {"R_pw_cl (:796)", pw, "--vcov cluster --cluster id --weight w --wtype pw",
         0x1.7f38be33c76eep-3, 14, NA, NA},
        {"A_cl (:972, :1101)", pw, "--fe id --vcov cluster --cluster id --keep-nested-fe-dof",
         0x1.1d41125f9cb99p-2, 14, -0x1.09c4f41b4f3d0p+1, 0x1.37f69c0db7682p-2},
        {"A_cl_t (:1182)", pw, "--fe id --vcov cluster --cluster t --keep-nested-fe-dof",
         0x1.2d86c8053eee4p-2, 7, NA, NA},
        {"A_pw_cl (:1534, :1663)", pw,
         "--fe id --vcov cluster --cluster id --keep-nested-fe-dof --weight w --wtype pw",
         0x1.cfb6746651353p-3, 14, -0x1.f954da104f929p+0, 0x1.33159b9206a66p-2},
        // Control: reghdfe keeps the nested-FE exemption on the same data.
        {"H_cl_ctl (:1896, control)", pw, "--fe id --vcov cluster --cluster id",
         0x1.0ba4e5218cb86p-2, 14, NA, NA},
        // FU-385, w2l_vce_shape_det.log:263 / :619 (%18.0g).
        {"regress_vce_c2 (:263)", mw, "--vcov twoway --cluster id,id2 --multiway-subset-factors",
         .0643525900770342, 6, NA, NA},
        {"areg_vce_c2 (:619)", mw,
         "--fe id --vcov twoway --cluster id,id2 --keep-nested-fe-dof --multiway-subset-factors",
         .0778721308909151, 6, NA, NA},
        // FU-386: xtreg,fe vce(cluster cg1 cg2), w2s_multiway_class.log:696.
        {"xtreg_fe_cg (w2s :696)", mw,
         "--fe id --vcov twoway --cluster cg1,cg2 --multiway-subset-factors --keep-singletons",
         .0532737214421747, 4, NA, NA},
    };
    for (const auto& s : shapes) check_shape(s);
    {
        // FU-439: lincom's dof after a multiway regress is N - k (w2s_multiway_class.log:158
        // r(df) 177) while test keeps e(df_r) 6 (:174, the shape above); after areg it is
        // e(df_r) (:372 r(df) 6).
        const std::string reg = "--data " + qt(mw) +
                                " --y y --x x1,x2 --vcov twoway --cluster id,id2 --multiway-subset-factors";
        Run l = cobra("lincom " + reg + " --exp x1", "mw_lincom_df");
        Run a = cobra("lincom " + reg + " --fe id --keep-nested-fe-dof --exp x1", "mw_lincom_df_areg");
        check(l.status == 0 && first(l.out, "df") == 177.0 && a.status == 0 &&
                  first(a.out, "df") == 6.0,
              "mw_reg_c2_lincom :158 r(df) 177, mw_areg_c2_lincom :372 r(df) 6 (" + l.err + a.err + ")");
        Run c3 = cobra("lincom --data " + qt(mw) + " --y y --x x1,x2 --vcov cluster --cluster id "
                       "--exp x1", "mw_lincom_df_c1");
        check(c3.status == 0 && first(c3.out, "df") == 29.0,
              "mw_reg_c1_lincom :494 (control): one-way r(df) 29 = e(df_r)");
    }
    {
        // FU-385/FU-387: the 2SLS refit (`--model iv`) answers from the fit's own V:
        // w2s_multiway_class.log:840 mw_iv_c2_lincom b 1.983838860024832 se
        // .0503059336995026, normal reference (df 0 -> r(df) `.`); with `small` the
        // per-combination variance and df 6 (:875/:880).
        const std::string iv = "--data " + qt(mw) +
                               " --y y --x x1 --endog x2 --instruments z1 --vcov twoway --cluster id,id2";
        Run fit = cobra("iv " + iv, "iv_fit");
        Run l = cobra("lincom " + iv + " --model iv --exp x1", "iv_lincom");
        check(fit.status == 0 && l.status == 0 && rel_eq(first(l.out, "b"), 1.983838860024832, 1e-12) &&
                  rel_eq(first(l.out, "se"), .0503059336995026, 1e-9) && first(l.out, "df") == 0.0 &&
                  rel_eq(first(l.out, "se"), num(fit.out, "se", "x1"), 1e-12),
              "mw_iv_c2_lincom :840: lincom x1 = the iv table, se .0503059336995026, normal "
              "reference (" + l.err + ")");
        Run ls = cobra("lincom " + iv + " --model iv --small --multiway-subset-factors --exp x1",
                       "iv_lincom_small");
        Run fs = cobra("iv " + iv + " --small --multiway-subset-factors", "iv_fit_small");
        check(ls.status == 0 && fs.status == 0 && rel_eq(first(ls.out, "se"), .0585934519606149, 1e-9) &&
                  first(ls.out, "df") == 6.0 && rel_eq(num(fs.out, "se", "x1"), .0585934519606149, 1e-9) &&
                  num(fs.out, "vcov_rank") == 3.0,
              "mw_iv_c2s :875/:880: small two-way se .0585934519606149, df 6, rank 3 (" + ls.err +
                  fs.err + ")");
        Run bad = cobra("iv " + iv + " --multiway-subset-factors", "iv_bad");
        check(bad.status != 0, "iv: --multiway-subset-factors without --small is refused");
        Run badl = cobra("lincom " + iv + " --model iv --fe id --exp x1", "iv_lincom_bad");
        check(badl.status != 0, "lincom --model iv: absorbed effects are refused");
    }
    {
        // FU-401/FU-407: xtreg,fe's panels-nested-in-clusters rule (--fe-nested-in-clusters):
        // `vce(cluster id id2)` and `vce(cluster id2)` are r(498) (w2s_multiway_class.log
        // :640, :774), the nesting pair cg1 cg2 and `vce(cluster id)` are served.
        const std::string xt = "--data " + qt(mw) + " --y y --x x1,x2 --fe id --keep-singletons "
                               "--fe-nested-in-clusters";
        Run a = cobra("feols " + xt + " --vcov twoway --cluster id,id2", "xt_nest_a");
        Run b = cobra("feols " + xt + " --vcov cluster --cluster id2", "xt_nest_b");
        Run c = cobra("feols " + xt + " --vcov twoway --cluster cg1,cg2 --multiway-subset-factors",
                      "xt_nest_c");
        Run d = cobra("feols " + xt + " --vcov cluster --cluster id", "xt_nest_d");
        check(a.status != 0 && a.err.find("panels_not_nested") != std::string::npos &&
                  a.err.find("cluster variable id2") != std::string::npos,
              "mw_xt_idid2 :630-640: refused, detail panels_not_nested, names id2 (" + a.err + ")");
        check(b.status != 0 && b.err.find("panels are not nested within clusters") != std::string::npos &&
                  b.err.find("cluster variable") == std::string::npos,
              "mw_xt_c1id2 :774: refused, one-way message without the variable line (" + b.err + ")");
        check(c.status == 0 && rel_eq(num(c.out, "se", "x1"), .0532737214421747, 1e-9) &&
                  d.status == 0 && rel_eq(num(d.out, "se", "x1"), .0426516559294467, 1e-9),
              "mw_xt_cg :696 / mw_xt_c1id :760 served (" + c.err + d.err + ")");
        Run e = cobra("feols --data " + qt(mw) + " --y y --x x1,x2 --vcov cluster --cluster id "
                      "--fe-nested-in-clusters", "xt_nest_e");
        check(e.status != 0, "--fe-nested-in-clusters without --fe is refused");
    }
    {
        // fweight: the refit posts n_obs = sum of w (Stata e(N), nlcom r(N) 360 at :750).
        Run n = cobra("nlcom --data " + qt(pw) + " --y y --x x1,x2 --weight w --wtype fw "
                      "--expr \"r=_b[x1]/_b[x2]\"", "fw_nlcom");
        check(n.status == 0 && num(n.out, "n_obs") == 360.0 &&
                  rel_eq(first(n.out, "b"), -0x1.4cf19bc1abdc5p+1, 1e-12) &&
                  rel_eq(first(n.out, "se"), 0x1.26ff390c5d725p-2, 1e-9),
              "R_fw_nlcom (:750): n_obs 360, estimate and se are the oracle's (" + n.err + ")");
    }
    {
        // FU-385 margins, at() hand-check: after `regress y x1 x2, vce(cluster id id2)`
        // the linear predictive margin at x1=1 is _cons + _b[x1] + mean(x2)*_b[x2] with
        // se sqrt(g'Vg) under the FIT's V -- i.e. lincom of that form, whose V the
        // shapes above tie to the feols table. Without the flag it is the Gmin V.
        char m2[64];
        std::snprintf(m2, sizeof m2, "%.17g", mean_x2);
        const std::string spec = "--data " + qt(mw) + " --y y --x x1,x2 --vcov twoway --cluster id,id2";
        Run mg = cobra("margins " + spec + " --at \"x1=1\" --multiway-subset-factors", "mw_margins");
        Run lc = cobra("lincom " + spec + " --multiway-subset-factors --exp \"_cons + x1 + " +
                           std::string(m2) + "*x2\"", "mw_lincom");
        Run g = cobra("margins " + spec + " --at \"x1=1\"", "mw_margins_gmin");
        double mse = std::nan(""), mb = std::nan(""), gse = std::nan("");
        try { mse = mg.out.at("margins").at(0).at("se").get<double>(); } catch (...) {}
        try { mb = mg.out.at("margins").at(0).at("margin").get<double>(); } catch (...) {}
        try { gse = g.out.at("margins").at(0).at("se").get<double>(); } catch (...) {}
        std::printf("  margins at(x1=1) multiway: margin %.17g se %.17g (Gmin se %.17g)\n", mb, mse, gse);
        check(mg.status == 0 && lc.status == 0 && rel_eq(mse, first(lc.out, "se"), 1e-12) &&
                  rel_eq(mb, first(lc.out, "b"), 1e-12) && !rel_eq(mse, gse, 1e-6),
              "FU-385 margins --at --multiway-subset-factors: margin/se equal lincom _cons + x1 + "
              "mean(x2)*x2 under the fit's V (1e-12), and differ from the Gmin se (" + mg.err +
                  lc.err + ")");
        Run bad = cobra("margins " + spec + " --at \"x1=1\" --model logit --multiway-subset-factors",
                        "mw_margins_bad");
        check(bad.status != 0, "margins: --multiway-subset-factors outside the linear --at path is "
                               "refused");
    }
    // Refusals: the refit validates these flags as cmd_feols does, never fitting a
    // different model.
    struct Bad {
        const char* what;
        std::string args;
    };
    const std::vector<Bad> bad = {
        {"pweight without a sandwich VCE", "--weight w --wtype pw"},
        {"unknown --wtype", "--weight w --wtype xw"},
        {"--wtype without --weight", "--wtype aw"},
        {"iweight with --fe", "--fe id --weight w --wtype iw"},
        {"--keep-nested-fe-dof without --fe", "--vcov cluster --cluster id --keep-nested-fe-dof"},
        {"--keep-nested-fe-dof without a cluster VCE", "--fe id --vcov hc1 --keep-nested-fe-dof"},
        {"--multiway-subset-factors under a one-way cluster",
         "--vcov cluster --cluster id --multiway-subset-factors"},
        {"weights on the ML refit", "--model logit --weight w --wtype aw"},
    };
    for (const auto& b : bad) {
        Run r = cobra("lincom --data " + qt(pw) + " --y y --x x1,x2 " + b.args + " --exp x1",
                      "bad");
        check(r.status != 0 && !(r.out.is_object() && r.out.contains("coefs")),
              std::string("refused, no reply: ") + b.what + " (exit " + std::to_string(r.status) +
                  ")");
    }
    // BUG-1292: weighted xtreg,fe panel scalars -- the weighted fit's xb summarised
    // unweighted. w2_pweight.log X3_aw_fe :349-352 / X4_pw_fe :378-381
    // (this fixture; probe BUG-1292.do).
    for (const char* f : {"--weight w --wtype aw", "--weight w --wtype pw --vcov cluster --cluster id"}) {
        Run x = cobra("feols --data " + qt(pw) + " --y y --x x1,x2 --fe id " + f, "xtfe_w");
        check(x.status == 0 && rel_eq(num(x.out, "sigma_u"), 1.3060475430308727, 1e-9) &&
                  rel_eq(num(x.out, "rho"), 0.21432602617972163, 1e-9) &&
                  rel_eq(num(x.out, "corr_u_xb"), 0.5258847566156668, 1e-9) &&
                  rel_eq(num(x.out, "r2_between"), 0.96861490639220682, 1e-9) &&
                  rel_eq(num(x.out, "r2_overall"), 0.67523214206995785, 1e-9),
              std::string("weighted xtreg,fe scalars (") + f + "): sigma_u 1.3060475430308727, rho "
              ".21432602617972163, corr .5258847566156668, r2_b .96861490639220682, r2_o "
              ".67523214206995785");
    }
    {
        // A weight varying within the absorbed level (areg/reghdfe) has no xtreg scalars.
        Run x = cobra("feols --data " + qt(pw) + " --y y --x x1,x2 --fe id --weight wv --wtype aw",
                      "xtfe_wv");
        check(x.status == 0 && !x.out.contains("sigma_u") && !x.out.contains("r2_between"),
              "weight varying within id: no panel scalars");
    }
    fs::remove_all(g_dir, ec);
}

// BUG-1266 (b): `ereturn post`'s screen of a posted variance (stata_post_vcov_screen,
// StataBE 19.5 hand-built matrices) and the reghdfe fit it zeroes: probe BUG-1266.do
// part 2, a two-way (id, t) fit whose slope block is not PSD -> e(V) 0, every _se 0.
static void test_reghdfe_post_screen() {
    std::printf("reghdfe composite variance through ereturn post's screen (BUG-1266):\n");
    auto screen = [](std::vector<double> V, int k, bool& ok) {
        ok = stata_post_vcov_screen(V, k);
        return V;
    };
    bool ok = false;
    std::vector<double> V = screen({1, 1 + 9e-11, 1 + 9e-11, 1}, 2, ok);
    check(ok && V[1] == 1.0 && V[2] == 1.0, "corr 1 + 9e-11: posted, clamped to 1");
    V = screen({1, -1 - 1e-11, -1 - 1e-11, 1}, 2, ok);
    check(ok && V[1] == -1.0, "corr -1 - 1e-11: posted, clamped to -1");
    V = screen({1, 1.0000000001, 1.0000000001, 1}, 2, ok);
    check(!ok && V[0] == 0.0 && V[1] == 0.0 && V[3] == 0.0, "corr 1 + 1e-10: zeroed");
    V = screen({4, 2.0000000002, 2.0000000002, 1}, 2, ok);
    check(!ok && V[0] == 0.0, "corr 1 + 1e-10 at another scale: zeroed");
    V = screen({1, 0, 0, -1e-3}, 2, ok);
    check(ok && V[3] == -1e-3, "a negative variance is not examined: diag(1, -1e-3) posted");
    V = screen({1, 0.1, 0.1, 0}, 2, ok);
    check(ok && V[1] == 0.1, "a zero variance is not examined");
    V = screen({1, .9, -.9, .9, 1, .9, -.9, .9, 1}, 3, ok);
    check(ok && V[1] == .9 && V[2] == -.9, "indefinite, |corr| .9: posted unchanged");

    const std::string path = native_temp_path("cecon_test_prewave_post_screen.hbin");
    const int N = 180;
    std::vector<double> id(N), t(N), x1(N), x2(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        t[i] = static_cast<double>((n - 1) % 6 + 1);
        const double g6 = t[i] == 1 ? 1 : t[i] == 2 ? -1 : t[i] == 3 ? -1 : t[i] == 4 ? 1 : 0;
        const double u = static_cast<int>(id[i]) % 2 ? 1.0 : -1.0;
        x1[i] = t[i] + static_cast<double>(static_cast<int>(id[i]) * 7 % 11) / 4.0;
        x2[i] = 50.0 * g6 + 20.0 * (t[i] - 3.5) + static_cast<double>(static_cast<int>(id[i]) * 5 % 13);
        y[i] = 1.0 + 2.0 * x1[i] - 0.5 * x2[i] + id[i] / 4.0 + g6 * u +
               static_cast<double>(n * 7 % 5) / 16.0;
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("t", t);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    ModelData md = build_model(ds, "y", {"x1", "x2"}, {"id"}, {"id", "t"});
    FeolsResult r = feols_fit(md, Vcov::Multiway);
    bool zero = r.vcov_matrix.size() == static_cast<size_t>(r.k) * r.k;
    for (double v : r.vcov_matrix) zero = zero && v == 0.0;
    for (double s : r.se) zero = zero && s == 0.0;
    check(zero && r.k == 3 && std::fabs(r.beta[0] - 2.0) < 1e-9 && std::isnan(r.f_stat),
          "BUG-1266 part 2: e(V) 0, every se 0, b[x1] 2, F missing");
    native_unlink(path);
}

// BUG-1484: `poisson cnt L.x1 x2` after xtset reaches cobra fepois with --xtset; the
// operators read the full series and an `if` applies after them. StataBE (scratch
// eng-linear/mlts.do): e(N) 150, _b[L.x1] -.050220717740893102, _se[L.x1]
// .017887448442851974; `poisson cnt D.x1 x2 if t > 2, vce(robust)`: e(N) 120, _b[D.x1]
// .059241186155305929 (ml's own tolerance: rtol 1e-7).
static void test_fepois_ts_cli() {
    using namespace postest_cli;
    std::printf("fepois with time-series operators under --xtset (CLI):\n");
    const char* env = std::getenv("COBRA_BIN");
    g_cobra = env ? std::string(env) : (fs::current_path() / "cobra").string();
    if (!fs::exists(g_cobra)) {
        check(false, "cobra binary not found at " + g_cobra);
        return;
    }
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) /
            ("cobra_fepois_ts_" + std::to_string(cecon::native_process_id()));
    fs::create_directories(g_dir, ec);
    const std::string path = (g_dir / "mlts.hbin").string();
    {
        const int N = 180;
        std::vector<double> id(N), t(N), x1(N), x2(N), cnt(N);
        for (int i = 0; i < N; ++i) {
            const int n = i + 1;
            id[i] = std::ceil(n / 6.0);
            t[i] = static_cast<double>((n - 1) % 6 + 1);
            // `generate` stores floats; these values are all exact in float but x2's.
            x1[i] = static_cast<double>(n * 7 % 11) + static_cast<double>(n % 5) / 8.0;
            const double z1 = static_cast<double>(static_cast<float>(
                static_cast<double>(n * 5 % 13) + x1[i] / 2.0 + static_cast<double>(n * 17 % 19) / 16.0));
            x2[i] = static_cast<double>(static_cast<float>(
                0.5 * z1 + static_cast<double>(n * 3 % 5) + static_cast<double>(n * 13 % 7) / 8.0 +
                static_cast<double>(n * 23 % 29) / 32.0));
            cnt[i] = std::trunc((static_cast<double>(n * 7 % 11) + static_cast<double>(n * 13 % 17) / 2.0 +
                                 static_cast<double>(static_cast<int>(id[i]) % 4)) / 4.0);
        }
        Frame f;
        f.set_n_rows_hint(N);
        f.append_f64_column("id", id);
        f.append_f64_column("t", t);
        f.append_f64_column("x1", x1);
        f.append_f64_column("x2", x2);
        f.append_f64_column("cnt", cnt);
        f.finalize_after_bulk_load();
        f.write_hbin(path);
    }
    Run a = cobra("fepois --data " + qt(path) + " --y cnt --x L.x1,x2 --xtset id,t", "ts_l");
    check(a.status == 0 && num(a.out, "n_obs") == 150 &&
              rel_eq(num(a.out, "coef", "L.x1"), -.050220717740893102, 1e-7) &&
              rel_eq(num(a.out, "se", "L.x1"), .017887448442851974, 1e-7),
          "poisson cnt L.x1 x2: N 150, _b[L.x1] -.050220717740893102, _se .017887448442851974 (" +
              a.err + ")");
    Run b = cobra("fepois --data " + qt(path) +
                      " --y cnt --x D.x1,x2 --xtset id,t --keepif \"t > 2\" --vcov hc1",
                  "ts_d_if");
    check(b.status == 0 && num(b.out, "n_obs") == 120 &&
              rel_eq(num(b.out, "coef", "D.x1"), .059241186155305929, 1e-7),
          "poisson cnt D.x1 x2 if t > 2: N 120 (D.x1 at t = 3 reads t = 2), _b[D.x1] "
          ".059241186155305929 (" + b.err + ")");
    fs::remove_all(g_dir, ec);
}

// BUG-1225: an included regressor constant within the absorbed level is omitted, not
// refused. StataBE `ivreghdfe y x2 x3 (x1 = z1), absorb(id)` (probe BUG-1225.do):
// x2 "(omitted)", _b[x1] 2.0200764942375309, _se[x1] .065431301337589967,
// _b[x3] -.99831438394655125, F(2, 98).
static void test_iv_absorbed_exog_omitted() {
    std::printf("ivreghdfe: a regressor the absorption annihilates is omitted (BUG-1225):\n");
    const std::string path = native_temp_path("cecon_test_prewave_iv_omit.hbin");
    const int N = 120;
    std::vector<double> id(N), z1(N), x1(N), x2(N), x3(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        z1[i] = static_cast<double>(n * 5 % 13) + static_cast<double>(n * 17 % 19) / 16.0;
        x1[i] = 0.5 * z1[i] + static_cast<double>(n * 7 % 11) / 4.0 + static_cast<double>(n % 5) / 8.0;
        x2[i] = static_cast<double>(static_cast<int>(id[i]) * 3 % 7);
        x3[i] = static_cast<double>(n * 3 % 5) + static_cast<double>(n * 13 % 7) / 8.0;
        y[i] = 1.0 + 2.0 * x1[i] - x3[i] + x2[i] + id[i] / 4.0 + static_cast<double>(n * 11 % 17) / 4.0;
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("z1", z1);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("x3", x3);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    IvModelData md = build_iv_model(ds, "y", {"x2", "x3"}, {"x1"}, {"z1"}, {"id"}, {});
    IvResult r = iv2sls_fit(md, Vcov::IID, 0, false, /*small=*/true);
    auto at = [&](const char* nm) {
        for (size_t j = 0; j < r.names.size(); ++j)
            if (r.names[j] == nm) return static_cast<int>(j);
        return -1;
    };
    const int i1 = at("x1"), i3 = at("x3");
    check(r.omitted_exog == std::vector<std::string>{"x2"} && at("x2") < 0 && i1 >= 0 && i3 >= 0 &&
              r.dof.dof_resid == 98,
          "x2 omitted, x1/x3 fitted, dof 98");
    if (i1 >= 0 && i3 >= 0)
        check(rel_eq(r.beta[static_cast<size_t>(i1)], 2.0200764942375309, 1e-9) &&
                  rel_eq(r.se[static_cast<size_t>(i1)], .065431301337589967, 1e-9) &&
                  rel_eq(r.beta[static_cast<size_t>(i3)], -.99831438394655125, 1e-9),
              "_b[x1] 2.0200764942375309, _se[x1] .065431301337589967, _b[x3] -.99831438394655125");
    native_unlink(path);
}

// BUG-1277: a time-series operator on the depvar, probe BUG-1277.do's fixture (y a
// float in Stata, so D.y is Stata's float temporary). StataBE `regress D.y x1 x2 if
// t>=3`: e(N) 48, _b[x1] .72148919028596759, _se[x1] .085627833054443464; `regress D.y
// D.x1`: _b[D.x1] .49217914311006128 (a double D.y gives .4921791398588985).
static void test_ts_depvar() {
    std::printf("time-series operator on the depvar (BUG-1277):\n");
    const std::string path = native_temp_path("cecon_test_prewave_ts_depvar.hbin");
    const int N = 60;
    std::vector<double> id(N), t(N), x1(N), x2(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 10.0);
        t[i] = static_cast<double>((n - 1) % 10 + 1);
        x1[i] = static_cast<double>(n * 7 % 11);
        x2[i] = static_cast<double>(n * 3 % 5);
        // `generate y = ...` stores a float
        y[i] = static_cast<double>(static_cast<float>(1.0 + 0.5 * x1[i] - 0.2 * x2[i] +
                                                      static_cast<double>(n * 5 % 9) / 4.0 +
                                                      t[i] / 3.0));
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("t", t);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    TsMeta tsm;
    tsm.panelvar = "id";
    tsm.timevar = "t";
    std::vector<char> mask(N, 0);
    for (int i = 0; i < N; ++i) mask[static_cast<size_t>(i)] = t[static_cast<size_t>(i)] >= 3;
    ModelData md = build_model(ds, "D.y", {"x1", "x2"}, {}, {}, "", true, &tsm, &mask);
    FeolsResult r = feols_fit(md, Vcov::IID);
    const int i1 = coef_index(r, "x1");
    check(r.n == 48 && i1 >= 0 && eq(r.beta[static_cast<size_t>(i1)], .72148919028596759, 1e-9) &&
              eq(r.se[static_cast<size_t>(i1)], .085627833054443464, 1e-9),
          "D.y x1 x2 if t>=3: N 48, _b[x1] .72148919028596759, _se[x1] .085627833054443464");
    ModelData m2 = build_model(ds, "D.y", {"D.x1"}, {}, {}, "", true, &tsm);
    FeolsResult r2 = feols_fit(m2, Vcov::IID);
    const int j1 = coef_index(r2, "D.x1");
    check(r2.n == 54 && j1 >= 0 && eq(r2.beta[static_cast<size_t>(j1)], .49217914311006128, 1e-12),
          "D.y D.x1: N 54, _b[D.x1] .49217914311006128 (D.y rounded to float)");
    native_unlink(path);
}

// BUG-1249: `ppmlhdfe cnt x1 x2 [pw=w], absorb(id)` (probe BUG-1249.do; StataBE
// scratch-laneA/agents/W/e3.log section O): e(N) 180, _b[x1] 0.12244645797006155,
// _se[x1] 0.0088407626153069081 (robust), _b[x2] -.0115943, _b[_cons] .1756453 se
// .0956865, e(ll) -629.7946392.
static void test_fepois_weighted_fe() {
    std::printf("ppmlhdfe [pw=], absorb() (BUG-1249):\n");
    const std::string path = native_temp_path("cecon_test_prewave_fepois_pw.hbin");
    const int N = 180;
    std::vector<double> id(N), x1(N), x2(N), cnt(N), w(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        x1[i] = static_cast<double>(n * 7 % 11) + static_cast<double>(n % 5) / 8.0;
        const double z1 = static_cast<double>(n * 5 % 13) + x1[i] / 2.0 +
                          static_cast<double>(n * 17 % 19) / 16.0;
        x2[i] = 0.5 * z1 + static_cast<double>(n * 3 % 5) + static_cast<double>(n * 13 % 7) / 8.0 +
                static_cast<double>(n * 23 % 29) / 32.0;
        cnt[i] = std::trunc((static_cast<double>(n * 7 % 11) + static_cast<double>(n * 13 % 17) / 2.0 +
                             static_cast<double>(static_cast<int>(id[i]) % 4)) / 4.0);
        w[i] = 1.0 + static_cast<double>(n % 4);
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("id", id);
    f.append_f64_column("x1", x1);
    f.append_f64_column("x2", x2);
    f.append_f64_column("cnt", cnt);
    f.append_f64_column("w", w);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    ModelData md = build_model(ds, "cnt", {"x1", "x2"}, {"id"}, {}, "", true, nullptr, nullptr,
                               "w", WeightKind::Probability);
    FepoisResult r = fepois_fit(md, Vcov::HC1);
    check(r.n == 180 && r.converged && r.k == 2 && rel_eq(r.beta[0], 0.12244645797006155, 1e-9) &&
              rel_eq(r.se[0], 0.0088407626153069081, 1e-9),
          "N 180, _b[x1] 0.12244645797006155, _se[x1] 0.0088407626153069081 (got b " +
              std::to_string(r.beta[0]) + ")");
    check(std::fabs(r.beta[1] - (-.0115943)) < 5e-8 && std::fabs(r.cons_b - .1756453) < 5e-8 &&
              std::fabs(r.cons_se - .0956865) < 5e-8 && std::fabs(r.ll - (-629.7946392)) < 5e-8,
          "_b[x2] -.0115943, _cons .1756453 (se .0956865), e(ll) -629.7946392");
    native_unlink(path);
}

// BUG-1283: `nl (y = {b0} + {b1}*x1 + {b2}*x2) [aw=w]` / `[pw=w]` (probe BUG-1283.do's
// fixture; StataBE scratch eng-linear/w1283.do): aw _b[/b1] 1.9826615267411951, _se
// .067819537723433465, e(rss) 1228.6978222330188, e(tss) 7170.4229906684022, e(dev)
// 878.47338532728838; pw _se[/b1] .071775268345285762.
static void test_nl_weighted() {
    std::printf("nl [aw=] / [pw=] (BUG-1283):\n");
    const int N = 180;
    std::vector<double> x1(N), x2(N), y(N), w(N);
    double sw = 0;
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        const double id = std::ceil(n / 6.0);
        x1[i] = static_cast<double>(n * 7 % 11) + static_cast<double>(n % 5) / 8.0;
        const double z1 = static_cast<double>(static_cast<float>(
            static_cast<double>(n * 5 % 13) + x1[i] / 2.0 + static_cast<double>(n * 17 % 19) / 16.0));
        x2[i] = static_cast<double>(static_cast<float>(
            0.5 * z1 + static_cast<double>(n * 3 % 5) + static_cast<double>(n * 13 % 7) / 8.0 +
            static_cast<double>(n * 23 % 29) / 32.0));
        y[i] = static_cast<double>(static_cast<float>(
            1.0 + 2.0 * x1[i] - x2[i] + id / 4.0 + static_cast<double>(n * 11 % 17) / 4.0 +
            (static_cast<double>(n * 37 % 41) / 64.0 + static_cast<double>(n * 53 % 59) / 64.0 +
             static_cast<double>(n * 29 % 31) / 64.0 - 1.0) * 3.0));
        w[i] = 1.0 + static_cast<double>(n % 4);
        sw += w[i];
    }
    for (double& v : w) v *= static_cast<double>(N) / sw;
    const NlResidFn resid = [&](const std::vector<double>& b, int64_t i) {
        const size_t r = static_cast<size_t>(i);
        return y[r] - (b[0] + b[1] * x1[r] + b[2] * x2[r]);
    };
    const NlResult a = nl_fit(resid, y.data(), N, {0.0, 0.0, 0.0}, {"b0", "b1", "b2"}, w.data());
    check(a.ok && rel_eq(a.beta[1], 1.9826615267411951, 1e-9) &&
              rel_eq(a.se[1], .067819537723433465, 1e-9) && rel_eq(a.rss, 1228.6978222330188, 1e-9) &&
              rel_eq(a.tss, 7170.4229906684022, 1e-9) && rel_eq(a.res_dev, 878.47338532728838, 1e-9) &&
              a.cons_param == 0,
          "aw: _b[/b1], _se, e(rss), e(tss), e(dev) as Stata; b0 is the constant");
    const NlResult p =
        nl_fit(resid, y.data(), N, {0.0, 0.0, 0.0}, {"b0", "b1", "b2"}, w.data(), /*robust=*/true);
    check(p.ok && rel_eq(p.se[1], .071775268345285762, 1e-9), "pw: robust _se[/b1] .071775268345285762");
}

// BUG-1283: `eventstudyinteract ye rm2 r0 r1 [aw=wid], cohort(g) control_cohort(never)
// absorb(id t) vce(cluster id)` (probe BUG-1283.do's fixture; StataBE scratch
// eng-linear/w1283.do): e(b_iw) .86176928767412542 .1243624891652011 1.1556403315466284,
// e(V_iw) diagonal 6.3930934560306003 5.8898898225742959 5.2464959577287758, e(ff_w)
// .49074074074074081 / .5092592592592593, e(F) .23247636562916, e(r2) .1509545593295808.
static void test_sunab_weighted() {
    std::printf("eventstudyinteract [aw=] (BUG-1283):\n");
    const int N = 180;
    std::vector<double> ye(N), g(N), nev(N), id(N), t(N), wid(N), rel(static_cast<size_t>(N) * 3, 0.0);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        id[i] = std::ceil(n / 6.0);
        t[i] = static_cast<double>((n - 1) % 6 + 1);
        const double x1 = static_cast<double>(n * 7 % 11) + static_cast<double>(n % 5) / 8.0;
        const int idm3 = static_cast<int>(id[i]) % 3;
        g[i] = idm3 == 0 ? 0.0 : (idm3 == 1 ? 3.0 : 5.0);
        nev[i] = g[i] == 0.0 ? 1.0 : 0.0;
        const double r = g[i] > 0 ? t[i] - g[i] : -99.0;
        rel[static_cast<size_t>(0) * N + i] = r == -2.0;
        rel[static_cast<size_t>(1) * N + i] = r == 0.0;
        rel[static_cast<size_t>(2) * N + i] = r == 1.0;
        ye[i] = static_cast<double>(static_cast<float>(
            1.0 + 2.0 * x1 + id[i] / 4.0 + static_cast<double>(n * 11 % 17) / 4.0 +
            (static_cast<double>(n * 37 % 41) / 64.0 - 0.5) * 3.0));
        wid[i] = 1.0 + static_cast<double>(static_cast<int>(id[i]) % 4) / 4.0;
    }
    SunabResult R = sunab_run(ye.data(), rel.data(), 3, g.data(), nev.data(), id.data(), t.data(),
                              id.data(), N, SunabConfig{}, wid.data());
    const double bw[] = {.86176928767412542, .1243624891652011, 1.1556403315466284};
    const double vw[] = {6.3930934560306003, 5.8898898225742959, 5.2464959577287758};
    bool ok = R.ok && R.coefs.size() == 3 && R.n_obs == 180;
    for (size_t l = 0; ok && l < 3; ++l)
        ok = rel_eq(R.coefs[l].coef, bw[l], 1e-9) && rel_eq(R.coefs[l].se * R.coefs[l].se, vw[l], 1e-9) &&
             rel_eq(R.coefs[l].cohort_weight[0], .49074074074074081, 1e-12);
    check(ok && rel_eq(R.F, .23247636562916, 1e-9) && rel_eq(R.r2, .1509545593295808, 1e-9) &&
              R.sum_w == 247.5 && R.sum_w_weights == 162.0,
          "e(b_iw), e(V_iw) diagonal, e(ff_w), e(F), e(r2), sums of weights 247.5 / 162");
    // BUG-1524: the whole e(V_iw) (StataBE 19.5, scratch-laneA2/agents/esib/m20.do).
    const double vo[] = {2.5895682523835175, 3.1825335629558218, .27690364744784723};  // [1,2] [1,3] [2,3]
    const auto V = [&R](int i, int j) { return R.V_iw[static_cast<size_t>(i) * 3 + j]; };
    check(R.V_iw.size() == 9 && rel_eq(V(0, 1), vo[0], 1e-9) && rel_eq(V(0, 2), vo[1], 1e-9) &&
              rel_eq(V(1, 2), vo[2], 1e-9) && V(1, 0) == V(0, 1) && V(2, 0) == V(0, 2) &&
              V(2, 1) == V(1, 2) && rel_eq(V(0, 0), vw[0], 1e-9) && rel_eq(V(1, 1), vw[1], 1e-9) &&
              rel_eq(V(2, 2), vw[2], 1e-9),
          "e(V_iw) off-diagonals 2.5895682523835175 3.1825335629558218 .27690364744784723, symmetric");
}

// BUG-1055: the exact-fit rule under [aw]/[fw]/[iw] and areg's absorption (StataBE,
// probe BUG-1055.do + scratch eng-linear/ef.do: e(rss) 0, _se[x] 0 on y2 = 3 + 2x; the
// areg test is the full-model e(r2), so y3 = 1e7*gp + 3 + 2x + 3e-5*noise is exact too).
static void test_exact_fit_widened() {
    std::printf("exact-fit rule under weights and absorb (BUG-1055):\n");
    const std::string path = native_temp_path("cecon_test_prewave_exact_fit.hbin");
    const int N = 50;
    std::vector<double> x(N), y2(N), y3(N), w(N), gp(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        x[i] = n / 7.0;
        y2[i] = 3.0 + 2.0 * x[i];
        w[i] = 1.0 + static_cast<double>(n % 4);
        gp[i] = 1.0 + static_cast<double>(n % 5);
        y3[i] = 1e7 * gp[i] + 3.0 + 2.0 * x[i] + 3e-5 * (static_cast<double>(n * 7 % 11) / 11.0 - .5);
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("x", x);
    f.append_f64_column("y2", y2);
    f.append_f64_column("y3", y3);
    f.append_f64_column("w", w);
    f.append_f64_column("gp", gp);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    struct Arm {
        const char* tag;
        const char* y;
        std::vector<std::string> fe;
        const char* wv;
        WeightKind wk;
    };
    const std::vector<Arm> arms = {
        {"regress [aw=w]", "y2", {}, "w", WeightKind::Analytic},
        {"regress [fw=gp]", "y2", {}, "gp", WeightKind::Frequency},
        {"regress [iw=w]", "y2", {}, "w", WeightKind::Importance},
        {"areg, absorb(gp)", "y2", {"gp"}, "", WeightKind::None},
        {"areg [aw=w], absorb(gp)", "y2", {"gp"}, "w", WeightKind::Analytic},
        {"areg y3 (full-model e(r2))", "y3", {"gp"}, "", WeightKind::None},
    };
    for (const Arm& a : arms) {
        ModelData md = build_model(ds, a.y, {"x"}, a.fe, {}, "", false, nullptr, nullptr, a.wv, a.wk);
        FeolsResult r = feols_fit(md, Vcov::IID, 0, false, true, false, /*regress_exact_fit=*/true);
        bool zero = r.exact_fit && r.rss == 0.0 && std::isnan(r.f_stat);
        for (double s : r.se) zero = zero && s == 0.0;
        check(zero, std::string(a.tag) + ": e(rss) 0, every se 0, F missing");
    }
    native_unlink(path);
}

// BUG-1180: reghdfe absorb(a##b) of grouping variables absorbs the single group a#b
// (StataBE: e(extended_absvars) "a#b", e(df_a) 12, _b[x] 2.0447956075972669, cluster(cl)
// _se[x] .097033290674130052 -- absorb(a#b)'s fit exactly).
static void test_absorb_factorial() {
    std::printf("absorb(a##b) is a#b (BUG-1180):\n");
    const std::string path = native_temp_path("cecon_test_prewave_absorb_ff.hbin");
    const int N = 120;
    std::vector<double> a(N), b(N), cl(N), x(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        a[i] = static_cast<double>(n % 4);
        b[i] = static_cast<double>(static_cast<int>(std::ceil(n / 3.0)) % 3);
        cl[i] = 1.0 + static_cast<double>(n % 9);
        const double z = static_cast<double>(static_cast<float>(static_cast<double>(n * 7 % 11) / 3.0));
        const double z2 = static_cast<double>(n * 5 % 13) / 4.0;
        x[i] = static_cast<double>(static_cast<float>(0.5 * z + static_cast<double>(n * 3 % 5) / 2.0 + 0.2 * z2));
        y[i] = static_cast<double>(static_cast<float>(1.0 + 2.0 * x[i] + a[i] / 3.0 + b[i] / 4.0 +
                                                      static_cast<double>(n * 11 % 17) / 5.0));
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("a", a);
    f.append_f64_column("b", b);
    f.append_f64_column("cl", cl);
    f.append_f64_column("x", x);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    ModelData mf = build_model(ds, "y", {"x"}, {"a##b"}, {"cl"});
    FeolsResult rf = feols_fit(mf, Vcov::Cluster);
    ModelData mi = build_model(ds, "y", {"x"}, {"i.a#i.b"}, {"cl"});
    FeolsResult ri = feols_fit(mi, Vcov::Cluster);
    const int jx = coef_index(rf, "x");
    check(jx >= 0 && mf.fe.size() == 1 && rf.dof.k_fe == 12 && rf.beta == ri.beta &&
              rel_eq(rf.beta[static_cast<size_t>(jx)], 2.0447956075972669, 1e-9) &&
              rel_eq(rf.se[static_cast<size_t>(jx)], .097033290674130052, 1e-9),
          "one absorbed group, e(df_a) 12, b and se of absorb(a#b) = StataBE's");
    native_unlink(path);
}

// BUG-1043: e(V_modelbased) after a one-way vce(robust)/vce(cluster) is the plain fit's
// e(V)/s2 -- regress unweighted or [fw] (no FE), areg/xtreg,fe unweighted (the extended
// bread with _cons) -- and is not sent under an aweight (StataBE scratch eng-linear/vmb.do).
static void test_vmb_one_way() {
    std::printf("e(V_modelbased) after one-way robust/cluster fits (BUG-1043):\n");
    const std::string path = native_temp_path("cecon_test_prewave_vmb.hbin");
    Dataset ds = make_w2l_vce_shape_det_dataset(path);
    auto plain = [&](const std::vector<std::string>& fe) {
        ModelData m = build_model(ds, "y", {"x1", "x2"}, fe, {"id"}, "", false);
        FeolsResult r = feols_fit(m, Vcov::IID);
        std::vector<double> v = r.vcov_matrix;
        for (double& e : v) e /= r.sigma2;
        return v;
    };
    auto mb = [&](const std::vector<std::string>& fe, Vcov vt, const char* wv, WeightKind wk) {
        ModelData m = build_model(ds, "y", {"x1", "x2"}, fe, {"id"}, "", false, nullptr, nullptr, wv, wk);
        return feols_fit(m, vt).vcov_modelbased;
    };
    auto same = [](const std::vector<double>& a, const std::vector<double>& b) {
        if (a.size() != b.size() || a.empty()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!(std::fabs(a[i] - b[i]) <= 1e-12 * std::fabs(b[i]) + 1e-300)) return false;
        return true;
    };
    const std::vector<double> p0 = plain({}), p1 = plain({"id"});
    check(same(mb({}, Vcov::HC1, "", WeightKind::None), p0) &&
              same(mb({}, Vcov::Cluster, "", WeightKind::None), p0),
          "regress robust / cluster: e(V_modelbased) = plain e(V)/s2");
    check(same(mb({"id"}, Vcov::Cluster, "", WeightKind::None), p1) &&
              same(mb({"id"}, Vcov::HC1, "", WeightKind::None), p1),
          "areg / xtreg,fe robust / cluster: the extended bread, _cons included");
    check(mb({}, Vcov::HC1, "w", WeightKind::Analytic).empty() && mb({}, Vcov::IID, "", WeightKind::None).empty(),
          "aweight and the classic VCE: not sent");
    native_unlink(path);
}

// BUG-1489: a factor variable with a noninteger or negative level on the
// estimation sample is Stata's r(452) (StataBE scratch eng-linear/fvni.do), error code
// "factor_values"; noninteger is reported before negative.
static void test_factor_level_checks() {
    std::printf("factor variables with noninteger / negative levels (r(452)):\n");
    const std::string path = native_temp_path("cecon_test_prewave_fvni.hbin");
    const int N = 100;
    std::vector<double> x(N), z(N), xn(N), xnf(N), xi(N), y(N);
    for (int i = 0; i < N; ++i) {
        const int n = i + 1;
        x[i] = static_cast<double>(n * 7 % 23) / 4.0;
        z[i] = static_cast<double>(n * 13 % 17) / 3.0;
        xn[i] = static_cast<double>(n % 5 - 2);
        xnf[i] = static_cast<double>(n % 5 - 2) / 2.0;
        xi[i] = static_cast<double>(n % 4);
        y[i] = 1.0 + 0.5 * x[i] + 0.3 * z[i] + static_cast<double>(n * 5 % 9) / 4.0;
    }
    Frame f;
    f.set_n_rows_hint(N);
    f.append_f64_column("x", x);
    f.append_f64_column("z", z);
    f.append_f64_column("xn", xn);
    f.append_f64_column("xnf", xnf);
    f.append_f64_column("xi", xi);
    f.append_f64_column("y", y);
    f.finalize_after_bulk_load();
    native_unlink(path);
    f.write_hbin(path);
    Dataset ds = Dataset::open(path);
    auto msg_of = [&](const std::vector<std::string>& xs) -> std::string {
        try {
            (void)build_model(ds, "y", xs, {}, {});
        } catch (const EngineError& e) {
            return e.code + "|" + e.what();
        }
        return "ok";
    };
    check(msg_of({"i.x"}) == "factor_values|x:  factor variables may not contain noninteger values" &&
              msg_of({"x#z"}) == "factor_values|x:  factor variables may not contain noninteger values",
          "i.x and x#z: noninteger, r(452)");
    check(msg_of({"i.xn"}) == "factor_values|xn:  factor variables may not contain negative values" &&
              msg_of({"i.xnf"}) == "factor_values|xnf:  factor variables may not contain noninteger values" &&
              msg_of({"i.xi", "i.xn"}) == "factor_values|xn:  factor variables may not contain negative values",
          "negative levels; noninteger reported first");
    check(msg_of({"i.xi##c.x"}) == "ok" && msg_of({"c.x#c.z"}) == "ok" && msg_of({"i.xi#c.z"}) == "ok",
          "integer factors and continuous interactions are served");
    native_unlink(path);
}

int test_cli_prewave_main() {
    std::printf("test_cli_prewave:\n");
    test_factor_level_checks();
    test_vmb_one_way();
    test_absorb_factorial();
    test_exact_fit_widened();
    test_sunab_weighted();
    test_nl_weighted();
    test_fepois_weighted_fe();
    test_iv_absorbed_exog_omitted();
    test_ts_depvar();
    test_e1_ts_if_ordering();
    test_e1_split_ts_detector();
    test_areg_robust_unweighted_control();
    test_weighted_fe_absorption();
    test_areg_cluster_dof();
    test_multiway_subset_factors();
    test_reghdfe_post_screen();
    test_postest_refit_cli();
    test_fepois_ts_cli();
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
