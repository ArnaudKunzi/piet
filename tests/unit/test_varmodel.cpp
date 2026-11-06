// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — varmodel oracle assertions (Stata 19.5,
// internal capture p7_stata.log, the
// `preserve / keep if id==1 / tsset t` block of p7.do, plus the `.irf` artifact
// that block writes at _runs/p7_irf.irf).
//
// FIXTURE. kFixture below is the id==1 subset of the wave fixture
// internal fixture wave_fixture.dta — 24 rows of
// (t, y, x1, x2), written with Python's shortest-round-trip float repr so every
// literal is the SAME double the .dta holds. The wave artifact is a generated
// oracle input and is gitignored, so embedding it is what makes these assertions
// run in a bare worktree; when the .dta IS present (COBRA_WAVE_FIXTURE, or the
// pangram tree next to tests/validate) the last leg re-reads it and asserts the
// literals match it cell for cell, so the two can never drift apart silently.
//
// ============================================================================
// WHAT THIS FIXTURE CANNOT PIN, AND WHY  (read before adding an assertion)
// ============================================================================
// In the id==1 subset x1 is EXACTLY a deterministic function of its own two
// lags (it is a sampled sinusoid: x1_t = 1.9796 x1_{t-1} - x1_{t-2} + c). Its
// equation therefore fits perfectly — Stata prints R-sq 1.0000 and RMSE 0 — and
// Sigma_ml's x1 diagonal is whatever the last bits of cancellation happen to
// leave behind. Stata's solver lands on ~4.0e-27; the normal-equations path here
// lands on ~5.3e-21. Both are "zero"; neither is a number.
//
// Consequently the following are NOT asserted against the oracle here:
//
//   * det(Sigma_ml), Log likelihood, FPE, AIC, HQIC, SBIC. Every one of them is
//     a function of ln det(Sigma_ml), which on this fixture is dominated by
//     ln(epsilon) from the collapsed x1 residual. Stata prints det 3.47e-39 and
//     LL 880.4857; this implementation gets det ~4.6e-33 and LL ~725. That gap
//     is floating-point noise in a degenerate determinant, not an implementation
//     difference — and papering over it (clipping the diagonal, dropping the
//     equation, pseudo-inverting) would be inventing a number.
//     WHAT IS ASSERTED INSTEAD, below: (a) the formulas are pinned on a
//     synthetic non-degenerate K=2 VAR against an independently computed
//     Gaussian log-likelihood; (b) the ORACLE'S OWN numbers are used to close
//     the formula chain — Stata's printed LL implies a det that rounds to
//     Stata's printed det, and an FPE/AIC/HQIC/SBIC that round to Stata's
//     printed FPE/AIC/HQIC/SBIC. That pins the transformations without
//     pretending we can reproduce the determinant.
//
//   * The x1 equation's RMSE. Stata prints exactly 0; any honest refit prints a
//     tiny positive number. Stata's own Sigma_ml[x1,x1] is NOT zero (its
//     oirf(0)_x1<-x1 is 6.35e-14), so the printed 0 is a cancellation artifact
//     of its RMSE path, not a value to reproduce.
//
//   * Every IRF cell whose response OR impulse is x1, EXCEPT the pure-
//     coefficient columns of the x1<-x1 cell. Those cells are functions of the
//     collapsed x1 residual and disagree with the oracle by factors of 10^3.
//
// The x1 equation's chi2 and its three vargranger rows ARE asserted — as
// `missing with df 0`, which is exactly what Stata prints and exactly what the
// degeneracy guard must produce.
// ============================================================================
//
// Tolerances follow the wave rule — coefficients rtol 1e-6, standard errors
// rtol 1e-5 — widened, per assertion, to half of the last digit Stata actually
// PRINTED, because that rounding (not our arithmetic) is the binding uncertainty
// on a 6-or-7-significant-digit oracle value.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "estimators/posthest.h"
#include "estimators/varmodel.h"
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
// for ".2040346", 1e-4 for "370.7666", ...). A value passes when it is within the
// wave's relative tolerance OR within half a printed digit — whichever is looser
// — so an assertion never fails on digits the oracle never showed.
static void near(double c, double o, double rtol, double unit, const std::string& what) {
    const double tol = std::max(rtol * std::fabs(o), 0.5 * unit);
    if (!(std::fabs(c - o) <= tol)) {
        std::printf("  FAIL: %s — got %.10g, oracle %.10g (|d| %.3g > tol %.3g)\n", what.c_str(), c,
                    o, std::fabs(c - o), tol);
        fail_count = 1;
    }
}

