// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — survival oracle assertions (StataBE 19.5,
// internal capture p7_stata.log, the
// `preserve / sort id exit / stset exit, failure(fail) id(id)` block of p7.do).
//
// FIXTURE. tests/validate/survival_p7.csv is a verbatim five-column extract
// (id, exit, fail, x1, x2 — 4,800 rows, 129 missing x2) of the wave fixture
// internal fixture wave_fixture.dta, written with
// Python's shortest-round-trip float repr so every cell is the SAME double the
// .dta holds. The wave artifact itself is a generated oracle input and is
// gitignored, so the extract is what makes these assertions run everywhere; when
// the .dta IS present (COBRA_WAVE_FIXTURE, or the pangram tree next to
// tests/validate) the last leg re-reads it and asserts the extract matches it
// cell for cell, so the two can never drift apart silently.
//
// Tolerances follow the wave rule — coefficients rtol 1e-6, standard errors
// rtol 1e-5, log-likelihood rtol 1e-6 — widened, per assertion, to half of the
// last digit Stata actually PRINTED, because that rounding (not our arithmetic)
// is the binding uncertainty on a 6-or-7-significant-digit oracle value.
//
// Stata's iteration counts are deliberately NOT asserted: our Newton path is our
// own, only the converged numbers are pinned.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "estimators/posthest.h"
#include "estimators/survival.h"
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
// for ".0550338", 1e-4 for "-830.6836", ...). A value passes when it is within
// the wave's relative tolerance OR within half a printed digit — whichever is
// looser — so an assertion never fails on digits the oracle never showed.
static void near(double c, double o, double rtol, double unit, const std::string& what) {
    const double tol = std::max(rtol * std::fabs(o), 0.5 * unit);
    if (!(std::fabs(c - o) <= tol))
        std::printf("  FAIL: %s — got %.10g, oracle %.10g (|d| %.3g > tol %.3g)\n", what.c_str(), c,
                    o, std::fabs(c - o), tol);
    if (!(std::fabs(c - o) <= tol)) fail_count = 1;
}

namespace {

struct Fixture {
    std::vector<double> id, exit_t, fail, x1, x2;
    int64_t n = 0;
};

double parse_cell(const std::string& s) {
    if (s.empty()) return std::nan("");
    return std::stod(s);
}

Fixture read_csv(const std::string& path) {
    Fixture f;
    std::ifstream in(path);
    if (!in.good()) {
        std::printf("  FAIL: cannot open the survival fixture %s\n", path.c_str());
        fail_count = 1;
        return f;
    }
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string a, b, c, d, e;
        std::getline(ss, a, ',');
        std::getline(ss, b, ',');
        std::getline(ss, c, ',');
        std::getline(ss, d, ',');
        std::getline(ss, e, ',');
        f.id.push_back(parse_cell(a));
        f.exit_t.push_back(parse_cell(b));
        f.fail.push_back(parse_cell(c));
        f.x1.push_back(parse_cell(d));
        f.x2.push_back(parse_cell(e));
    }
    f.n = static_cast<int64_t>(f.id.size());
    return f;
}

// Column-major design assembled from the columns the caller lists.
std::vector<double> design(const std::vector<const std::vector<double>*>& cols,
                           const std::vector<int64_t>& sel, bool add_cons) {
    const int64_t n = static_cast<int64_t>(sel.size());
    const int k = static_cast<int>(cols.size()) + (add_cons ? 1 : 0);
    std::vector<double> X(static_cast<size_t>(n) * static_cast<size_t>(k), 1.0);
    for (size_t c = 0; c < cols.size(); ++c)
        for (int64_t i = 0; i < n; ++i)
            X[static_cast<int64_t>(c) * n + i] = (*cols[c])[static_cast<size_t>(sel[static_cast<size_t>(i)])];
    return X;
}

