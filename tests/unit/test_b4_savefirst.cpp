// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// B4/W6 item 3: `ivreg2 …, savefirst` — the stored first-stage estimate.
//
// Oracle: tests/eval/golden/aer_b4/ivreg2-savefirst.log (StataBE, ivreg2 4.1.11 on
// sysuse auto). Every expected number below is a %21x cell of that log, cited by its
// arm letter and re-decoded here with std::strtod on the hex literal so the
// transcription is the log's bytes, not a re-typed decimal.
//
// The load-bearing claim it settles — and the reason W6 did not land this — is that
// the stored first stage is NOT a bespoke system variance. It is
//     regress <endog> <excluded IVs> <included exog>, <the PARENT's vce>
// with the small-sample correction ALWAYS on. The receipt asserted the opposite;
// REFUTE.md refuted it from ivreg2's ado and help (identified from them), and
// the capture then measured it: mreldif(V_first, V_regress) = 1.5e-13 (robust arm A)
// and 1.0e-14 (cluster arm E), against nulls of ~1.0 (permuted regressor order) and
// ~0.93 (classical OLS) through the same comparator.
//
// So the tests come in two kinds:
//   (a) CELLS — scalars read straight off the log: N, df_r, df_m, k_eq, rmse, the
//       column ORDER (ivreg2's cnZ1 = exexog inexog _cons), the cluster count, and
//       two individual coefficients.
//   (b) IDENTITIES — the oracle's own mreldif claims, re-run here with cobra's OWN
//       `regress` on the right-hand side. cobra's no-FE feols path is separately
//       gated against Stata `regress` (see feols.h), so "first stage == regress"
//       is a real reproduction of the oracle's finding rather than a tautology,
//       and it comes with the same nulls the log used.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/feols.h"
#include "estimators/iv2sls.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int g_fail = 0;

// The log prints every number as Stata's %21x. Decode the literal rather than a
// hand-converted decimal so what this file carries is the captured bytes.
static double hx(const char* s) { return std::strtod(s, nullptr); }

static void near(double got, double want, double rtol, const std::string& tag) {
    const double rel = std::fabs(got - want) / (std::fabs(want) + 1e-300);
    if (!(rel <= rtol)) {
        std::printf("  FAIL: %s: got=%.17g want=%.17g rel=%.3e (tol %.0e)\n", tag.c_str(), got,
                    want, rel, rtol);
        g_fail = 1;
    }
}
static void eqi(long long got, long long want, const std::string& tag) {
    if (got != want) {
        std::printf("  FAIL: %s: got=%lld want=%lld\n", tag.c_str(), got, want);
        g_fail = 1;
    }
}
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); g_fail = 1; }
}
// Stata mreldif(A,B) = max |a-b| / (1+|b|), the instrument the oracle used. A
// %21x bit-diff would report a difference for two algebraically identical solvers
// and read as "they differ" (REFUTE.md "what the receipt did not ask" §3).
static double mreldif(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(a[i] - b[i]) / (1.0 + std::fabs(b[i])));
    return m;
}