namespace {

// Spelled out rather than <cmath>'s M_PI (a POSIX extension MSVC only defines
// under _USE_MATH_DEFINES), matching src/estimators/varmodel.cpp.
constexpr double kPi = 3.14159265358979323846;

// t, y, x1, x2 — the id==1 subset of wave_fixture.dta, 24 rows, t = 1..24.
const double kFixture[24][4] = {
    {1.0, 1.9158851358153135, 0.14737172979226365, 1.0292039470338963},
    {2.0, 2.219819333953883, 0.28684285212220995, 1.0501832251313379},
    {3.0, 2.5068840119394, 0.420571854993052, 1.0630398636251923},
    {4.0, 2.7678462716339016, 0.5458342133588315, 1.067942974881715},
    {5.0, 2.9944823594947216, 0.6600778971785186, 1.0651273576399745},
    {6.0, 3.179862754939629, 0.7609753651467324, 1.0548915578624578},
    {7.0, 3.318588356590034, 0.8464709848078965, 1.0375954031118573},
    {8.0, 3.406969121067582, 0.914822912941124, 1.013657030817056},
    {9.0, 3.4431387232630684, 0.9646385829666849, 0.9835494349711864},
    {10.0, 4.627101264010661, 0.9949030763721239, 0.9477965597818008},
    {11.0, 4.560708654912362, 1.0049998001333682, 0.9069689725348065},
    {12.0, 4.447569959075408, 0.9947230488598214, 0.8616791514089996},
    {13.0, 4.292896557541311, 0.9642821957288404, 0.8125764271580646},
    {14.0, 4.103289443442236, 0.9142974268256817, 0.7603416204355375},
    {15.0, 3.886477125075328, 0.8457871057952504, 0.7056814190518486},
    {16.0, 3.6510144609175876, 0.7601470262316581, 0.6493225416003963},
    {17.0, 3.405954183316868, 0.6591219745065957, 0.592005735653943},
    {18.0, 3.16050383858612, 0.5447701824006438, 0.5344796600988565},
    {19.0, 2.9236813432135116, 0.41942139376109533, 0.47749470213161466},
    {20.0, 2.7039823123109223, 0.28562939951435684, 0.4217967799815877},
    {21.0, 2.5090717610175033, 0.14612000805986722, 0.3681211825420047},
    {22.0, 2.345511736251706, 0.00373551106962271, 0.3171864967861909},
    {23.0, 2.218534948012617, -0.13862322388181803, 0.26968867312116096},
    {24.0, 2.1318725968217676, -0.2780558540822556, 0.22629527769146818},
};

const VarIrfCell* cell(const VarResult& r, int step, int response, int impulse) {
    for (const VarIrfCell& c : r.irf)
        if (c.step == step && c.response == response && c.impulse == impulse) return &c;
    return nullptr;
}

const VarGrangerRow* grow(const VarResult& r, const std::string& eq, const std::string& ex) {
    for (const VarGrangerRow& g : r.granger)
        if (g.equation == eq && g.excluded == ex) return &g;
    return nullptr;
}

int rindex(const VarResult& r, const std::string& name) {
    for (size_t i = 0; i < r.rnames.size(); ++i)
        if (r.rnames[i] == name) return static_cast<int>(i);
    return -1;
}

// Coefficient + SE assertion against the printed oracle row.
void coef(const VarResult& r, int eq, const char* rn, double b_o, double b_unit, double se_o,
          double se_unit) {
    const int j = rindex(r, rn);
    const std::string tag = r.eqs[static_cast<size_t>(eq)].name + "/" + rn;
    if (j < 0) {
        check(false, "regressor " + std::string(rn) + " is present");
        return;
    }
    near(r.eqs[static_cast<size_t>(eq)].b[static_cast<size_t>(j)], b_o, 1e-6, b_unit, tag + " b");
    near(r.eqs[static_cast<size_t>(eq)].se[static_cast<size_t>(j)], se_o, 1e-5, se_unit, tag + " se");
}

// Phi_s from a K x Kp coefficient block A (row-major), by the standard recursion
// Phi_0 = I, Phi_s = sum_j Phi_{s-j} A_j. Deliberately a DIFFERENT derivation from
// the companion-power path the estimator uses, so agreement is informative.
std::vector<double> phi_rec(const std::vector<double>& A, int K, int p, int s) {
    std::vector<std::vector<double>> Ph(static_cast<size_t>(s) + 1);
    Ph[0].assign(static_cast<size_t>(K) * K, 0.0);
    for (int i = 0; i < K; ++i) Ph[0][static_cast<size_t>(i) * K + i] = 1.0;
    for (int u = 1; u <= s; ++u) {
        Ph[static_cast<size_t>(u)].assign(static_cast<size_t>(K) * K, 0.0);
        for (int j = 1; j <= p && j <= u; ++j)
            for (int a = 0; a < K; ++a)
                for (int b = 0; b < K; ++b) {
                    double acc = 0;
                    for (int c = 0; c < K; ++c)
                        acc += Ph[static_cast<size_t>(u - j)][static_cast<size_t>(a) * K + c] *
                               A[static_cast<size_t>(c) * (K * p) + ((j - 1) * K + b)];
                    Ph[static_cast<size_t>(u)][static_cast<size_t>(a) * K + b] += acc;
                }
    }
    return Ph[static_cast<size_t>(s)];
}

}  // namespace