double quad_form(const std::vector<double>& b, const std::vector<double>& V, int np,
                 const std::vector<int>& idx) {
    const int j = static_cast<int>(idx.size());
    std::vector<double> S(static_cast<size_t>(j) * static_cast<size_t>(j));
    for (int r = 0; r < j; ++r)
        for (int c = 0; c < j; ++c)
            S[static_cast<size_t>(r) * static_cast<size_t>(j) + static_cast<size_t>(c)] =
                V[static_cast<size_t>(idx[static_cast<size_t>(r)]) * static_cast<size_t>(np) +
                  static_cast<size_t>(idx[static_cast<size_t>(c)])];
    if (la::spd_inverse(S.data(), j) != 0) return -1;
    double q = 0;
    for (int r = 0; r < j; ++r)
        for (int c = 0; c < j; ++c)
            q += b[static_cast<size_t>(idx[static_cast<size_t>(r)])] *
                 S[static_cast<size_t>(r) * static_cast<size_t>(j) + static_cast<size_t>(c)] *
                 b[static_cast<size_t>(idx[static_cast<size_t>(c)])];
    return q;
}

}  // namespace

int test_survival_main() {
    std::printf("test_survival:\n");
    const cobra_test::ValidateBase base;
    Fixture f = read_csv(base + "survival_p7.csv");
    if (f.n == 0) return 1;
    check(f.n == 4800, "fixture has 4,800 rows");

    // ---- stset -------------------------------------------------------------
    StsetResult ss = stset_build(f.id.data(), f.exit_t.data(), f.fail.data(), f.n);
    check(ss.ok, "stset_build succeeds");
    check(ss.n_total == 4800, "stset: 4,800 total observations");
    check(ss.n_excl_after_failure == 3593,
          "stset: 3,593 obs. begin on or after (first) failure — got " + std::to_string(ss.n_excl_after_failure));
    check(ss.n_excl_missing == 0 && ss.n_excl_negative == 0 && ss.n_excl_zero_length == 0,
          "stset: no missing / negative / zero-length exclusions on this fixture");
    check(ss.n_kept == 1207, "stset: 1,207 observations remaining — got " + std::to_string(ss.n_kept));
    check(ss.n_subjects == 200, "stset: 200 subjects");
    check(ss.n_failures == 200, "stset: 200 failures");
    check(ss.single_failure, "stset: single-failure-per-subject data");
    near(ss.time_at_risk, 270.392, 1e-6, 1e-3, "stset: total analysis time at risk");
    near(ss.origin, 0.0, 1e-12, 1e-9, "stset: at risk from t = 0");
    near(ss.first_entry, 0.0, 1e-12, 1e-9, "stset: earliest observed entry t = 0");
    near(ss.last_exit, 1.656333, 1e-6, 1e-6, "stset: last observed exit t");
    check(ss.n_excl_missing + ss.n_excl_after_failure + ss.n_excl_negative + ss.n_excl_zero_length +
                  ss.n_kept == ss.n_total,
          "stset: the exclusion counters and the kept count partition the input");

    // The P7 fixture triggers exactly ONE of the four exclusion rules, so the
    // other three are exercised on a hand-built six-record frame instead — a
    // counter that can only ever read 0 is not a counter.
    {
        //  id  time  fail   what stset should do with it
        //   1   1.0    0     kept, interval (0, 1]
        //   1   1.0    0     ZERO-LENGTH: (1, 1]
        //   1   2.0    1     kept, interval (1, 2], the subject's first failure
        //   1   3.0    0     AFTER FAILURE
        //   2   2.0    0     arrives FIRST in row order but sorts second
        //   2   1.0    0     ... so subject 2 is walked 1.0 then 2.0: (0,1] and (1,2]
        //   3   NaN    0     MISSING time
        const double sid[7] = {1, 1, 1, 1, 2, 2, 3};
        const double stm[7] = {1.0, 1.0, 2.0, 3.0, 2.0, 1.0, std::nan("")};
        const double sfl[7] = {0, 0, 1, 0, 0, 0, 0};
        StsetResult s2 = stset_build(sid, stm, sfl, 7);
        check(s2.ok, "stset(synthetic): builds");
        check(s2.n_excl_zero_length == 1,
              "stset(synthetic): 1 zero-length interval — got " + std::to_string(s2.n_excl_zero_length));
        check(s2.n_excl_after_failure == 1,
              "stset(synthetic): 1 obs. after the first failure — got " +
                  std::to_string(s2.n_excl_after_failure));
        check(s2.n_excl_missing == 1,
              "stset(synthetic): 1 obs. with a missing time — got " + std::to_string(s2.n_excl_missing));
        // Subject 2's records sort to 1.0 then 2.0, giving (0,1] and (1,2] — both
        // kept, no negative interval. Records are walked in TIME order regardless
        // of the row order they arrive in, which is what makes that true.
        check(s2.n_excl_negative == 0, "stset(synthetic): sorting by time leaves no negative interval");
        check(s2.n_kept == 4, "stset(synthetic): 4 records remain — got " + std::to_string(s2.n_kept));
        check(s2.n_subjects == 2, "stset(synthetic): 2 subjects (the missing-time subject drops out)");
        check(s2.n_failures == 1, "stset(synthetic): 1 failure");
        near(s2.time_at_risk, 4.0, 1e-12, 1e-9, "stset(synthetic): time at risk (1 + 1) + (1 + 1)");
        check(s2.st[1] == 0.0 && s2.st[3] == 0.0 && s2.st[6] == 0.0,
              "stset(synthetic): excluded rows PERSIST with _st = 0 (never deleted)");
        check(s2.t0[2] == 1.0 && s2.t[2] == 2.0 && s2.d[2] == 1.0,
              "stset(synthetic): the failure record carries (_t0, _t, _d) = (1, 2, 1)");
        // A negative interval needs a time that runs backwards past the origin.
        const double nid[2] = {1, 1};
        const double ntm[2] = {-1.0, 2.0};
        const double nfl[2] = {0, 1};
        StsetResult s3 = stset_build(nid, ntm, nfl, 2);
        check(s3.n_excl_negative == 1,
              "stset(synthetic): 1 negative interval (0, -1] — got " + std::to_string(s3.n_excl_negative));
    }

    // ---- estimation sample: _st == 1, then listwise on x1 x2 ---------------
    std::vector<int64_t> sel;
    for (int64_t i = 0; i < f.n; ++i)
        if (ss.st[static_cast<size_t>(i)] == 1.0 && !std::isnan(f.x1[static_cast<size_t>(i)]) &&
            !std::isnan(f.x2[static_cast<size_t>(i)]))
            sel.push_back(i);
    const int64_t n = static_cast<int64_t>(sel.size());
    check(n == 1173, "stcox/streg sample: 1,173 obs — got " + std::to_string((long long)n));

    std::vector<double> t0(static_cast<size_t>(n)), tt(static_cast<size_t>(n)), dd(static_cast<size_t>(n));
    std::vector<int32_t> cl(static_cast<size_t>(n));
    std::map<double, int32_t> id2code;
    double risk = 0;
    int64_t nfail = 0;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t r = sel[static_cast<size_t>(i)];
        t0[static_cast<size_t>(i)] = ss.t0[static_cast<size_t>(r)];
        tt[static_cast<size_t>(i)] = ss.t[static_cast<size_t>(r)];
        dd[static_cast<size_t>(i)] = ss.d[static_cast<size_t>(r)];
        risk += tt[static_cast<size_t>(i)] - t0[static_cast<size_t>(i)];
        if (dd[static_cast<size_t>(i)] != 0.0) ++nfail;
        const double idv = f.id[static_cast<size_t>(r)];
        auto it = id2code.find(idv);
        if (it == id2code.end()) it = id2code.emplace(idv, static_cast<int32_t>(id2code.size())).first;
        cl[static_cast<size_t>(i)] = it->second;
    }
    const int G = static_cast<int>(id2code.size());
    check(G == 199, "stcox/streg sample: 199 subjects — got " + std::to_string(G));
    check(nfail == 194, "stcox/streg sample: 194 failures — got " + std::to_string((long long)nfail));
    near(risk, 263.5, 1e-9, 1e-1, "stcox/streg sample: time at risk 263.5");

    const std::vector<double> Xcox = design({&f.x1, &f.x2}, sel, false);
    const std::vector<double> Xreg = design({&f.x1, &f.x2}, sel, true);

    // ---- 1. MANDATORY: Weibull gradient self-check -------------------------
    // Analytic gradient vs central-difference numerical gradient at a
    // deliberately NON-optimal theta (where every term is far from zero, so an
    // agreement is informative rather than an artifact of being at the optimum).
    {
        const std::vector<double> theta = {0.3, -0.2, -3.0, 2.0};  // (b_x1, b_x2, b_cons, ln p)
        const int k = 3, np = 4;
        std::vector<double> g(static_cast<size_t>(np));
        weib::ll_grad(Xreg.data(), n, k, t0.data(), tt.data(), dd.data(), theta, &g);
        for (int j = 0; j < np; ++j) {
            const double h = 1e-6;
            std::vector<double> tp = theta, tm = theta;
            tp[static_cast<size_t>(j)] += h;
            tm[static_cast<size_t>(j)] -= h;
            const double lp = weib::ll_grad(Xreg.data(), n, k, t0.data(), tt.data(), dd.data(), tp, nullptr);
            const double lm = weib::ll_grad(Xreg.data(), n, k, t0.data(), tt.data(), dd.data(), tm, nullptr);
            const double num = (lp - lm) / (2 * h);
            const double rel = std::fabs(g[static_cast<size_t>(j)] - num) / (std::fabs(num) + 1.0);
            check(rel < 1e-6, "weibull gradient component " + std::to_string(j) +
                                  ": analytic vs central difference (rel " + std::to_string(rel) + ")");
        }
    }

    // ---- 2. stcox ----------------------------------------------------------
    CoxResult cx = cox_fit(Xcox.data(), n, 2, t0.data(), tt.data(), dd.data());
    check(cx.ok, "cox_fit succeeds");
    check(cx.converged, "cox_fit converges");
    check(cx.n_fail == 194, "cox_fit sees 194 failures");
    // "Cox regression with no ties" is a CLAIM about the data, so it is asserted
    // from a real tie scan, not assumed: this fixture has no tied failure times,
    // which is exactly why Breslow reproduces Stata's default here.
    check(cx.n_tied_failures == 0, "cox_fit: no tied failure times (Stata's 'no ties' header)");
    near(cx.ll, -830.6836, 1e-6, 1e-4, "stcox: log pseudolikelihood");
    near(cx.ll_null, -830.99847, 1e-6, 1e-5, "stcox: LL at beta = 0 (Stata's iteration 0)");
    near(std::exp(cx.beta[0]), 0.9390707, 1e-6, 1e-7, "stcox: x1 hazard ratio");
    near(std::exp(cx.beta[1]), 0.9900633, 1e-6, 1e-7, "stcox: x2 hazard ratio");

    std::vector<double> S;
    cox_score_residuals(Xcox.data(), n, 2, t0.data(), tt.data(), dd.data(), cx.beta, &S);
    // q_c = G/(G-1) is VERIFIED here, not assumed. The seeded-RED leg below shows
    // the assertion is sharp: with q_c = 1 the same pipeline gives .05489539, and
    // the oracle prints .0550338 — a 0.25% gap, far outside the SE tolerance.
    const double qc = static_cast<double>(G) / static_cast<double>(G - 1);
    std::vector<double> Vc = survival_sandwich(S, n, 2, cx.vcov, cl.data(), G, qc);
    check(!Vc.empty(), "stcox: cluster sandwich builds");
    const double se_b1 = std::sqrt(Vc[0]), se_b2 = std::sqrt(Vc[3]);
    near(std::exp(cx.beta[0]) * se_b1, 0.0550338, 1e-5, 1e-7, "stcox: x1 Haz. ratio std. err.");
    near(std::exp(cx.beta[1]) * se_b2, 0.1019729, 1e-5, 1e-7, "stcox: x2 Haz. ratio std. err.");
    near(cx.beta[0] / se_b1, -1.07, 1e-2, 1e-2, "stcox: x1 z (raw-b metric, NOT the HR metric)");
    near(cx.beta[1] / se_b2, -0.10, 1e-2, 1e-2, "stcox: x2 z");
    // CI is exp(b +/- z* se_b), not HR +/- z* se_HR.
    const double z95 = 1.959963984540054;
    near(std::exp(cx.beta[0] - z95 * se_b1), 0.8371707, 1e-5, 1e-7, "stcox: x1 95% CI lower");
    near(std::exp(cx.beta[0] + z95 * se_b1), 1.053374, 1e-5, 1e-6, "stcox: x1 95% CI upper");
    near(std::exp(cx.beta[1] - z95 * se_b2), 0.8090815, 1e-5, 1e-7, "stcox: x2 95% CI lower");
    near(std::exp(cx.beta[1] + z95 * se_b2), 1.211528, 1e-5, 1e-6, "stcox: x2 95% CI upper");
    {
        const double w = quad_form(cx.beta, Vc, 2, {0, 1});
        near(w, 1.16, 1e-2, 1e-2, "stcox: Wald chi2(2)");
        near(chi2_pvalue(w, 2.0), 0.5599, 1e-4, 1e-4, "stcox: Prob > chi2");
    }
    {  // seeded-RED: the q_c = 1 arm must NOT reproduce the oracle SE
        std::vector<double> V1 = survival_sandwich(S, n, 2, cx.vcov, cl.data(), G, 1.0);
        const double se_HR = std::exp(cx.beta[0]) * std::sqrt(V1[0]);
        check(std::fabs(se_HR - 0.0550338) > 1e-5 * 0.0550338,
              "stcox: q_c = 1 is DISTINGUISHABLE from G/(G-1) at the SE tolerance "
              "(so the factor above is pinned, not free)");
    }

    // ---- 3. streg, distribution(weibull) -----------------------------------
    WeibullResult wr = weibull_fit(Xreg.data(), n, 3, t0.data(), tt.data(), dd.data(), 2);
    check(wr.ok, "weibull_fit succeeds");
    check(wr.converged, "weibull_fit converges");
    // The POSITIVE log likelihood is the delayed-entry correction's signature:
    // p ~ 10.5 on t in (0, 1.66] makes the densities much greater than 1. Drop
    // the t0^p term and this lands nowhere near +156.73414.
    near(wr.ll, 156.73414, 1e-6, 1e-5, "streg: log pseudolikelihood (full model)");
    near(wr.ll_null, 156.38123, 1e-6, 1e-5, "streg: constant-only prefit log pseudolikelihood");
    near(std::exp(wr.beta[0]), 0.9399031, 1e-6, 1e-7, "streg: x1 hazard ratio");
    near(std::exp(wr.beta[1]), 0.9729846, 1e-6, 1e-7, "streg: x2 hazard ratio");
    near(std::exp(wr.beta[2]), 0.0264001, 1e-6, 1e-7, "streg: _cons (baseline hazard)");
    near(wr.ln_p, 2.352792, 1e-6, 1e-6, "streg: /ln_p");
    near(wr.p, 10.51488, 1e-6, 1e-5, "streg: p");
    near(wr.inv_p, 0.0951033, 1e-6, 1e-7, "streg: 1/p");

    {
        std::vector<double> theta = wr.beta;
        theta.push_back(wr.ln_p);
        std::vector<double> Sw;
        weib::ll_grad(Xreg.data(), n, 3, t0.data(), tt.data(), dd.data(), theta, nullptr, &Sw);
        // Stata's vce(robust) on stset id() data clusters on the SUBJECT — 199
        // independent subjects, not 1,173 independent records. VERIFIED here, and
        // the seeded-RED leg below shows the record-level alternative misses.
        std::vector<double> Vw = survival_sandwich(Sw, n, 4, wr.vcov, cl.data(), G, qc);
        check(!Vw.empty(), "streg: subject-cluster sandwich builds");
        auto sd = [&](int i) { return std::sqrt(Vw[static_cast<size_t>(i) * 4 + static_cast<size_t>(i)]); };
        near(std::exp(wr.beta[0]) * sd(0), 0.0596222, 1e-5, 1e-7, "streg: x1 Haz. ratio std. err.");
        near(std::exp(wr.beta[1]) * sd(1), 0.1013885, 1e-5, 1e-7, "streg: x2 Haz. ratio std. err.");
        near(std::exp(wr.beta[2]) * sd(2), 0.0050684, 1e-5, 1e-7, "streg: _cons std. err.");
        near(sd(3), 0.0423804, 1e-5, 1e-7, "streg: /ln_p std. err.");
        near(wr.p * sd(3), 0.4456248, 1e-5, 1e-7, "streg: p std. err. (delta method)");
        near(sd(3) / wr.p, 0.0040305, 1e-5, 1e-7, "streg: 1/p std. err. (delta method)");
        near(wr.ln_p / sd(3), 55.52, 1e-3, 1e-2, "streg: /ln_p z");
        near(std::exp(wr.ln_p - z95 * sd(3)), 9.676766, 1e-5, 1e-6, "streg: p 95% CI lower");
        near(std::exp(wr.ln_p + z95 * sd(3)), 11.42559, 1e-5, 1e-5, "streg: p 95% CI upper");
        near(std::exp(-(wr.ln_p + z95 * sd(3))), 0.0875228, 1e-5, 1e-7, "streg: 1/p 95% CI lower");
        near(std::exp(-(wr.ln_p * 1.0 - z95 * sd(3))), 0.1033403, 1e-5, 1e-7, "streg: 1/p 95% CI upper");
        // Stata's header Wald covers the SLOPES only — _cons and /ln_p are out.
        std::vector<double> th = theta;
        const double w = quad_form(th, Vw, 4, {0, 1});
        near(w, 1.02, 1e-2, 1e-2, "streg: Wald chi2(2)");
        near(chi2_pvalue(w, 2.0), 0.5992, 1e-4, 1e-4, "streg: Prob > chi2");

        // seeded-RED: the RECORD-level (unclustered) sandwich must NOT reproduce
        // /ln_p's SE — this is what makes the subject-clustering rule load-bearing.
        std::vector<double> Vr = survival_sandwich(
            Sw, n, 4, wr.vcov, nullptr, 0, static_cast<double>(n) / static_cast<double>(n - 1));
        const double se_lnp_rec = std::sqrt(Vr[15]);
        check(std::fabs(se_lnp_rec - 0.0423804) > 1e-3 * 0.0423804,
              "streg: the record-level sandwich is DISTINGUISHABLE from the subject-clustered one "
              "(so the auto-cluster-on-id rule is pinned, not free)");
    }

    // ---- 4. the CSV extract still matches the .dta it came from -------------
    // Runs only where the (gitignored, generated) wave artifact is present. It
    // closes the one hole the extract opens: a regenerated wave_fixture.dta that
    // silently no longer matches the numbers pinned above.
    {
        std::string dta;
        if (const char* env = std::getenv("COBRA_WAVE_FIXTURE")) dta = env;
        if (dta.empty())
            dta = base.str() + "../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
        std::ifstream probe(dta.c_str(), std::ios::binary);
        if (!probe.good()) {
            std::printf("  [note] wave_fixture.dta absent (%s) — CSV/.dta cross-check skipped;\n"
                        "         every assertion above ran against tests/validate/survival_p7.csv.\n",
                        dta.c_str());
        } else {
            probe.close();
            DtaResult dr = read_dta(dta);
            const Frame& fr = dr.frame;
            check(fr.n_rows() == f.n, "cross-check: wave_fixture.dta row count matches the extract");
            const char* nm[5] = {"id", "exit", "fail", "x1", "x2"};
            const std::vector<double>* mine[5] = {&f.id, &f.exit_t, &f.fail, &f.x1, &f.x2};
            for (int c = 0; c < 5; ++c) {
                const auto& col = fr.col(nm[c]).f64;
                int64_t bad = 0;
                for (int64_t i = 0; i < f.n && i < fr.n_rows(); ++i) {
                    const double a = col[static_cast<size_t>(i)], b = (*mine[c])[static_cast<size_t>(i)];
                    if (std::isnan(a) && std::isnan(b)) continue;
                    if (a != b) ++bad;
                }
                check(bad == 0, std::string("cross-check: column ") + nm[c] +
                                    " is bit-identical in the CSV extract and the .dta (" +
                                    std::to_string((long long)bad) + " mismatches)");
            }
            std::printf("  cross-check: tests/validate/survival_p7.csv == wave_fixture.dta\n");
        }
    }

    if (fail_count == 0) std::printf("  all survival oracle assertions passed\n");
    return fail_count;
}
