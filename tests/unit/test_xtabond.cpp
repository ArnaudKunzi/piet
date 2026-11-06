// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — xtabond oracle assertions (StataBE 19.5,
// internal capture p7_stata.log: `xtset id t` then
// `xtabond y L.y x1 x2, lags(1) vce(robust)`).
//
// FIXTURE. The oracle legs read the wave artifact
// internal fixture wave_fixture.dta directly. That
// file is a GENERATED oracle input and is gitignored, so it is resolved in this
// order — $COBRA_WAVE_FIXTURE (a file), $COBRA_PANGRAM_ARTIFACTS (a directory),
// then <validate>/../kitchensink/pangram/waves/_artifacts/ — and when it is
// absent every oracle leg is SKIPPED LOUDLY (with the paths tried) rather than
// silently passing. The synthetic legs at the end need no fixture and always
// run: they check the GMM first-order conditions and, on a just-identified
// design, the moment conditions themselves — insurance that is independent of
// Stata entirely.
//
// Tolerances follow the wave rule — coefficients rtol 1e-6, standard errors
// rtol 1e-5 — widened, per assertion, to half of the last digit Stata actually
// PRINTED, because that rounding (not our arithmetic) is the binding
// uncertainty on a 7-significant-digit oracle value.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "estimators/posthest.h"
#include "estimators/xtabond.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail_count = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg.c_str());
        fail_count = 1;
    }
}

// `o` is the value Stata PRINTED; `unit` is that print's last-digit unit (1e-7
// for ".7018551", 1e-2 for "4249.48"). A value passes when it is within the
// wave's relative tolerance OR within half a printed digit — whichever is
// looser — so an assertion never fails on digits the oracle never showed.
static void near(double c, double o, double rtol, double unit, const std::string& what) {
    const double tol = std::max(rtol * std::fabs(o), 0.5 * unit);
    if (!(std::fabs(c - o) <= tol)) {
        std::printf("  FAIL: %s — got %.10g, oracle %.10g (|d| %.3g > tol %.3g)\n", what.c_str(), c, o,
                    std::fabs(c - o), tol);
        fail_count = 1;
    }
}

namespace {

// ---------------------------------------------------------------------------
// A tiny panel container the two halves of this file share. `x` is column-major
// n*kx exactly as xtabond_fit wants it.
// ---------------------------------------------------------------------------
struct Panel {
    std::vector<double> id, t, y, x;
    int kx = 0;
    std::vector<std::string> xnames;
    int64_t n() const { return static_cast<int64_t>(id.size()); }
};

int index_of(const XtabondResult& R, const std::string& nm) {
    for (int i = 0; i < static_cast<int>(R.names.size()); ++i)
        if (R.names[static_cast<size_t>(i)] == nm) return i;
    return -1;
}

double inf_norm(const std::vector<double>& v) {
    double m = 0.0;
    for (double e : v) m = std::max(m, std::fabs(e));
    return m;
}

// The one place the oracle command's `L.y` is built: a genuine lag by (id,
// time) value, independent of the estimator's own internal lagging.
std::vector<double> lag_by_key(const Panel& P, const std::vector<double>& col, int k) {
    std::map<std::pair<double, double>, int64_t> idx;
    for (int64_t r = 0; r < P.n(); ++r) idx[{P.id[static_cast<size_t>(r)], P.t[static_cast<size_t>(r)]}] = r;
    std::vector<double> out(static_cast<size_t>(P.n()), std::nan(""));
    for (int64_t r = 0; r < P.n(); ++r) {
        auto it = idx.find({P.id[static_cast<size_t>(r)], P.t[static_cast<size_t>(r)] - k});
        if (it != idx.end()) out[static_cast<size_t>(r)] = col[static_cast<size_t>(it->second)];
    }
    return out;
}

}  // namespace