int test_varmodel_main() {
    std::printf("test_varmodel:\n");
    
    // ========================================================================
    // A. SYNTHETIC NON-DEGENERATE K = 2 VAR(1)
    // The P7 fixture cannot pin the header scalars (see the banner), so the
    // formulas are pinned here, on data where Sigma is honestly full rank.
    // ========================================================================
    {
        const int K = 2, p = 1;
        const int64_t n = 300;
        std::vector<double> Y(static_cast<size_t>(n) * 2, 0.0), tm(static_cast<size_t>(n));
        uint64_t st = 20260805ULL;
        auto unif = [&]() {
            st = st * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>(st >> 11) * (1.0 / 9007199254740992.0);
        };
        double y0 = 0.0, y1 = 0.0;
        for (int64_t t = 0; t < n; ++t) {
            const double u1 = std::max(unif(), 1e-12), u2 = unif();
            const double rad = std::sqrt(-2.0 * std::log(u1));
            const double z1 = rad * std::cos(2.0 * kPi * u2), z2 = rad * std::sin(2.0 * kPi * u2);
            const double e1 = 0.40 * z1;
            const double e2 = 0.55 * e1 + 0.30 * z2;  // correlated => Sigma is not diagonal
            const double ny0 = 0.10 + 0.50 * y0 + 0.20 * y1 + e1;
            const double ny1 = -0.20 - 0.30 * y0 + 0.40 * y1 + e2;
            y0 = ny0;
            y1 = ny1;
            tm[static_cast<size_t>(t)] = static_cast<double>(t + 1);
            Y[static_cast<size_t>(t)] = y0;
            Y[static_cast<size_t>(n) + static_cast<size_t>(t)] = y1;
        }
        const std::vector<std::string> nm = {"a", "b"};
        VarResult r = var_fit(Y.data(), tm.data(), n, nm, p, 5, 1.0);
        check(r.ok, "synthetic: var_fit succeeds");
        check(r.n_obs == n - p, "synthetic: T = n - p");
        check(r.m == K * p + 1, "synthetic: m = K*p + 1");
        check(r.sigma_pd, "synthetic: Sigma is positive definite");
        for (int a = 0; a < K; ++a)
            check(!r.eqs[static_cast<size_t>(a)].degenerate,
                  "synthetic: equation " + nm[static_cast<size_t>(a)] + " is NOT degenerate");

        const int64_t T = r.n_obs;
        const int m = r.m;

        // --- (1) header scalars vs an INDEPENDENT computation ---------------
        // The log likelihood is rebuilt the long way: reconstruct the residuals
        // from the reported coefficients, invert Sigma, and sum the multivariate
        // normal log densities. Nothing about the closed form -(T/2)(K ln 2pi +
        // ln|Sigma| + K) is reused, so a wrong constant or a wrong divisor shows.
        {
            std::vector<double> E(static_cast<size_t>(T) * static_cast<size_t>(K), 0.0);
            for (int64_t q = 0; q < T; ++q)
                for (int a = 0; a < K; ++a) {
                    double fit = r.eqs[static_cast<size_t>(a)].b[static_cast<size_t>(K * p)];  // _cons
                    for (int j = 1; j <= p; ++j)
                        for (int c = 0; c < K; ++c)
                            fit += r.eqs[static_cast<size_t>(a)].b[static_cast<size_t>((j - 1) * K + c)] *
                                   Y[static_cast<size_t>(c) * static_cast<size_t>(n) +
                                     static_cast<size_t>(q + p - j)];
                    E[static_cast<size_t>(q) * static_cast<size_t>(K) + static_cast<size_t>(a)] =
                        Y[static_cast<size_t>(a) * static_cast<size_t>(n) + static_cast<size_t>(q + p)] - fit;
                }
            // det via the closed form for K = 2 — a different path from the
            // estimator's Cholesky.
            const double s11 = r.sigma[0], s12 = r.sigma[1], s22 = r.sigma[3];
            const double det = s11 * s22 - s12 * s12;
            near(r.det_sigma, det, 1e-12, 0.0, "synthetic: det(Sigma) == s11*s22 - s12^2");
            std::vector<double> Si = r.sigma;
            check(la::spd_inverse(Si.data(), K) == 0, "synthetic: Sigma inverts");
            double quad = 0;
            for (int64_t q = 0; q < T; ++q)
                for (int a = 0; a < K; ++a)
                    for (int b = 0; b < K; ++b)
                        quad += E[static_cast<size_t>(q) * static_cast<size_t>(K) + static_cast<size_t>(a)] *
                                Si[static_cast<size_t>(a) * static_cast<size_t>(K) + static_cast<size_t>(b)] *
                                E[static_cast<size_t>(q) * static_cast<size_t>(K) + static_cast<size_t>(b)];
            const double Td = static_cast<double>(T);
            const double ll_direct = -0.5 * (Td * static_cast<double>(K) * std::log(2.0 * kPi) +
                                             Td * std::log(det) + quad);
            near(r.ll, ll_direct, 1e-10, 0.0,
                 "synthetic: LL == sum_t log phi(e_t; 0, Sigma) computed directly");
            // The trace identity that makes the closed form valid: at the ML
            // Sigma the quadratic form is exactly T*K. Asserted so a future
            // divisor change cannot pass silently.
            near(quad, Td * static_cast<double>(K), 1e-10, 0.0,
                 "synthetic: sum_t e_t' Sigma_ml^-1 e_t == T*K (ML Sigma uses divisor T)");

            const double tp = static_cast<double>(K) * static_cast<double>(m);
            near(r.fpe, std::pow((Td + m) / (Td - m), K) * det, 1e-12, 0.0, "synthetic: FPE");
            near(r.aic, -2.0 * ll_direct / Td + 2.0 * tp / Td, 1e-10, 0.0, "synthetic: AIC");
            near(r.hqic, -2.0 * ll_direct / Td + 2.0 * std::log(std::log(Td)) * tp / Td, 1e-10, 0.0,
                 "synthetic: HQIC");
            near(r.sbic, -2.0 * ll_direct / Td + std::log(Td) * tp / Td, 1e-10, 0.0, "synthetic: SBIC");
        }

        // --- (2) oirf(0) IS the Cholesky factor -----------------------------
        {
            for (int a = 0; a < K; ++a)
                for (int b = 0; b < K; ++b) {
                    const VarIrfCell* c = cell(r, 0, a, b);
                    check(c != nullptr, "synthetic: cell(0) exists");
                    if (!c) continue;
                    check(c->irf == ((a == b) ? 1.0 : 0.0), "synthetic: irf(0) == I");
                    check(c->stdirf == 0.0,
                          "synthetic: stdirf(0) == 0 exactly (Phi_0 = I is not estimated)");
                    if (b > a) check(c->oirf == 0.0, "synthetic: oirf(0) is lower triangular");
                }
            // P P' == Sigma
            for (int a = 0; a < K; ++a)
                for (int b = 0; b < K; ++b) {
                    double s = 0;
                    for (int c2 = 0; c2 < K; ++c2)
                        s += cell(r, 0, a, c2)->oirf * cell(r, 0, b, c2)->oirf;
                    near(s, r.sigma[static_cast<size_t>(a) * K + b], 1e-12, 0.0,
                         "synthetic: oirf(0) oirf(0)' == Sigma");
                }
        }

        // --- (3) stdirf(1) IS the coefficient SE ----------------------------
        // Phi_1 == A_1, so the delta-method SE at step 1 must land exactly on the
        // reported coefficient standard error. This is the sharpest available
        // check on the Kronecker index arithmetic in G_s / Sigma_alpha.
        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b) {
                const int j = rindex(r, "L1." + nm[static_cast<size_t>(b)]);
                near(cell(r, 1, a, b)->stdirf, r.eqs[static_cast<size_t>(a)].se[static_cast<size_t>(j)],
                     1e-10, 0.0, "synthetic: stdirf(1) == SE of the L1 coefficient");
                near(cell(r, 1, a, b)->irf, r.eqs[static_cast<size_t>(a)].b[static_cast<size_t>(j)],
                     1e-12, 0.0, "synthetic: irf(1) == the L1 coefficient");
            }

        // --- (4) stdirf(s>1) vs a FINITE-DIFFERENCE delta method ------------
        // Independent derivation of the same variance: numerically differentiate
        // vec(Phi_s) in the K*Kp coefficients and sandwich with the reported
        // system covariance. Nothing here reuses the closed-form Kronecker sum.
        {
            const int Kp = K * p, KM = K * m;
            std::vector<double> A(static_cast<size_t>(K) * static_cast<size_t>(Kp));
            for (int a = 0; a < K; ++a)
                for (int rr = 0; rr < Kp; ++rr)
                    A[static_cast<size_t>(a) * static_cast<size_t>(Kp) + static_cast<size_t>(rr)] =
                        r.eqs[static_cast<size_t>(a)].b[static_cast<size_t>(rr)];
            for (int s = 2; s <= 4; ++s) {
                // J[(a,rr)] = d vec(Phi_s) / d A[a][rr]
                std::vector<std::vector<double>> Jac(static_cast<size_t>(K) * static_cast<size_t>(Kp));
                for (int a = 0; a < K; ++a)
                    for (int rr = 0; rr < Kp; ++rr) {
                        const size_t idx = static_cast<size_t>(a) * static_cast<size_t>(Kp) +
                                           static_cast<size_t>(rr);
                        const double h = 1e-6 * std::max(1.0, std::fabs(A[idx]));
                        std::vector<double> Ap = A, Am = A;
                        Ap[idx] += h;
                        Am[idx] -= h;
                        const std::vector<double> fp = phi_rec(Ap, K, p, s), fm = phi_rec(Am, K, p, s);
                        Jac[idx].resize(static_cast<size_t>(K) * static_cast<size_t>(K));
                        for (size_t q = 0; q < fp.size(); ++q)
                            Jac[idx][q] = (fp[q] - fm[q]) / (2.0 * h);
                    }
                for (int a = 0; a < K; ++a)
                    for (int b = 0; b < K; ++b) {
                        double v = 0;
                        for (int a1 = 0; a1 < K; ++a1)
                            for (int r1 = 0; r1 < Kp; ++r1)
                                for (int a2 = 0; a2 < K; ++a2)
                                    for (int r2 = 0; r2 < Kp; ++r2)
                                        v += Jac[static_cast<size_t>(a1) * static_cast<size_t>(Kp) +
                                                 static_cast<size_t>(r1)]
                                                [static_cast<size_t>(a) * static_cast<size_t>(K) +
                                                 static_cast<size_t>(b)] *
                                             Jac[static_cast<size_t>(a2) * static_cast<size_t>(Kp) +
                                                 static_cast<size_t>(r2)]
                                                [static_cast<size_t>(a) * static_cast<size_t>(K) +
                                                 static_cast<size_t>(b)] *
                                             r.vcov[static_cast<size_t>(a1 * m + r1) *
                                                        static_cast<size_t>(KM) +
                                                    static_cast<size_t>(a2 * m + r2)];
                        const double sd = std::sqrt(std::max(0.0, v));
                        near(cell(r, s, a, b)->stdirf, sd, 2e-6, 0.0,
                             "synthetic: stdirf(" + std::to_string(s) +
                                 ") matches a finite-difference delta method");
                    }
            }
        }

        // --- (5) FEVD rows sum to 1; (6) the mse recursion; (7) the definition
        for (int s = 1; s <= r.irf_steps; ++s)
            for (int a = 0; a < K; ++a) {
                double sum = 0;
                for (int b = 0; b < K; ++b) sum += cell(r, s, a, b)->fevd;
                near(sum, 1.0, 1e-12, 0.0,
                     "synthetic: fevd row (step " + std::to_string(s) + ") sums to 1 across impulses");
                // mse(h)^2 - mse(h-1)^2 == sum_k oirf(h-1)^2
                double inc = 0;
                for (int b = 0; b < K; ++b) {
                    const double th = cell(r, s - 1, a, b)->oirf;
                    inc += th * th;
                }
                const double m2 = cell(r, s, a, 0)->mse * cell(r, s, a, 0)->mse;
                const double m1 = cell(r, s - 1, a, 0)->mse * cell(r, s - 1, a, 0)->mse;
                near(m2 - m1, inc, 1e-10, 0.0,
                     "synthetic: mse(h)^2 - mse(h-1)^2 == sum_k oirf(h-1)^2");
                // fevd * mse^2 == the cumulative squared oirf for that impulse
                for (int b = 0; b < K; ++b) {
                    double acc = 0;
                    for (int u = 0; u < s; ++u) {
                        const double th = cell(r, u, a, b)->oirf;
                        acc += th * th;
                    }
                    near(cell(r, s, a, b)->fevd * m2, acc, 1e-10, 0.0,
                         "synthetic: fevd * mse^2 == cumulative oirf^2");
                }
            }
        check(cell(r, 0, 0, 0)->mse == 0.0 && cell(r, 0, 0, 0)->fevd == 0.0,
              "synthetic: mse(0) == 0 and fevd(0) == 0");

        // --- (8) cirf / coirf really are the partial sums --------------------
        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b) {
                double ci = 0, co = 0;
                for (int s = 0; s <= r.irf_steps; ++s) {
                    ci += cell(r, s, a, b)->irf;
                    co += cell(r, s, a, b)->oirf;
                    near(cell(r, s, a, b)->cirf, ci, 1e-12, 0.0, "synthetic: cirf is the partial sum");
                    near(cell(r, s, a, b)->coirf, co, 1e-12, 0.0, "synthetic: coirf is the partial sum");
                }
            }
    }

    // ========================================================================
    // B. THE P7 ORACLE
    // ========================================================================
    const int64_t n = 24;
    std::vector<double> Y(static_cast<size_t>(n) * 3), tm(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        tm[static_cast<size_t>(i)] = kFixture[i][0];
        for (int c = 0; c < 3; ++c)
            Y[static_cast<size_t>(c) * static_cast<size_t>(n) + static_cast<size_t>(i)] =
                kFixture[i][c + 1];
    }
    const std::vector<std::string> vars = {"y", "x1", "x2"};
    VarResult r = var_fit(Y.data(), tm.data(), n, vars, 2, 6, 1.0);
    check(r.ok, "var_fit succeeds on the P7 fixture");
    if (!r.ok) return 1;

    // ---- header / sample ---------------------------------------------------
    check(r.n_obs == 22, "var: Number of obs = 22 — got " + std::to_string((long long)r.n_obs));
    near(r.t_first, 3.0, 1e-12, 0.0, "var: Sample starts at t = 3");
    near(r.t_last, 24.0, 1e-12, 0.0, "var: Sample ends at t = 24");
    check(r.m == 7, "var: Parms = 7 per equation");
    check(r.lags == 2 && r.K == 3, "var: 3 variables, 2 lags");

    // ---- equation table ----------------------------------------------------
    near(r.eqs[0].rmse, 0.215325, 1e-6, 1e-6, "var: y RMSE");
    near(r.eqs[0].r2, 0.9440, 1e-4, 1e-4, "var: y R-sq");
    check(r.eqs[0].chi2_ok, "var: the y equation has a chi2");
    near(r.eqs[0].chi2, 370.7666, 1e-6, 1e-4, "var: y chi2");
    check(r.eqs[0].chi2_df == 6, "var: y chi2 df = 6 (every coefficient but the constant)");
    near(r.eqs[0].p, 0.0, 1e-6, 1e-4, "var: y P>chi2");

    // The x1 equation: Stata prints R-sq 1.0000 and chi2 `.` with df 0.
    check(r.eqs[1].degenerate, "var: the x1 equation is flagged degenerate");
    check(!r.eqs[1].chi2_ok, "var: x1 chi2 is MISSING (Stata's `.`)");
    check(r.eqs[1].chi2_df == 0, "var: x1 chi2 df = 0");
    check(std::isnan(r.eqs[1].chi2), "var: x1 chi2 is NaN, not a manufactured number");
    near(r.eqs[1].r2, 1.0, 1e-6, 1e-4, "var: x1 R-sq = 1.0000");
    // (x1 RMSE is deliberately NOT asserted — see the banner.)

    near(r.eqs[2].rmse, 6.5e-06, 1e-2, 1e-7, "var: x2 RMSE");
    near(r.eqs[2].r2, 1.0, 1e-6, 1e-4, "var: x2 R-sq = 1.0000");
    check(r.eqs[2].chi2_ok, "var: the x2 equation has a chi2");
    near(r.eqs[2].chi2, 6.23e+10, 2e-3, 0.0, "var: x2 chi2 (oracle printed to 3 significant digits)");
    check(!r.eqs[2].degenerate,
          "var: the x2 equation is NOT degenerate (R-sq prints 1.0000 but the residual variance is "
          "1e10 times the degeneracy threshold)");

    // ---- coefficient block: the y equation ---------------------------------
    coef(r, 0, "L1.y", 0.3344127, 1e-7, 0.2040346, 1e-7);
    coef(r, 0, "L2.y", -0.273153, 1e-6, 0.1938274, 1e-7);
    coef(r, 0, "L1.x1", 54.80852, 1e-5, 15.4447, 1e-4);
    coef(r, 0, "L2.x1", -38.13551, 1e-5, 10.97063, 1e-5);
    coef(r, 0, "L1.x2", 136.9601, 1e-4, 61.64551, 1e-5);
    coef(r, 0, "L2.x2", -174.8694, 1e-4, 73.18342, 1e-5);
    coef(r, 0, "_cons", 28.39448, 1e-5, 9.067245, 1e-6);
    // z and P>|z| for the first printed row.
    near(r.eqs[0].b[0] / r.eqs[0].se[0], 1.64, 1e-3, 1e-2, "var: y/L1.y z");

    // ---- coefficient block: the x2 equation --------------------------------
    coef(r, 2, "L1.y", -1.74e-06, 1e-8, 6.13e-06, 1e-8);
    coef(r, 2, "L2.y", 9.40e-06, 1e-8, 5.82e-06, 1e-8);
    coef(r, 2, "L1.x1", 0.0246278, 1e-7, 0.0004637, 1e-7);
    coef(r, 2, "L2.x1", -0.0242693, 1e-7, 0.0003293, 1e-7);
    coef(r, 2, "L1.x2", 1.919842, 1e-6, 0.0018506, 1e-7);
    coef(r, 2, "L2.x2", -0.9390945, 1e-7, 0.002197, 1e-6);
    coef(r, 2, "_cons", 0.0098797, 1e-7, 0.0002722, 1e-7);

    // ---- vargranger --------------------------------------------------------
    {
        const VarGrangerRow* g;
        g = grow(r, "y", "x1");
        check(g && g->ok && g->df == 2, "vargranger: y <- x1 has df 2");
        if (g && g->ok) {
            near(g->chi2, 12.623, 1e-4, 1e-3, "vargranger: y <- x1 chi2");
            near(g->p, 0.002, 1e-2, 1e-3, "vargranger: y <- x1 Prob > chi2");
        }
        g = grow(r, "y", "x2");
        check(g && g->ok && g->df == 2, "vargranger: y <- x2 has df 2");
        if (g && g->ok) near(g->chi2, 12.158, 1e-4, 1e-3, "vargranger: y <- x2 chi2");
        g = grow(r, "y", "ALL");
        check(g && g->ok && g->df == 4, "vargranger: y <- ALL has df 4");
        if (g && g->ok) near(g->chi2, 34.313, 1e-4, 1e-3, "vargranger: y <- ALL chi2");

        // The whole x1 block prints `.` with df 0 — the degeneracy guard's whole point.
        for (const char* ex : {"y", "x2", "ALL"}) {
            g = grow(r, "x1", ex);
            check(g != nullptr, std::string("vargranger: row x1 <- ") + ex + " exists");
            if (!g) continue;
            check(!g->ok, std::string("vargranger: x1 <- ") + ex + " is MISSING (Stata's `.`)");
            check(g->df == 0, std::string("vargranger: x1 <- ") + ex + " has df 0");
            check(std::isnan(g->chi2),
                  std::string("vargranger: x1 <- ") + ex + " chi2 is NaN, not pseudo-inverted");
        }

        g = grow(r, "x2", "y");
        check(g && g->ok && g->df == 2, "vargranger: x2 <- y has df 2");
        if (g && g->ok) {
            near(g->chi2, 2.6578, 1e-4, 1e-4, "vargranger: x2 <- y chi2");
            near(g->p, 0.265, 1e-2, 1e-3, "vargranger: x2 <- y Prob > chi2");
        }
        g = grow(r, "x2", "x1");
        check(g && g->ok && g->df == 2, "vargranger: x2 <- x1 has df 2");
        if (g && g->ok) near(g->chi2, 10557.0, 1e-4, 1.0, "vargranger: x2 <- x1 chi2");
        g = grow(r, "x2", "ALL");
        check(g && g->ok && g->df == 4, "vargranger: x2 <- ALL has df 4");
        if (g && g->ok) near(g->chi2, 11588.0, 1e-4, 1.0, "vargranger: x2 <- ALL chi2");
        check(r.granger.size() == 9, "vargranger: 9 rows (3 equations x {A, B, ALL})");
    }

    // ---- the header-scalar FORMULA chain, closed on the oracle's own numbers
    // Our det/LL/FPE/AIC/HQIC/SBIC are NOT asserted on this fixture (banner).
    // What IS asserted: feed Stata's PRINTED log likelihood through our formulas
    // and every other printed header scalar comes back. That pins the
    // transformations without pretending we can reproduce a degenerate
    // determinant.
    {
        const double Td = 22.0, K = 3.0, mm = 7.0, tp = K * mm;  // 21 estimated coefficients
        const double LL = 880.4857;  // Stata's printed value
        const double lndet = -2.0 * LL / Td - K * std::log(2.0 * kPi) - K;
        near(std::exp(lndet), 3.47e-39, 3e-3, 0.0,
             "var(formula): Stata's LL implies Stata's printed Det(Sigma_ml)");
        near(std::pow((Td + mm) / (Td - mm), 3) * std::exp(lndet), 2.50e-38, 3e-3, 0.0,
             "var(formula): that det gives Stata's printed FPE");
        near(-2.0 * LL / Td + 2.0 * tp / Td, -78.13506, 1e-7, 1e-5, "var(formula): AIC");
        near(-2.0 * LL / Td + 2.0 * std::log(std::log(Td)) * tp / Td, -77.88973, 1e-7, 1e-5,
             "var(formula): HQIC");
        near(-2.0 * LL / Td + std::log(Td) * tp / Td, -77.09361, 1e-7, 1e-5, "var(formula): SBIC");
    }

    // ---- irf create --------------------------------------------------------
    check(static_cast<int>(r.irf.size()) == 7 * 9,
          "irf: 63 cells (9 impulse-response pairs x 7 steps 0..6) — the .irf file's row count");
    check(r.sigma_pd, "irf: Sigma factors, so the orthogonalized columns exist");
    {
        const int Y_ = 0, X1 = 1, X2 = 2;
        // step 0 — irf is the identity, stdirf is exactly 0, oirf IS the Cholesky.
        near(cell(r, 0, Y_, Y_)->irf, 1.0, 1e-12, 0.0, "irf(0)_y<-y == 1");
        check(cell(r, 0, Y_, Y_)->stdirf == 0.0, "stdirf(0)_y<-y == 0 exactly");
        check(cell(r, 0, Y_, X2)->stdirf == 0.0, "stdirf(0)_y<-x2 == 0 exactly");
        near(cell(r, 0, Y_, Y_)->oirf, 0.1777987, 1e-6, 1e-7, "oirf(0)_y<-y");
        near(cell(r, 0, Y_, Y_)->coirf, 0.1777987, 1e-6, 1e-7, "coirf(0)_y<-y");
        // ... and that value is the y-equation RMSE computed with the ML divisor.
        near(cell(r, 0, Y_, Y_)->oirf, r.eqs[0].rmse * std::sqrt(15.0 / 22.0), 1e-9, 0.0,
             "oirf(0)_y<-y == RMSE_y * sqrt((T-m)/T) — the ML-divisor residual sd");
        // The stdoirf(0) calibration target: it depends ONLY on the Sigma channel.
        near(cell(r, 0, Y_, Y_)->stdoirf, 0.0268042, 1e-5, 1e-7, "stdoirf(0)_y<-y");
        near(cell(r, 0, Y_, Y_)->stdcoirf, 0.0268042, 1e-5, 1e-7, "stdcoirf(0)_y<-y");
        near(cell(r, 0, X2, Y_)->oirf, -1.15634275e-06, 1e-6, 0.0, "oirf(0)_x2<-y");
        near(cell(r, 0, X2, X2)->oirf, 5.21083252e-06, 1e-5, 0.0, "oirf(0)_x2<-x2");
        near(cell(r, 0, X2, Y_)->stdoirf, 1.12454728e-06, 1e-5, 0.0, "stdoirf(0)_x2<-y");
        near(cell(r, 0, X2, X2)->stdoirf, 7.8556256e-07, 1e-5, 0.0, "stdoirf(0)_x2<-x2");
        check(cell(r, 0, Y_, Y_)->fevd == 0.0 && cell(r, 0, Y_, Y_)->mse == 0.0,
              "fevd(0) and mse(0) are 0");

        // step 1 — irf/stdirf are the lag-1 coefficients and their SEs.
        near(cell(r, 1, Y_, Y_)->irf, 0.334412695, 1e-6, 0.0, "irf(1)_y<-y");
        near(cell(r, 1, Y_, X1)->irf, 54.8085195, 1e-6, 0.0, "irf(1)_y<-x1");
        near(cell(r, 1, Y_, X2)->irf, 136.96005, 1e-6, 0.0, "irf(1)_y<-x2");
        near(cell(r, 1, Y_, Y_)->stdirf, 0.204034636, 1e-5, 0.0, "stdirf(1)_y<-y");
        near(cell(r, 1, Y_, X1)->stdirf, 15.444701, 1e-5, 0.0, "stdirf(1)_y<-x1");
        near(cell(r, 1, Y_, X2)->stdirf, 61.6455132, 1e-5, 0.0, "stdirf(1)_y<-x2");
        near(cell(r, 1, Y_, Y_)->cirf, 1.33441269, 1e-6, 0.0, "cirf(1)_y<-y");
        near(cell(r, 1, Y_, Y_)->oirf, 0.059299778, 1e-6, 0.0, "oirf(1)_y<-y");
        near(cell(r, 1, Y_, Y_)->coirf, 0.237098503, 1e-6, 0.0, "coirf(1)_y<-y");
        near(cell(r, 1, Y_, Y_)->stdoirf, 0.0373917545, 1e-5, 0.0, "stdoirf(1)_y<-y");
        near(cell(r, 1, Y_, Y_)->stdcoirf, 0.0509495164, 1e-5, 0.0, "stdcoirf(1)_y<-y");
        near(cell(r, 1, Y_, Y_)->fevd, 1.0, 1e-9, 0.0,
             "fevd(1)_y<-y == 1 (the Cholesky order makes y's own 1-step error entirely its own)");
        near(cell(r, 1, Y_, Y_)->mse, 0.177798725, 1e-6, 0.0, "mse(1)_y");
        near(cell(r, 1, X2, X2)->irf, 1.919842, 1e-6, 0.0, "irf(1)_x2<-x2");
        near(cell(r, 1, X2, X1)->irf, 0.0246277542, 1e-6, 0.0, "irf(1)_x2<-x1");
        near(cell(r, 1, X2, X2)->stdirf, 0.00185062473, 1e-5, 0.0, "stdirf(1)_x2<-x2");
        near(cell(r, 1, X2, X1)->stdirf, 0.000463656543, 1e-5, 0.0, "stdirf(1)_x2<-x1");
        near(cell(r, 1, X2, Y_)->fevd, 0.04693340933, 1e-6, 0.0, "fevd(1)_x2<-y");
        near(cell(r, 1, X2, X2)->fevd, 0.9530664225, 1e-6, 0.0, "fevd(1)_x2<-x2");
        near(cell(r, 1, X2, Y_)->stdfevd, 0.08804059171, 1e-5, 0.0, "stdfevd(1)_x2<-y");
        near(cell(r, 1, X2, X2)->stdfevd, 0.0880407339, 1e-5, 0.0, "stdfevd(1)_x2<-x2");
        near(cell(r, 1, X2, X2)->mse, 5.337593929e-06, 1e-6, 0.0, "mse(1)_x2");

        // step 2 — the mse recursion Stata's own value was checked against.
        near(cell(r, 2, Y_, Y_)->irf, -0.161560079, 1e-6, 0.0, "irf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->cirf, 1.17285262, 1e-6, 0.0, "cirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->stdirf, 0.200200581, 1e-5, 0.0, "stdirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->stdcirf, 0.336533514, 1e-5, 0.0, "stdcirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->oirf, -0.0288799796, 1e-6, 0.0, "oirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->coirf, 0.208218524, 1e-6, 0.0, "coirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->stdoirf, 0.0358937277, 1e-5, 0.0, "stdoirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->stdcoirf, 0.0676248848, 1e-5, 0.0, "stdcoirf(2)_y<-y");
        near(cell(r, 2, Y_, Y_)->mse, 0.187428279, 1e-6, 0.0, "mse(2)_y");
        near(cell(r, 2, Y_, Y_)->fevd, 0.999985501, 1e-6, 0.0, "fevd(2)_y<-y");
        near(cell(r, 2, Y_, X2)->fevd, 1.44987916e-05, 1e-4, 0.0, "fevd(2)_y<-x2");
        near(cell(r, 2, Y_, Y_)->stdfevd, 1.520656878e-05, 1e-5, 0.0, "stdfevd(2)_y<-y");
        near(cell(r, 2, X2, X2)->irf, 2.74645983, 1e-6, 0.0, "irf(2)_x2<-x2");
        near(cell(r, 2, X2, X2)->cirf, 5.66630183, 1e-6, 0.0, "cirf(2)_x2<-x2");
        near(cell(r, 2, X2, X2)->fevd, 0.9426626619, 1e-6, 0.0, "fevd(2)_x2<-x2");
        near(cell(r, 2, X2, X2)->mse, 1.161770985e-05, 1e-6, 0.0, "mse(2)_x2");

        // later steps — the companion recursion keeps tracking.
        near(cell(r, 3, Y_, X1)->irf, 105.814117, 1e-6, 0.0, "irf(3)_y<-x1");
        near(cell(r, 3, Y_, X1)->stdirf, 23.6108928, 1e-5, 0.0, "stdirf(3)_y<-x1");
        near(cell(r, 3, Y_, Y_)->stdfevd, 2.704796998e-05, 1e-5, 0.0, "stdfevd(3)_y<-y");
        near(cell(r, 6, Y_, Y_)->irf, 0.0137057902, 1e-5, 0.0, "irf(6)_y<-y");
        near(cell(r, 6, Y_, Y_)->cirf, 1.07820891, 1e-6, 0.0, "cirf(6)_y<-y");
        near(cell(r, 6, Y_, Y_)->fevd, 0.99996715, 1e-6, 0.0, "fevd(6)_y<-y");
        near(cell(r, 6, Y_, Y_)->mse, 0.191506218, 1e-6, 0.0, "mse(6)_y");
        near(cell(r, 6, Y_, X2)->irf, -102.632099, 1e-6, 0.0, "irf(6)_y<-x2");

        // mse does not vary with impulse — it is a property of the response.
        for (int s = 0; s <= 6; ++s)
            for (int b = 1; b < 3; ++b)
                check(cell(r, s, Y_, b)->mse == cell(r, s, Y_, 0)->mse,
                      "irf: mse is constant across impulses for a given response/step");
    }

    // ---- seeded-RED: the Sigma-channel divisor is PINNED, not free ---------
    // stdoirf(0)_y<-y = P_11 / sqrt(2 * divisor). With T - m = 15 instead of
    // T = 22 the same pipeline gives .0324..., against the oracle's .0268042 —
    // a 21% gap, far outside the SE tolerance. So the divisor above is a
    // measured choice, not an unconstrained one.
    {
        const double P11 = cell(r, 0, 0, 0)->oirf;
        const double alt = P11 / std::sqrt(2.0 * (22.0 - 7.0));
        check(std::fabs(alt - 0.0268042) > 1e-5 * 0.0268042,
              "irf: the T-m divisor for the Sigma channel is DISTINGUISHABLE from T at the SE "
              "tolerance (so the calibration is pinned)");
        near(P11 / std::sqrt(2.0 * 22.0), 0.0268042, 1e-5, 1e-7,
             "irf: stdoirf(0)_y<-y == P_11 / sqrt(2T) — the closed form the divisor was read off");
    }

    // ---- the degeneracy guard, reproduced on synthetic data ----------------
    // The P7 fixture's x1 equation collapses because of the data; here it
    // collapses by construction. Equation b is a deterministic linear function
    // of the very regressors the VAR uses (L1.a, L1.b, _cons), so its residual is
    // zero to within rounding while the design X stays full rank. The guard must
    // flag it, and every Wald statistic in that equation must come back missing
    // with df 0 — while the REST of the system keeps producing numbers.
    //
    // MEASURED, and worth stating: this does NOT make the Cholesky refuse.
    // Sigma = E'E/T is positive semi-definite by construction, so a numerically
    // exact fit leaves a tiny POSITIVE pivot (~1e-32 here), not a zero or
    // negative one — Stata hits the same thing on the P7 fixture, where its own
    // oirf(0)_x1<-x1 is 6.35e-14 rather than 0. The `!sigma_pd` branch in the
    // estimator is therefore a defensive guard that data cannot reliably reach,
    // and it is deliberately not asserted here rather than asserted against a
    // rounding accident.
    {
        const int64_t nn = 60;
        std::vector<double> Y2(static_cast<size_t>(nn) * 2), t2(static_cast<size_t>(nn));
        uint64_t st2 = 991ULL;
        double a = 0.0, b = 0.0;
        for (int64_t i = 0; i < nn; ++i) {
            t2[static_cast<size_t>(i)] = static_cast<double>(i + 1);
            st2 = st2 * 6364136223846793005ULL + 1442695040888963407ULL;
            const double e = (static_cast<double>(st2 >> 11) * (1.0 / 9007199254740992.0) - 0.5);
            const double na = 0.10 + 0.50 * a + 0.20 * b + e;
            const double nb = 1.00 + 0.30 * a - 0.40 * b;  // EXACT — no innovation
            a = na;
            b = nb;
            Y2[static_cast<size_t>(i)] = a;
            Y2[static_cast<size_t>(nn) + static_cast<size_t>(i)] = b;
        }
        const std::vector<std::string> nm2 = {"a", "b"};
        VarResult d = var_fit(Y2.data(), t2.data(), nn, nm2, 1, 3, 1.0);
        check(d.ok, "degenerate: the fit itself still succeeds (X is full rank)");
        if (d.ok) {
            check(!d.eqs[0].degenerate, "degenerate: equation a is live");
            check(d.eqs[1].degenerate, "degenerate: equation b is flagged degenerate");
            check(!d.eqs[1].chi2_ok && d.eqs[1].chi2_df == 0,
                  "degenerate: equation b reports a missing chi2 with df 0");
            check(std::isnan(d.eqs[1].chi2), "degenerate: equation b's chi2 is NaN, not a number");
            check(d.eqs[0].chi2_ok && d.eqs[0].chi2_df == 2,
                  "degenerate: equation a still reports its chi2 — one collapsed equation does not "
                  "take the system down with it");
            check(!d.irf.empty(), "degenerate: the IRF block is still produced");
            int missing = 0;
            for (const VarGrangerRow& g : d.granger) {
                if (g.equation != "b") continue;
                ++missing;
                check(!g.ok && g.df == 0 && std::isnan(g.chi2),
                      "degenerate: equation b's vargranger rows are missing with df 0");
            }
            check(missing == 2, "degenerate: equation b contributes 2 vargranger rows (a, ALL)");
            for (const VarGrangerRow& g : d.granger)
                if (g.equation == "a") check(g.ok && g.df > 0, "degenerate: equation a's rows survive");
        }
    }

    // ---- tsset gaps really are gaps ----------------------------------------
    // Deleting t = 12 must cost FOUR sample rows (t = 12, 13, 14 lose a lag, and
    // t = 12 itself is gone), not one — i.e. the lag lookup is by time value.
    {
        std::vector<double> Yg, tg;
        for (int64_t i = 0; i < n; ++i)
            if (kFixture[i][0] != 12.0) tg.push_back(kFixture[i][0]);
        const int64_t ng = static_cast<int64_t>(tg.size());
        Yg.resize(static_cast<size_t>(ng) * 3);
        int64_t q = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (kFixture[i][0] == 12.0) continue;
            for (int c = 0; c < 3; ++c)
                Yg[static_cast<size_t>(c) * static_cast<size_t>(ng) + static_cast<size_t>(q)] =
                    kFixture[i][c + 1];
            ++q;
        }
        VarResult g = var_fit(Yg.data(), tg.data(), ng, vars, 2, -1, 1.0);
        check(g.ok, "gap: var_fit still succeeds");
        check(g.n_obs == 19,
              "gap: dropping t=12 costs 3 sample rows (t=12,13,14), leaving 19 — got " +
                  std::to_string((long long)g.n_obs));
        check(g.irf.empty(), "gap: --irf-steps -1 skips the IRF block entirely");
    }

    // ---- the embedded fixture still matches the .dta it came from ----------
    // Runs only where the (gitignored, generated) wave artifact is present. It
    // closes the one hole embedding opens: a regenerated wave_fixture.dta that
    // silently no longer matches the numbers pinned above.
    {
        const cobra_test::ValidateBase base;
        std::string dta;
        if (const char* env = std::getenv("COBRA_WAVE_FIXTURE")) dta = env;
        if (dta.empty()) {
            if (const char* env = std::getenv("COBRA_PANGRAM_ARTIFACTS"))
                dta = std::string(env) + "/wave_fixture.dta";
        }
        if (dta.empty()) dta = base.str() + "../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
        std::ifstream probe(dta.c_str(), std::ios::binary);
        if (!probe.good()) {
            std::printf("  [note] wave_fixture.dta absent (%s) — fixture cross-check SKIPPED;\n"
                        "         every assertion above ran against the embedded literals. Set\n"
                        "         COBRA_WAVE_FIXTURE or COBRA_PANGRAM_ARTIFACTS to run it.\n",
                        dta.c_str());
        } else {
            probe.close();
            DtaResult dr = read_dta(dta);
            const Frame& fr = dr.frame;
            const char* nmv[4] = {"t", "y", "x1", "x2"};
            const auto& idc = fr.col("id").f64;
            int64_t row = 0, bad = 0, seen = 0;
            for (int64_t i = 0; i < fr.n_rows(); ++i) {
                if (idc[static_cast<size_t>(i)] != 1.0) continue;
                ++seen;
                if (row >= 24) continue;
                for (int c = 0; c < 4; ++c)
                    if (fr.col(nmv[c]).f64[static_cast<size_t>(i)] != kFixture[row][c]) ++bad;
                ++row;
            }
            check(seen == 24, "cross-check: id==1 has 24 rows in wave_fixture.dta — got " +
                                  std::to_string((long long)seen));
            check(bad == 0, "cross-check: the embedded fixture is bit-identical to the .dta (" +
                                std::to_string((long long)bad) + " mismatches)");
            std::printf("  cross-check: embedded fixture == wave_fixture.dta (id==1)\n");
        }
    }

    if (fail_count == 0) std::printf("  all varmodel assertions passed\n");
    return fail_count;
}