namespace {
struct Auto {
    int64_t n = 0;
    std::vector<double> price, mpg, turn, weight, headroom, trunk, length, cl;
};
Auto load_auto() {
    const cobra_test::ValidateBase base;
    Dataset ds = Dataset::open(base + "auto.hbin");
    Auto A;
    A.n = ds.n_rows();
    auto col = [&](const char* nm) {
        const double* p = ds.col(nm).f64();
        return std::vector<double>(p, p + A.n);
    };
    A.price = col("price"); A.mpg = col("mpg"); A.turn = col("turn");
    A.weight = col("weight"); A.headroom = col("headroom");
    A.trunk = col("trunk"); A.length = col("length");
    // Arm E's cluster variable, as in ivreg2-savefirst.do:61 —
    //   generate byte cl = 1+(_n>1)+(_n>4)+(_n>9)+(_n>17)+(_n>27)+(_n>40)+(_n>55)+(_n>70)
    // Unequal groups WITH a singleton, on purpose: 9 clusters, the first of size 1.
    A.cl.resize(static_cast<size_t>(A.n));
    for (int64_t i = 0; i < A.n; ++i) {
        const double k = static_cast<double>(i + 1);  // Stata's _n is 1-based
        A.cl[static_cast<size_t>(i)] = 1 + (k > 1) + (k > 4) + (k > 9) + (k > 17) + (k > 27) +
                                       (k > 40) + (k > 55) + (k > 70);
    }
    return A;
}

// A dense Factor over a numeric column, `count` filled — compute_vcov's cluster
// branch reads both codes and n_levels, and the CGM factor uses the level count.
Factor make_cluster(const std::vector<double>& v) {
    Factor f;
    f.name = "cl";
    f.codes.resize(v.size());
    std::vector<double> lv;
    for (size_t i = 0; i < v.size(); ++i) {
        size_t k = 0;
        for (; k < lv.size(); ++k) if (lv[k] == v[i]) break;
        if (k == lv.size()) lv.push_back(v[i]);
        f.codes[i] = static_cast<int32_t>(k);
    }
    f.n_levels = static_cast<int>(lv.size());
    f.count.assign(static_cast<size_t>(f.n_levels), 0.0);
    for (int32_t c : f.codes) f.count[static_cast<size_t>(c)] += 1.0;
    return f;
}

// An IvModelData in cobra's own layout: Xexog = [intercept, included exog...],
// Zinst = excluded instruments, Xendog = the endogenous regressors.
IvModelData make_md(const Auto& A, const std::vector<const std::vector<double>*>& exog,
                    const std::vector<std::string>& exog_names,
                    const std::vector<const std::vector<double>*>& endog,
                    const std::vector<std::string>& endog_names,
                    const std::vector<const std::vector<double>*>& inst,
                    const std::vector<std::string>& inst_names,
                    const std::vector<double>* cluster = nullptr) {
    const int64_t n = A.n;
    IvModelData md;
    md.n = n;
    md.has_intercept = true;
    md.kexog = 1 + static_cast<int>(exog.size());
    md.kendog = static_cast<int>(endog.size());
    md.kinst = static_cast<int>(inst.size());
    md.Xexog.resize(static_cast<size_t>(n) * md.kexog);
    for (int64_t i = 0; i < n; ++i) md.Xexog[static_cast<size_t>(i)] = 1.0;
    md.exog_names.push_back("(intercept)");
    for (size_t j = 0; j < exog.size(); ++j) {
        std::copy(exog[j]->begin(), exog[j]->end(),
                  md.Xexog.begin() + static_cast<int64_t>(j + 1) * n);
        md.exog_names.push_back(exog_names[j]);
    }
    md.Xendog.resize(static_cast<size_t>(n) * md.kendog);
    for (size_t j = 0; j < endog.size(); ++j)
        std::copy(endog[j]->begin(), endog[j]->end(),
                  md.Xendog.begin() + static_cast<int64_t>(j) * n);
    md.endog_names = endog_names;
    md.Zinst.resize(static_cast<size_t>(n) * md.kinst);
    for (size_t j = 0; j < inst.size(); ++j)
        std::copy(inst[j]->begin(), inst[j]->end(),
                  md.Zinst.begin() + static_cast<int64_t>(j) * n);
    md.inst_names = inst_names;
    md.y.assign(A.price.begin(), A.price.end());
    if (cluster) md.cluster.push_back(make_cluster(*cluster));
    return md;
}

// cobra's own `regress <dep> <xs...>` with the requested vce — the right-hand side
// of the oracle's mreldif identities. The intercept sits at column 0 (build_design's
// house convention, which feols_fit's --no-intercept path depends on); reorder_like()
// below puts the result into ivreg2's cnZ1 order before any comparison, so the two
// sides are matched by NAME and no permutation can hide a real difference.
FeolsResult ols(const Auto& A, const std::vector<double>& dep,
                const std::vector<const std::vector<double>*>& xs,
                const std::vector<std::string>& xnames, Vcov vt,
                const std::vector<double>* cluster) {
    const int64_t n = A.n;
    ModelData md;
    md.dm.n = n;
    md.dm.k = 1 + static_cast<int>(xs.size());
    md.dm.X.resize(static_cast<size_t>(n) * md.dm.k);
    for (int64_t i = 0; i < n; ++i) md.dm.X[static_cast<size_t>(i)] = 1.0;
    md.dm.xnames.push_back("(intercept)");
    for (size_t j = 0; j < xs.size(); ++j) {
        std::copy(xs[j]->begin(), xs[j]->end(),
                  md.dm.X.begin() + static_cast<int64_t>(j + 1) * n);
        md.dm.xnames.push_back(xnames[j]);
    }
    md.dm.y.assign(dep.begin(), dep.end());
    md.has_intercept = true;
    if (cluster) md.cluster.push_back(make_cluster(*cluster));
    return feols_fit(md, vt, 0);
}

// Reorder an OLS fit's beta / vcov into `want` (the first stage's column order),
// matching by NAME. Aborts the comparison loudly if a name is absent rather than
// silently comparing a shorter vector.
void reorder_like(const FeolsResult& o, const std::vector<std::string>& want,
                  std::vector<double>& beta, std::vector<double>& V) {
    const int k = static_cast<int>(o.names.size());
    std::vector<int> ix;
    for (const auto& nm : want) {
        int at = -1;
        for (int j = 0; j < k; ++j) if (o.names[static_cast<size_t>(j)] == nm) { at = j; break; }
        if (at < 0) { check(false, "reorder_like: regress has no column named " + nm); return; }
        ix.push_back(at);
    }
    beta.assign(want.size(), 0.0);
    V.assign(want.size() * want.size(), 0.0);
    for (size_t a = 0; a < want.size(); ++a) {
        beta[a] = o.beta[static_cast<size_t>(ix[a])];
        for (size_t b = 0; b < want.size(); ++b)
            V[a + b * want.size()] =
                o.vcov_matrix[static_cast<size_t>(ix[a]) + static_cast<size_t>(ix[b]) * k];
    }
}
}  // namespace