// ===========================================================================
// 1. the P7 oracle
// ===========================================================================
static void oracle_legs() {
    const cobra_test::ValidateBase base;
    std::vector<std::string> tried;
    std::string dta;
    if (const char* env = std::getenv("COBRA_WAVE_FIXTURE")) {
        if (*env) { dta = env; tried.push_back(dta); }
    }
    if (dta.empty()) {
        if (const char* env = std::getenv("COBRA_PANGRAM_ARTIFACTS")) {
            if (*env) {
                std::string d = env;
                if (d.back() != '/') d += '/';
                dta = d + "wave_fixture.dta";
                tried.push_back(dta);
            }
        }
    }
    if (dta.empty() || !std::ifstream(dta.c_str(), std::ios::binary).good()) {
        const std::string fallback = base.str() + "../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
        tried.push_back(fallback);
        dta = fallback;
    }
    if (!std::ifstream(dta.c_str(), std::ios::binary).good()) {
        std::printf("  [SKIP] the P7 wave fixture (wave_fixture.dta) is not present — every ORACLE\n"
                    "         assertion below was skipped. It is a generated, gitignored oracle\n"
                    "         input; set COBRA_WAVE_FIXTURE (file) or COBRA_PANGRAM_ARTIFACTS\n"
                    "         (directory) to run them. Paths tried:\n");
        for (const auto& p : tried) std::printf("           %s\n", p.c_str());
        return;
    }

    DtaResult dr = read_dta(dta);
    const Frame& fr = dr.frame;
    check(fr.n_rows() == 4800, "fixture: wave_fixture.dta has 4,800 rows");

    auto column = [&](const char* nm) {
        const auto& c = fr.col(nm).f64;
        return std::vector<double>(c.data(), c.data() + c.size());
    };
    Panel P;
    P.id = column("id");
    P.t = column("t");
    P.y = column("y");
    const std::vector<double> x1 = column("x1");
    const std::vector<double> x2 = column("x2");
    {  // the fixture's own missing-value shape, so a regenerated artifact cannot
       // silently change what the numbers below are pinned to
        int64_t my = 0, m2 = 0;
        for (int64_t r = 0; r < fr.n_rows(); ++r) {
            if (!std::isfinite(P.y[static_cast<size_t>(r)])) ++my;
            if (!std::isfinite(x2[static_cast<size_t>(r)])) ++m2;
        }
        check(my == 22, "fixture: y has 22 missing values (got " + std::to_string((long long)my) + ")");
        check(m2 == 129, "fixture: x2 has 129 missing values (got " + std::to_string((long long)m2) + ")");
    }

    // `xtabond y L.y x1 x2, lags(1)` — the typed varlist, L.y included, exactly
    // as the oracle command wrote it. The redundant L.y is the point: it is
    // omitted from the MODEL but keeps its standard-instrument column.
    const std::vector<double> ly = lag_by_key(P, P.y, 1);
    P.kx = 3;
    P.xnames = {"L.y", "x1", "x2"};
    P.x.reserve(static_cast<size_t>(P.n()) * 3);
    P.x.insert(P.x.end(), ly.begin(), ly.end());
    P.x.insert(P.x.end(), x1.begin(), x1.end());
    P.x.insert(P.x.end(), x2.begin(), x2.end());

    XtabondSpec spec;  // lags(1), delta 1, constant
    XtabondResult R = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx, P.xnames,
                                  P.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
    check(R.ok, "xtabond: the fit succeeds — " + R.err);
    if (!R.ok) return;

    // ---- the note ---------------------------------------------------------
    // "note: L.y omitted because of collinearity."
    check(R.omitted.size() == 1 && R.omitted[0] == "L.y",
          "xtabond: exactly `L.y` is omitted for collinearity (got " +
              std::to_string((long long)R.omitted.size()) + " omitted)");
    check(R.names.size() == 4 && R.names[0] == "L1.y" && R.names[1] == "x1" && R.names[2] == "x2" &&
              R.names[3] == "_cons",
          "xtabond: the surviving coefficients are L1.y, x1, x2, _cons");

    // ---- the sample -------------------------------------------------------
    check(R.n_obs == 4103, "xtabond: Number of obs = 4,103 (got " + std::to_string((long long)R.n_obs) + ")");
    check(R.n_groups == 200, "xtabond: Number of groups = 200");
    check(R.min_obs == 17, "xtabond: obs per group min = 17 (got " + std::to_string(R.min_obs) + ")");
    check(R.max_obs == 22, "xtabond: obs per group max = 22 (got " + std::to_string(R.max_obs) + ")");
    near(R.avg_obs, 20.515, 1e-6, 1e-3, "xtabond: obs per group avg");

    // ---- the instrument count --------------------------------------------
    // 253 GMM-type + 3 standard (differenced) + 1 level = 257. The 253 is
    // SUM_{t=3..24}(t-2): one column per (period, available lag of y) pair,
    // zero-filled for panels that lack the observation rather than dropped.
    check(R.n_instruments == 257,
          "xtabond: Number of instruments = 257 (got " + std::to_string(R.n_instruments) + ")");
    check(R.n_gmm_instruments == 253,
          "xtabond: 253 GMM-type instruments (got " + std::to_string(R.n_gmm_instruments) + ")");
    check(R.n_std_instruments == 3,
          "xtabond: 3 standard differenced instruments — the OMITTED L.y keeps its column "
          "(got " + std::to_string(R.n_std_instruments) + ")");
    check(R.n_level_instruments == 1, "xtabond: 1 level instrument (_cons)");
    check(R.gmm_label == "L(2/.).y", "xtabond: footer GMM-type label is L(2/.).y");
    check(R.std_labels.size() == 3 && R.std_labels[0] == "LD.y" && R.std_labels[1] == "D.x1" &&
              R.std_labels[2] == "D.x2",
          "xtabond: footer standard instruments are `LD.y D.x1 D.x2`");

    // ---- coefficients and robust standard errors --------------------------
    const int ily = index_of(R, "L1.y"), ix1 = index_of(R, "x1"), ix2 = index_of(R, "x2"),
              ic = index_of(R, "_cons");
    auto B = [&](int i) { return R.beta[static_cast<size_t>(i)]; };
    auto S = [&](int i) { return R.se[static_cast<size_t>(i)]; };
    near(B(ily), 0.7018551, 1e-6, 1e-7, "xtabond: L1.y coefficient");
    near(B(ix1), 0.2739377, 1e-6, 1e-7, "xtabond: x1 coefficient");
    near(B(ix2), 0.0351889, 1e-6, 1e-7, "xtabond: x2 coefficient");
    near(B(ic), 0.8199228, 1e-6, 1e-7, "xtabond: _cons coefficient");
    near(S(ily), 0.0130415, 1e-5, 1e-7, "xtabond: L1.y robust std. err.");
    near(S(ix1), 0.0296933, 1e-5, 1e-7, "xtabond: x1 robust std. err.");
    near(S(ix2), 0.0325504, 1e-5, 1e-7, "xtabond: x2 robust std. err.");
    near(S(ic), 0.0386548, 1e-5, 1e-7, "xtabond: _cons robust std. err.");
    near(B(ily) / S(ily), 53.82, 1e-4, 1e-2, "xtabond: L1.y z");
    near(B(ix1) / S(ix1), 9.23, 1e-3, 1e-2, "xtabond: x1 z");
    near(B(ix2) / S(ix2), 1.08, 1e-2, 1e-2, "xtabond: x2 z");
    near(B(ic) / S(ic), 21.21, 1e-3, 1e-2, "xtabond: _cons z");

    const double zc = z_critical(0.05);
    near(B(ily) - zc * S(ily), 0.6762942, 1e-6, 1e-7, "xtabond: L1.y 95% CI lower");
    near(B(ily) + zc * S(ily), 0.7274161, 1e-6, 1e-7, "xtabond: L1.y 95% CI upper");
    near(B(ix1) - zc * S(ix1), 0.21574, 1e-6, 1e-6, "xtabond: x1 95% CI lower");
    near(B(ix1) + zc * S(ix1), 0.3321354, 1e-6, 1e-7, "xtabond: x1 95% CI upper");
    near(B(ix2) - zc * S(ix2), -0.0286088, 1e-5, 1e-7, "xtabond: x2 95% CI lower");
    near(B(ix2) + zc * S(ix2), 0.0989866, 1e-5, 1e-7, "xtabond: x2 95% CI upper");
    near(B(ic) - zc * S(ic), 0.7441608, 1e-6, 1e-7, "xtabond: _cons 95% CI lower");
    near(B(ic) + zc * S(ic), 0.8956849, 1e-6, 1e-7, "xtabond: _cons 95% CI upper");

    // ---- the header Wald --------------------------------------------------
    // Wald chi2(3): the three SLOPES, `_cons` excluded.
    check(R.chi2_df == 3, "xtabond: Wald df = 3 (the slopes, _cons excluded)");
    near(R.chi2, 4249.48, 1e-6, 1e-2, "xtabond: Wald chi2(3)");
    check(R.p_chi2 < 5e-5, "xtabond: Prob > chi2 = 0.0000");

    // ---- the FIRST-ORDER CONDITIONS, on the real data ---------------------
    // Q_xz A1 (Z'u) = 0 at the solution regardless of identification. Scaled by
    // the same quantity's natural magnitude so the test is not a bare epsilon.
    {
        double scale = 0.0;
        for (double e : R.zu) scale = std::max(scale, std::fabs(e));
        check(inf_norm(R.foc) <= 1e-8 * std::max(1.0, scale),
              "xtabond: the GMM first-order conditions hold at the reported solution");
    }

    // ---- WHY THE H MATRIX IS BLOCK DIAGONAL -------------------------------
    // [XT] xtdpd builds H1i = diag(H_d, H_L) — the differenced and level rows
    // do NOT couple, despite cov(D.eps_t, eps_s) being nonzero. The observable
    // consequence, which Stata states outright ("Including the constant does
    // not affect the other parameter estimates because it is identified only by
    // the level errors", [XT] xtabond example 5), is that dropping the level
    // equation must leave the three slopes bit-for-bit alone. A coupled H would
    // move them. ASSERTED, not argued:
    {
        XtabondSpec nc = spec;
        nc.constant = false;
        XtabondResult Rn = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx,
                                       P.xnames, P.n(), nc, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(Rn.ok, "xtabond(noconstant): the fit succeeds — " + Rn.err);
        if (Rn.ok) {
            check(Rn.n_instruments == 256, "xtabond(noconstant): 257 - 1 = 256 instruments");
            for (int j = 0; j < 3; ++j)
                check(std::fabs(Rn.beta[static_cast<size_t>(j)] - R.beta[static_cast<size_t>(j)]) <=
                          1e-10 * std::max(1.0, std::fabs(R.beta[static_cast<size_t>(j)])),
                      "xtabond: dropping the level equation leaves slope " + R.names[static_cast<size_t>(j)] +
                          " unchanged (H is block diagonal — the level moment identifies ONLY _cons)");
        }
    }

    // ---- WHY THE LEVEL SAMPLE IS ITS OWN SAMPLE ---------------------------
    // The level equation needs one fewer lag of y and no lagged covariate, so
    // it runs on 4,433 rows against the differenced equation's 4,103. Because
    // the level moment is exactly satisfied, `_cons` IS the mean level residual
    // over those rows — and the differenced sample's mean residual is a
    // different, WRONG number (.8284683). Both halves asserted, so the row rule
    // cannot silently revert.
    check(R.n_level_obs == 4433,
          "xtabond: the level equation runs on 4,433 rows (got " +
              std::to_string((long long)R.n_level_obs) + ")");
    {
        std::map<std::pair<double, double>, int64_t> idx;
        for (int64_t r = 0; r < P.n(); ++r) idx[{P.id[static_cast<size_t>(r)], P.t[static_cast<size_t>(r)]}] = r;
        auto at = [&](int64_t r, int k) -> int64_t {
            auto it = idx.find({P.id[static_cast<size_t>(r)], P.t[static_cast<size_t>(r)] - k});
            return it == idx.end() ? -1 : it->second;
        };
        auto resid = [&](int64_t r) {
            return P.y[static_cast<size_t>(r)] - B(ily) * P.y[static_cast<size_t>(at(r, 1))] -
                   B(ix1) * x1[static_cast<size_t>(r)] - B(ix2) * x2[static_cast<size_t>(r)];
        };
        auto ok_lvl = [&](int64_t r) {
            const int64_t r1 = at(r, 1);
            return r1 >= 0 && std::isfinite(P.y[static_cast<size_t>(r)]) &&
                   std::isfinite(P.y[static_cast<size_t>(r1)]) && std::isfinite(x1[static_cast<size_t>(r)]) &&
                   std::isfinite(x2[static_cast<size_t>(r)]);
        };
        double sl = 0.0, sd = 0.0;
        int64_t nl = 0, nd = 0;
        for (int64_t r = 0; r < P.n(); ++r) {
            if (!ok_lvl(r)) continue;
            sl += resid(r);
            ++nl;
            const int64_t r2 = at(r, 2), r1 = at(r, 1);
            if (r2 < 0 || !std::isfinite(P.y[static_cast<size_t>(r2)]) ||
                !std::isfinite(x1[static_cast<size_t>(r1)]) || !std::isfinite(x2[static_cast<size_t>(r1)]))
                continue;
            sd += resid(r);
            ++nd;
        }
        check(nl == 4433, "xtabond: the independently rebuilt level sample is also 4,433 rows");
        check(nd == 4103, "xtabond: the independently rebuilt differenced sample is also 4,103 rows");
        near(sl / static_cast<double>(nl), 0.8199228, 1e-6, 1e-7,
             "xtabond: _cons IS the mean level residual over the LEVEL sample");
        check(std::fabs(sd / static_cast<double>(nd) - 0.8199228) > 1e-3,
              "xtabond: the mean residual over the DIFFERENCED sample is a DIFFERENT number "
              "(so the level-sample rule is load-bearing, not free)");
    }

    // ---- WHY PERIOD ADJACENCY IN H_d IS LOAD-BEARING ----------------------
    // H_d's -.5 belongs to rows one PERIOD apart. That is only distinguishable
    // from "rows adjacent in the sample" when the sample HAS interior gaps —
    // which this fixture does (min 17 of a possible 22 per panel). If a future
    // fixture were gap-free the oracle match above would stop testing the rule,
    // so the gap count is asserted rather than assumed.
    check(R.n_period_gaps > 0,
          "xtabond: the estimation sample has interior period gaps (" +
              std::to_string((long long)R.n_period_gaps) +
              "), so H_d's period-adjacency rule is pinned by the oracle match above");

    // ---- SECOND AND THIRD ARMS, same fixture, OUT-OF-BAND capture ---------
    // The P7 wave pins exactly one xtabond call, and one call cannot tell
    // L(2/.).y from L(p+1/.).y — they coincide at lags(1). They do NOT coincide
    // at lags(2), and the plausible-looking p+1 reading is WRONG: it puts the
    // instrument count at 234 where Stata says 255. So two more arms were
    // captured by hand from the SAME StataBE 19.5 on the SAME fixture and are
    // pinned here. They are deliberately kept out of the frozen pangram corpus
    // (which is not ours to extend); to re-capture, run against
    // internal fixture wave_fixture.dta:
    //
    //     xtset id t
    //     xtabond y x1 x2, lags(2) vce(robust)
    //     xtabond y,       lags(1) vce(robust)
    {
        XtabondSpec s2;
        s2.lags = 2;
        Panel Q;  // the same fixture, but the varlist is `x1 x2` — no typed L.y
        Q.id = P.id;
        Q.t = P.t;
        Q.y = P.y;
        Q.kx = 2;
        Q.xnames = {"x1", "x2"};
        Q.x.insert(Q.x.end(), x1.begin(), x1.end());
        Q.x.insert(Q.x.end(), x2.begin(), x2.end());
        XtabondResult R2 = xtabond_fit(Q.id.data(), Q.t.data(), Q.y.data(), Q.x.data(), Q.kx,
                                       Q.xnames, Q.n(), s2, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(R2.ok, "xtabond(lags 2): the fit succeeds — " + R2.err);
        if (R2.ok) {
            check(R2.n_obs == 3899, "xtabond(lags 2): Number of obs = 3,899 (got " +
                                        std::to_string((long long)R2.n_obs) + ")");
            check(R2.min_obs == 15 && R2.max_obs == 21, "xtabond(lags 2): obs per group 15..21");
            near(R2.avg_obs, 19.495, 1e-6, 1e-3, "xtabond(lags 2): obs per group avg");
            // 252 = SUM_{t=4..24}(t-2): the GMM depth stays at lag 2. The p+1
            // reading would give 231 GMM columns and 234 instruments.
            check(R2.n_gmm_instruments == 252,
                  "xtabond(lags 2): the GMM depth is 2 for EVERY p — 252 columns, not the 231 the "
                  "L(p+1/.) reading would give (got " + std::to_string(R2.n_gmm_instruments) + ")");
            check(R2.n_instruments == 255,
                  "xtabond(lags 2): Number of instruments = 255 (got " +
                      std::to_string(R2.n_instruments) + ")");
            check(R2.gmm_label == "L(2/.).y", "xtabond(lags 2): the footer still reads L(2/.).y");
            check(R2.names.size() == 5 && R2.names[0] == "L1.y" && R2.names[1] == "L2.y",
                  "xtabond(lags 2): both autoregressive terms are in the model");
            near(R2.beta[0], 1.034633, 1e-6, 1e-6, "xtabond(lags 2): L1.y coefficient");
            near(R2.beta[1], -0.1041548, 1e-6, 1e-7, "xtabond(lags 2): L2.y coefficient");
            near(R2.beta[2], 0.0736582, 1e-6, 1e-7, "xtabond(lags 2): x1 coefficient");
            near(R2.beta[3], -0.0015396, 1e-5, 1e-7, "xtabond(lags 2): x2 coefficient");
            near(R2.beta[4], 0.2190531, 1e-6, 1e-7, "xtabond(lags 2): _cons coefficient");
            near(R2.se[0], 0.0194068, 1e-5, 1e-7, "xtabond(lags 2): L1.y robust std. err.");
            near(R2.se[1], 0.0119622, 1e-5, 1e-7, "xtabond(lags 2): L2.y robust std. err.");
            near(R2.se[2], 0.0201743, 1e-5, 1e-7, "xtabond(lags 2): x1 robust std. err.");
            near(R2.se[3], 0.0202413, 1e-5, 1e-7, "xtabond(lags 2): x2 robust std. err.");
            near(R2.se[4], 0.0313274, 1e-5, 1e-7, "xtabond(lags 2): _cons robust std. err.");
            check(R2.chi2_df == 4, "xtabond(lags 2): Wald df = 4 (two lags + two covariates)");
            near(R2.chi2, 13680.17, 1e-6, 1e-2, "xtabond(lags 2): Wald chi2(4)");
        }

        // `if` IS A MASK, against Stata this time. Same out-of-band capture:
        //     xtabond y x1 x2 if t != 12, lags(1) vce(robust)
        // gives 3,916 obs and 246 instruments; excluding period 12 by DELETION
        // (`drop if t == 12` then re-xtset) gives 3,543 and 213 instead,
        // because deletion takes period 13's lag down with it. 373 rows apart —
        // this is the assertion that makes the mask route load-bearing on real
        // data, not just on the synthetic panel below.
        {
            std::vector<char> mask(static_cast<size_t>(P.n()), 1);
            for (int64_t r = 0; r < P.n(); ++r)
                if (P.t[static_cast<size_t>(r)] == 12) mask[static_cast<size_t>(r)] = 0;
            XtabondSpec sm;
            XtabondResult Rm = xtabond_fit(Q.id.data(), Q.t.data(), Q.y.data(), Q.x.data(), Q.kx,
                                           Q.xnames, Q.n(), sm, mask.data(),
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
            check(Rm.ok, "xtabond(if t!=12): the fit succeeds — " + Rm.err);
            if (Rm.ok) {
                check(Rm.n_obs == 3916, "xtabond(if t!=12): Number of obs = 3,916 — NOT the 3,543 "
                                        "deletion would give (got " +
                                            std::to_string((long long)Rm.n_obs) + ")");
                check(Rm.n_instruments == 246,
                      "xtabond(if t!=12): Number of instruments = 246 — NOT 213 (got " +
                          std::to_string(Rm.n_instruments) + ")");
                check(Rm.min_obs == 16 && Rm.max_obs == 21, "xtabond(if t!=12): obs per group 16..21");
                near(Rm.avg_obs, 19.58, 1e-6, 1e-2, "xtabond(if t!=12): obs per group avg");
                near(Rm.beta[0], 0.9342921, 1e-6, 1e-7, "xtabond(if t!=12): L1.y coefficient");
                near(Rm.beta[1], 0.070413, 1e-6, 1e-6, "xtabond(if t!=12): x1 coefficient");
                near(Rm.beta[2], -0.0247984, 1e-6, 1e-7, "xtabond(if t!=12): x2 coefficient");
                near(Rm.beta[3], 0.2141181, 1e-6, 1e-7, "xtabond(if t!=12): _cons coefficient");
                near(Rm.se[0], 0.0166568, 1e-5, 1e-7, "xtabond(if t!=12): L1.y robust std. err.");
                near(Rm.se[1], 0.0315973, 1e-5, 1e-7, "xtabond(if t!=12): x1 robust std. err.");
                near(Rm.se[2], 0.0309989, 1e-5, 1e-7, "xtabond(if t!=12): x2 robust std. err.");
                near(Rm.se[3], 0.0459718, 1e-5, 1e-7, "xtabond(if t!=12): _cons robust std. err.");
                near(Rm.chi2, 5707.76, 1e-6, 1e-2, "xtabond(if t!=12): Wald chi2(3)");
            }
        }

        XtabondSpec s1;  // `xtabond y, lags(1)` — no covariates at all
        XtabondResult R1 = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), nullptr, 0, {}, P.n(), s1, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(R1.ok, "xtabond(bare): the fit succeeds — " + R1.err);
        if (R1.ok) {
            check(R1.n_obs == 4338, "xtabond(bare): Number of obs = 4,338 (got " +
                                        std::to_string((long long)R1.n_obs) + ")");
            check(R1.min_obs == 19 && R1.max_obs == 22, "xtabond(bare): obs per group 19..22");
            near(R1.avg_obs, 21.69, 1e-6, 1e-2, "xtabond(bare): obs per group avg");
            check(R1.n_instruments == 254 && R1.n_std_instruments == 0,
                  "xtabond(bare): 253 GMM + 0 standard + 1 level = 254 instruments (got " +
                      std::to_string(R1.n_instruments) + ")");
            near(R1.beta[0], 0.9319072, 1e-6, 1e-7, "xtabond(bare): L1.y coefficient");
            near(R1.beta[1], 0.2521211, 1e-6, 1e-7, "xtabond(bare): _cons coefficient");
            near(R1.se[0], 0.0074795, 1e-5, 1e-7, "xtabond(bare): L1.y robust std. err.");
            near(R1.se[1], 0.0252649, 1e-5, 1e-7, "xtabond(bare): _cons robust std. err.");
            check(R1.chi2_df == 1, "xtabond(bare): Wald df = 1");
            near(R1.chi2, 15524.08, 1e-6, 1e-2, "xtabond(bare): Wald chi2(1)");
        }
    }

    if (fail_count == 0)
        std::printf("  oracle: 4,103 obs / 200 groups / 257 instruments / 4 coefficients + SEs / "
                    "Wald chi2(3) matched; lags(2) and bare arms matched too\n");
}

// ===========================================================================
// 2. synthetic legs — no Stata anywhere in sight
// ===========================================================================
namespace {

// Deterministic panel AR(1) with a unit effect. `gap_every` drops one interior
// period from every gap_every-th panel, so the unbalanced construction rules
// (zero-filled GMM columns, period-adjacent H) are actually exercised.
Panel synth(int G, int T, double delta_ar, int kx, int gap_every, uint64_t seed) {
    Panel P;
    P.kx = kx;
    for (int c = 0; c < kx; ++c) P.xnames.push_back("z" + std::to_string(c + 1));
    uint64_t s = seed;
    auto u01 = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) / 9007199254740992.0;
    };
    auto nrm = [&]() {  // Box-Muller, one draw
        const double a = std::max(u01(), 1e-12), b = u01();
        return std::sqrt(-2.0 * std::log(a)) * std::cos(6.283185307179586 * b);
    };
    std::vector<std::vector<double>> xc(static_cast<size_t>(kx));
    for (int g = 1; g <= G; ++g) {
        const double nu = nrm();
        double prev = nu / std::max(1e-6, 1.0 - delta_ar) + nrm();
        for (int t = 1; t <= T; ++t) {
            double xb = 0.0;
            std::vector<double> xs(static_cast<size_t>(kx));
            for (int c = 0; c < kx; ++c) { xs[static_cast<size_t>(c)] = nrm(); xb += 0.5 * xs[static_cast<size_t>(c)]; }
            const double yt = 1.0 + delta_ar * prev + xb + nu + nrm();
            prev = yt;
            if (gap_every > 0 && g % gap_every == 0 && t == T / 2) continue;  // interior hole
            P.id.push_back(g);
            P.t.push_back(t);
            P.y.push_back(yt);
            for (int c = 0; c < kx; ++c) xc[static_cast<size_t>(c)].push_back(xs[static_cast<size_t>(c)]);
        }
    }
    for (int c = 0; c < kx; ++c) P.x.insert(P.x.end(), xc[static_cast<size_t>(c)].begin(), xc[static_cast<size_t>(c)].end());
    return P;
}

}  // namespace

static void synthetic_legs() {
    // ---- (a) JUST IDENTIFIED: the moment conditions themselves are zero ----
    // G panels, T = 3, lags(1), no covariates. The differenced equation exists
    // only at t = 3 and has exactly one GMM instrument (y_1); the level
    // equation contributes `_cons`. So m = 2 = k, the system is exactly
    // identified, and Z'u must vanish — not just its projection.
    {
        Panel P = synth(/*G=*/300, /*T=*/3, /*delta_ar=*/0.5, /*kx=*/0, /*gap_every=*/0, 20260805u);
        XtabondSpec spec;
        XtabondResult R = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), nullptr, 0, {}, P.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(R.ok, "synthetic(just-identified): the fit succeeds — " + R.err);
        if (R.ok) {
            check(R.n_instruments == 2 && R.k == 2,
                  "synthetic(just-identified): m = k = 2 (one GMM lag + _cons)");
            check(R.n_obs == 300, "synthetic(just-identified): one differenced row per panel");
            check(R.n_level_obs == 600, "synthetic(just-identified): two level rows per panel");
            double scale = 0.0;
            for (int64_t r = 0; r < P.n(); ++r) scale += std::fabs(P.y[static_cast<size_t>(r)]);
            check(inf_norm(R.zu) <= 1e-9 * std::max(1.0, scale),
                  "synthetic(just-identified): Z'u = 0 — every moment condition is satisfied "
                  "EXACTLY at the solution (|Z'u|_inf = " + std::to_string(inf_norm(R.zu)) + ")");

            // Closed form: with one instrument the AR coefficient is the
            // Anderson-Hsiao ratio, and _cons is the mean level residual.
            std::map<std::pair<double, double>, int64_t> idx;
            for (int64_t r = 0; r < P.n(); ++r) idx[{P.id[static_cast<size_t>(r)], P.t[static_cast<size_t>(r)]}] = r;
            auto Y = [&](double g, double t) { return P.y[static_cast<size_t>(idx.at({g, t}))]; };
            double num = 0.0, den = 0.0;
            for (int g = 1; g <= 300; ++g) {
                num += Y(g, 1) * (Y(g, 3) - Y(g, 2));
                den += Y(g, 1) * (Y(g, 2) - Y(g, 1));
            }
            const double d_hat = num / den;
            check(std::fabs(R.beta[0] - d_hat) <= 1e-9 * std::max(1.0, std::fabs(d_hat)),
                  "synthetic(just-identified): L1.y equals the closed-form Anderson-Hsiao IV ratio");
            double sm = 0.0;
            for (int g = 1; g <= 300; ++g)
                for (int t = 2; t <= 3; ++t) sm += Y(g, t) - d_hat * Y(g, t - 1);
            check(std::fabs(R.beta[1] - sm / 600.0) <= 1e-9,
                  "synthetic(just-identified): _cons equals the mean level residual");
        }
    }

    // ---- (b) OVERIDENTIFIED, unbalanced: the FOCs hold, Z'u does not -------
    // The same check that would be vacuous above: with 100+ moments and 4
    // parameters Z'u is emphatically NONZERO, and it is Q_xz A1 Z'u that
    // vanishes. Asserting both directions is what makes the FOC leg a test.
    {
        Panel P = synth(/*G=*/120, /*T=*/10, /*delta_ar=*/0.6, /*kx=*/2, /*gap_every=*/3, 77777u);
        XtabondSpec spec;
        XtabondResult R = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx, P.xnames,
                                      P.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(R.ok, "synthetic(overidentified): the fit succeeds — " + R.err);
        if (R.ok) {
            // t = 3..10 with lags 1..t-2 = 1+2+...+8 = 36 GMM columns, + 2
            // standard + 1 level = 39. The panels WITH a hole still get every
            // column (zero-filled), which is the whole point of the rule.
            check(R.n_gmm_instruments == 36,
                  "synthetic(overidentified): SUM_{t=3..10}(t-2) = 36 GMM columns, unaffected by the "
                  "holes (got " + std::to_string(R.n_gmm_instruments) + ")");
            check(R.n_instruments == 39, "synthetic(overidentified): 36 + 2 + 1 = 39 instruments");
            check(R.k == 4, "synthetic(overidentified): L1.y + 2 covariates + _cons");
            check(R.n_period_gaps > 0, "synthetic(overidentified): the holes really do gap the sample");
            const double zs = inf_norm(R.zu);
            check(zs > 1e-3, "synthetic(overidentified): Z'u is NOT zero (the system is overidentified, "
                             "so the FOC check below is not vacuous)");
            check(inf_norm(R.foc) <= 1e-8 * std::max(1.0, zs),
                  "synthetic(overidentified): Q_xz A1 Z'u = 0 — the GMM first-order conditions hold");
            check(std::isfinite(R.chi2) && R.chi2 > 0 && R.chi2_df == 3,
                  "synthetic(overidentified): the Wald covers the 3 slopes and is finite");
            for (int j = 0; j < R.k; ++j)
                check(R.se[static_cast<size_t>(j)] > 0,
                      "synthetic(overidentified): a positive robust std. err. for " +
                          R.names[static_cast<size_t>(j)]);
        }
    }

    // ---- (c) `if` is a MASK, not a deletion -------------------------------
    // Excluding period 5 from the estimation sample must not disturb the lags
    // of periods 6 and 7: the row is still there to be lagged. Physically
    // deleting it would take those two periods down with it. The two counts
    // differ by 100 rows here, so the rule is pinned, not asserted into a tie.
    {
        Panel P = synth(/*G=*/50, /*T=*/8, /*delta_ar=*/0.5, /*kx=*/1, /*gap_every=*/0, 5150u);
        XtabondSpec spec;
        XtabondResult R0 = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx,
                                       P.xnames, P.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(R0.ok && R0.n_obs == 300, "synthetic(if): unrestricted, t = 3..8 gives 6 rows x 50 panels");

        std::vector<char> mask(static_cast<size_t>(P.n()), 1);
        for (int64_t r = 0; r < P.n(); ++r)
            if (P.t[static_cast<size_t>(r)] == 5) mask[static_cast<size_t>(r)] = 0;
        XtabondResult Rm = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx,
                                       P.xnames, P.n(), spec, mask.data(),
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(Rm.ok && Rm.n_obs == 250,
              "synthetic(if): masking period 5 costs exactly its own 50 rows — periods 6 and 7 "
              "still find their lags (got " + std::to_string((long long)(Rm.ok ? Rm.n_obs : -1)) + ")");

        Panel D;  // the same exclusion done by DELETION, which is the wrong answer
        D.kx = P.kx;
        D.xnames = P.xnames;
        for (int64_t r = 0; r < P.n(); ++r) {
            if (!mask[static_cast<size_t>(r)]) continue;
            D.id.push_back(P.id[static_cast<size_t>(r)]);
            D.t.push_back(P.t[static_cast<size_t>(r)]);
            D.y.push_back(P.y[static_cast<size_t>(r)]);
            D.x.push_back(P.x[static_cast<size_t>(r)]);
        }
        XtabondResult Rd = xtabond_fit(D.id.data(), D.t.data(), D.y.data(), D.x.data(), D.kx,
                                       D.xnames, D.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(Rd.ok && Rd.n_obs == 150,
              "synthetic(if): DELETING period 5 instead would also kill periods 6 and 7 — the two "
              "readings of `if` are 250 vs 150 rows apart, so the mask route is load-bearing");
    }

    // ---- (d) honest rejections --------------------------------------------
    {
        Panel P = synth(60, 6, 0.4, 1, 0, 4242u);
        XtabondSpec spec;
        spec.lags = 9;  // more lags than the panel is long
        XtabondResult R = xtabond_fit(P.id.data(), P.t.data(), P.y.data(), P.x.data(), P.kx, P.xnames,
                                      P.n(), spec, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(!R.ok && !R.err.empty(),
              "synthetic: lags() deeper than the panel is rejected, not answered");

        // A duplicated (id, time) pair makes every lag ambiguous.
        Panel D = synth(20, 5, 0.4, 1, 0, 99u);
        D.id.push_back(D.id[0]);
        D.t.push_back(D.t[0]);
        D.y.push_back(0.0);
        D.x.push_back(0.0);  // kx == 1, so the column stays contiguous
        XtabondSpec s2;
        XtabondResult R2 = xtabond_fit(D.id.data(), D.t.data(), D.y.data(), D.x.data(), D.kx, D.xnames,
                                       D.n(), s2, nullptr,
                              /*conventional_vce=*/false);  // this file pins the vce(robust) oracle (BUG-383 flipped the DEFAULT to conventional)
        check(!R2.ok && R2.err.find("uniquely xtset") != std::string::npos,
              "synthetic: a repeated (id, time) pair is rejected as not uniquely xtset");
    }
}

int test_xtabond_main() {
    std::printf("test_xtabond:\n");
    oracle_legs();
    synthetic_legs();
    if (fail_count == 0) std::printf("  all xtabond assertions passed\n");
    return fail_count;
}