int test_b4_savefirst_main() {
    std::printf("test_b4_savefirst:\n");
    const Auto A = load_auto();
    eqi(A.n, 74, "fixture: sysuse auto has 74 rows");

    // ================= arm A: robust, TWO endogenous regressors =================
    // ivreg2 price weight (mpg turn = headroom trunk length), robust savefirst
    {
        IvModelData md = make_md(A, {&A.weight}, {"weight"}, {&A.mpg, &A.turn}, {"mpg", "turn"},
                                 {&A.headroom, &A.trunk, &A.length},
                                 {"headroom", "trunk", "length"});
        IvResult r = iv2sls_fit(md, Vcov::HC1, 0, /*gmm=*/false, /*small=*/false,
                                /*savefirst=*/true);
        check(r.first_stage.size() == 2, "arm A: e(firsteqs) has TWO equations (fs_mpg fs_turn)");
        const IvFirstStage& fm = r.first_stage[0];
        const IvFirstStage& ft = r.first_stage[1];
        check(fm.depvar == "mpg" && ft.depvar == "turn", "arm A: one equation per endogenous");

        // CELL: cn_fs = "headroom trunk length weight _cons" — ivreg2's
        // cnZ1 order (excluded IVs, included exog, constant LAST). cobra spells the
        // constant "(intercept)"; the POSITION is the claim.
        const std::vector<std::string> want_names = {"headroom", "trunk", "length", "weight",
                                                     "(intercept)"};
        check(fm.names == want_names, "arm A cn_fs: exexog, inexog, then the constant");

        // CELLS: N | dfr | dfm | keq | rmse, all from the fs_mpg B4VAL line.
        eqi(fm.n, 74, "arm A fs_mpg e(N)");
        eqi(fm.df_r, 69, "arm A fs_mpg e(df_r)");
        eqi(fm.df_m, 4, "arm A fs_mpg e(df_m)");
        eqi(fm.k_eq, 2, "arm A fs_mpg e(k_eq) = the ENDOGENOUS count, not this eq's k");
        eqi(ft.k_eq, 2, "arm A fs_turn e(k_eq) is the same 2");
        near(fm.rmse, hx("0x1.bb0faf503f452p+1"), 1e-12, "arm A fs_mpg e(rmse)");
        near(ft.rmse, hx("0x1.19eaa6090e685p+1"), 1e-12, "arm A fs_turn e(rmse)");
        // CELLS: the two individual coefficients the log prints (b_headroom_mpg from
        // bF[1,1], b_headroom_turn from _b[headroom] after restoring fs_turn).
        near(fm.beta[0], hx("0x1.08447da49e935p-6"), 1e-9, "arm A fs_mpg _b[headroom]");
        near(ft.beta[0], -hx("0x1.0a0343d900164p-4"), 1e-9, "arm A fs_turn _b[headroom]");

        // IDENTITY: the oracle's mreldif_b / mreldif_V vs `regress mpg headroom trunk
        // length weight, robust` — order-matched, so no permutation is needed.
        FeolsResult o = ols(A, A.mpg, {&A.headroom, &A.trunk, &A.length, &A.weight},
                            {"headroom", "trunk", "length", "weight"}, Vcov::HC1, nullptr);
        std::vector<double> ob, oV;
        reorder_like(o, fm.names, ob, oV);
        const double mb = mreldif(fm.beta, ob);
        const double mv = mreldif(fm.vcov_matrix, oV);
        std::printf("  arm A  mreldif_b=%.3e mreldif_V=%.3e (Stata: 2.9e-14 / 1.5e-13)\n", mb, mv);
        check(mb < 1e-10, "arm A: first-stage b == regress, robust");
        check(mv < 1e-10, "arm A: first-stage V == regress, robust (the :3092 rescale IS HC0->HC1)");
        near(fm.rmse, o.rmse, 1e-12, "arm A: first-stage rmse == regress Root MSE");
        eqi(fm.df_r, o.df_r, "arm A: under ROBUST, df_r agrees with regress too (69)");

        // NULL 1 (the log's own): classical OLS must be FAR, or the parent's vce
        // never reached the first stage and the identity above is vacuous.
        FeolsResult n2 = ols(A, A.mpg, {&A.headroom, &A.trunk, &A.length, &A.weight},
                             {"headroom", "trunk", "length", "weight"}, Vcov::IID, nullptr);
        std::vector<double> nb, nV;
        reorder_like(n2, fm.names, nb, nV);
        const double mnull = mreldif(fm.vcov_matrix, nV);
        std::printf("  arm A  null mreldif_V vs classical OLS=%.3e (Stata: 9.3e-01)\n", mnull);
        check(mnull > 1e-3, "arm A NULL: classical OLS V is NOT what the first stage carries");

        // CELLS: e(first)[21,2] (ivreg2-savefirst.log:75-97), ivreg2's s_ffirst under
        // robust with TWO endogenous regressors -- the Sanderson-Windmeijer and
        // Angrist-Pischke rows are the conditional statistics there, not the F.
        const char* want_first[21][2] = {
            {"0x1.bb0faf503f452p+1", "0x1.19eaa6090e684p+1"},   // rmse
            {"0x1.c71bc517d5effp-9", "0x1.9c39c034e8fbep-7"},   // sheapr2
            {"0x1.dc49fb11dd350p-6", "0x1.af690cea598f6p-4"},   // pr2
            {"0x1.0810477013f73p-1", "0x1.41e0babd19fd8p+2"},   // F
            {"0x1.8p+1", "0x1.8p+1"},                           // df
            {"0x1.14p+6", "0x1.14p+6"},                         // df_r
            {"0x1.5879d87457eb8p-1", "0x1.adb238ef24112p-9"},   // pvalue
            {"0x1.924906212cd9ep-4", "0x1.7b68a50abe7cbp-4"},   // SWF
            {"0x1p+1", "0x1p+1"},                               // SWFdf1
            {"0x1.14p+6", "0x1.14p+6"},                         // SWFdf2
            {"0x1.d02b6a616de08p-1", "0x1.d2c3178c84a06p-1"},   // SWFp
            {"0x1.af6fb4f358e9ap-3", "0x1.96e6f3c8bd732p-3"},   // SWchi2
            {"0x1.ccd043e36e4f4p-1", "0x1.cf94fdc43681bp-1"},   // SWchi2p
            {"0x1.86c80318d05f7p-9", "0x1.a61ff7f650586p-9"},   // SWr2
            {"0x1.b6eb195d540b2p-4", "0x1.605c6dff1690ep-1"},   // APF
            {"0x1p+1", "0x1p+1"},                               // APFdf1
            {"0x1.14p+6", "0x1.14p+6"},                         // APFdf2
            {"0x1.cc0c87fc99cd2p-1", "0x1.030459603d338p-1"},   // APFp
            {"0x1.d6b95a4668f96p-3", "0x1.79e4fb884c24bp+0"},   // APchi2
            {"0x1.c86a27f5c5122p-1", "0x1.e981880836f78p-2"},   // APchi2p
            {"0x1.cf5d988d7be3ap-9", "0x1.c2cddf6d0c80fp-7"}};  // APr2
        const std::vector<std::string>& rows = ivreg2_first_stat_rows();
        check(fm.ffirst.size() == 21 && ft.ffirst.size() == 21, "arm A: e(first) has 21 rows");
        if (fm.ffirst.size() == 21 && ft.ffirst.size() == 21) {
            // sheapr2 (row 1) alone is held at 1e-8: it is a ratio of diagonals of two
            // inverted cross-product matrices on auto's badly scaled columns, and Stata's
            // cell is the less accurate side -- exact rational arithmetic on auto.dta
            // gives 0.003472202101888965 (mpg) and 0.012580126567024117 (turn); cobra is
            // within 4e-12 of both, Stata's cells 5.0e-9 and 4.2e-9 away.
            for (size_t i = 0; i < 21; ++i) {
                const double tol = i == 1 ? 1e-8 : 1e-9;
                near(fm.ffirst[i], hx(want_first[i][0]), tol, "arm A e(first)[" + rows[i] + ",mpg]");
                near(ft.ffirst[i], hx(want_first[i][1]), tol, "arm A e(first)[" + rows[i] + ",turn]");
            }
            near(fm.ffirst[1], 0.003472202101888965, 1e-10, "arm A sheapr2[mpg] vs exact rational");
            near(ft.ffirst[1], 0.012580126567024117, 1e-10, "arm A sheapr2[turn] vs exact rational");
        }
    }

    // ================= arm E: CLUSTER — 100% of the corpus shape ================
    // ivreg2 price weight (mpg = headroom trunk), cluster(cl) savefirst
    {
        IvModelData md = make_md(A, {&A.weight}, {"weight"}, {&A.mpg}, {"mpg"},
                                 {&A.headroom, &A.trunk}, {"headroom", "trunk"}, &A.cl);
        IvResult r = iv2sls_fit(md, Vcov::Cluster, 0, false, false, /*savefirst=*/true);
        check(r.first_stage.size() == 1, "arm E: one first-stage equation");
        const IvFirstStage& f = r.first_stage[0];
        const std::vector<std::string> want_names = {"headroom", "trunk", "weight", "(intercept)"};
        check(f.names == want_names, "arm E cn_fs: headroom trunk weight _cons");
        eqi(f.n, 74, "arm E e(N)");
        eqi(f.n_clusters, 9, "arm E e(N_clust) — unequal groups with a singleton");
        eqi(f.df_m, 3, "arm E e(df_m)");
        near(f.rmse, hx("0x1.bca412c3f2a82p+1"), 1e-12, "arm E e(rmse)");

        // THE divergence REFUTE.md found and the receipt never asked about:
        // ivreg2 posts N - iv1_ct EVEN under cluster(), where Stata's own
        // `regress, cluster()` posts M-1. Measured in the log as 70 against 8, so a
        // consumer's t/p/CI off a restored first stage use 70.
        eqi(f.df_r, 70, "arm E e(df_r) = N - iv1_ct (NOT M-1)");
        FeolsResult o = ols(A, A.mpg, {&A.headroom, &A.trunk, &A.weight},
                            {"headroom", "trunk", "weight"}, Vcov::Cluster, &A.cl);
        eqi(o.df_r, 8, "arm E: `regress, cluster()` posts M-1 = 8 on the SAME fit");
        check(f.df_r != o.df_r, "arm E: the two df_r genuinely differ (70 vs 8)");

        // IDENTITY through the OTHER ivreg2 branch (:3095-3096, a different formula
        // that also drops dofminus): b and V still match regress cell for cell.
        std::vector<double> ob, oV;
        reorder_like(o, f.names, ob, oV);
        const double mb = mreldif(f.beta, ob);
        const double mv = mreldif(f.vcov_matrix, oV);
        std::printf("  arm E  mreldif_b=%.3e mreldif_V=%.3e (Stata: 4.2e-14 / 9.6e-15)\n", mb, mv);
        check(mb < 1e-10, "arm E: first-stage b == regress, cluster(cl)");
        check(mv < 1e-10, "arm E: first-stage V == regress, cluster(cl)");
        near(f.rmse, o.rmse, 1e-12, "arm E: first-stage rmse == regress Root MSE");

        // NULL 3 (the log's own): plain robust must be FAR, or cluster() never
        // reached the first stage.
        FeolsResult nr = ols(A, A.mpg, {&A.headroom, &A.trunk, &A.weight},
                             {"headroom", "trunk", "weight"}, Vcov::HC1, nullptr);
        std::vector<double> nb, nV;
        reorder_like(nr, f.names, nb, nV);
        const double mnull = mreldif(f.vcov_matrix, nV);
        std::printf("  arm E  null mreldif_V vs plain robust=%.3e (Stata: 1.6e-01)\n", mnull);
        check(mnull > 1e-3, "arm E NULL: plain-robust V is NOT what the first stage carries");
    }

    // ============ arm F: INVARIANT to the parent's `small` (help ivreg2) ==========
    // The log measures fs_mreldif_V_small_vs_not = +0.0000000000000X-3ff — EXACTLY
    // zero — while the PARENT's own V moves by 1.0422. Both halves are asserted:
    // a first stage that quietly inherited the parent's convention would be silently
    // wrong on every corpus line, and a probe that did not also move the parent
    // would prove nothing.
    {
        auto fit = [&](bool small) {
            IvModelData md = make_md(A, {&A.weight}, {"weight"}, {&A.mpg}, {"mpg"},
                                     {&A.headroom, &A.trunk}, {"headroom", "trunk"});
            return iv2sls_fit(md, Vcov::HC1, 0, false, small, /*savefirst=*/true);
        };
        IvResult ns = fit(false), sm = fit(true);
        const double mv = mreldif(ns.first_stage[0].vcov_matrix, sm.first_stage[0].vcov_matrix);
        std::printf("  arm F  fs mreldif_V small-vs-not=%.3e (Stata: exactly 0)\n", mv);
        check(mv == 0.0, "arm F: the first stage is EXACTLY invariant to the parent's --small");
        eqi(ns.first_stage[0].df_r, sm.first_stage[0].df_r, "arm F: df_r invariant too");
        const double parent_ratio = (sm.vcov_matrix[0] / ns.vcov_matrix[0]);
        std::printf("  arm F  PARENT ratio_V11 small-vs-not=%.6f (Stata: 1.042)\n", parent_ratio);
        near(parent_ratio, hx("0x1.0ad12073615a2p+0"), 1e-9,
             "arm F: the parent's own V DOES move (the probe is not vacuous)");
    }

    std::printf(g_fail ? "  RESULT: FAIL\n" : "  RESULT: ok\n");
    return g_fail;
}
