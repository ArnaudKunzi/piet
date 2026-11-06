// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// See sdid.h for method sources and the list of
// conventions AAHIW21 leaves open that this file pins.
//
// SWEEP (fleet law: when several plausible formulations exist, MEASURE which one
// the oracle picks — BUG-164/BUG-175). Every axis below was implemented and run
// against the Stata 19.5 oracle on the 178 complete units of the pangram wave
// fixture (target ATT 1.33439); the selected value is marked [*].
//
//   donor pool        never-treated only [*]  1.334381   | not-yet-treated  0.663064
//   aggregation w_a   N_tr^a * T_post^a  [*]  1.334381   | T_post^a  1.336330
//                                                        | N_tr^a    1.327019
//   sigma sample      control x block-pre diffs [*]       | + treated units   1.332833
//                                                         | all periods       1.330257
//                                                         | one global window 1.333899
//   sigma denominator n-1 (sample sd)  [*]  1.3343806     | n (paper's literal
//                                                            N_co(T_pre-1))  1.3342476
//                                                         | uncentered, n-1  1.3344032
//   omega intercept   free [*]  1.334381                  | none  0.708497
//   lambda intercept  free [*]  1.334381                  | none  1.334379
//   lambda ridge      (1e-6 sigma)^2 N_co [*]             | exactly 0: identical to 1e-13
//   omega ridge scale zeta^2 T_pre [*]                    | x0.9  1.325187 | x1.1  1.342060
//
// The selected combination lands ATT = 1.3343806, i.e. 9.4e-6 under the oracle's
// printed 1.33439 — inside the wave's 1e-5 acceptance band but NOT to the last
// printed digit. That residual was chased and attributed: it is solver PATH, not
// formulation. Running the same formulation through a truncated Frank-Wolfe
// (10k iterations, R-style min-decrease stop) walks the estimate across the
// target — 1.3343845 at min_decrease=1e-5*sigma, 1.3343811 at 10k plain
// iterations, 1.3356990 at 100 — so the last 1e-5 is an artifact of where the
// reference implementation stops its own optimizer, which the paper does not fix
// and which we are not going to fit. We ship the CONVERGED minimizer: it is
// deterministic, reproducible, and does not depend on an arbitrary iteration cap.

#include "estimators/sdid.h"

#include "core/distributions.h"
#include "core/rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace cecon {

const char* sdid_diagnostic_name(SdidDiagnostic d) {
    switch (d) {
        case SdidDiagnostic::None: return "none";
        case SdidDiagnostic::IncompletePanel: return "incomplete_panel";
        case SdidDiagnostic::PlaceboInsufficientControls: return "placebo_insufficient_controls";
        case SdidDiagnostic::UnsupportedVce: return "unsupported_vce";
        case SdidDiagnostic::BadPanel: return "bad_panel";
    }
    return "none";
}

namespace {

// The (unit x period) rectangle plus each unit's adoption index.
struct Rect {
    int nu = 0, nt = 0;
    std::vector<double> Y;          // nu * nt, unit-major; NaN marks a hole
    std::vector<double> units;      // nu unit ids, ascending
    std::vector<double> times;      // nt period values, ascending
    std::vector<int> adopt;         // per unit: index of first treated period, -1 = never treated
    inline double at(int u, int t) const { return Y[static_cast<size_t>(u) * nt + t]; }
};

// argmin over the simplex of  ||A w - b||^2 + pen ||w||^2  subject to w >= 0,
// sum w = 1, with a FREE INTERCEPT on the fit (absorbed by centering A's columns
// and b over the m observations, which is exactly what a free additive constant
// does to a least-squares objective).
//
// AAHIW21 names Frank-Wolfe as the scheme; this is its PAIRWISE variant (move
// mass from the worst active atom to the best atom, exact line search), which
// keeps FW's projection-free simplex feasibility while converging linearly on a
// strongly convex objective instead of FW's O(1/k). Measured on the oracle
// fixture: the duality gap is exhausted in ~250-350 steps per block, and the
// estimate is stable to 1e-12 across three further decades of tolerance.
std::vector<double> solve_simplex(std::vector<double> A, std::vector<double> b,
                                  int m, int n, double pen, int max_iter, double tol) {
    for (int j = 0; j < n; ++j) {
        double s = 0;
        for (int i = 0; i < m; ++i) s += A[static_cast<size_t>(i) * n + j];
        s /= m;
        for (int i = 0; i < m; ++i) A[static_cast<size_t>(i) * n + j] -= s;
    }
    {
        double s = 0;
        for (int i = 0; i < m; ++i) s += b[i];
        s /= m;
        for (int i = 0; i < m; ++i) b[i] -= s;
    }
    std::vector<double> Q(static_cast<size_t>(n) * n, 0.0), c(n, 0.0);
    for (int j = 0; j < n; ++j) {
        double cj = 0;
        for (int i = 0; i < m; ++i) cj += A[static_cast<size_t>(i) * n + j] * b[i];
        c[j] = cj;
        for (int k = j; k < n; ++k) {
            double s = 0;
            for (int i = 0; i < m; ++i)
                s += A[static_cast<size_t>(i) * n + j] * A[static_cast<size_t>(i) * n + k];
            Q[static_cast<size_t>(j) * n + k] = s;
            Q[static_cast<size_t>(k) * n + j] = s;
        }
    }
    std::vector<double> w(n, 1.0 / n), Qw(n, 0.0);
    for (int j = 0; j < n; ++j) {
        double s = 0;
        for (int k = 0; k < n; ++k) s += Q[static_cast<size_t>(j) * n + k] * w[k];
        Qw[j] = s;
    }
    for (int it = 0; it < max_iter; ++it) {
        int jbest = -1, kworst = -1;
        double gmin = 0, gmax = 0;
        for (int j = 0; j < n; ++j) {
            double g = 2.0 * (Qw[j] - c[j]) + 2.0 * pen * w[j];
            if (jbest < 0 || g < gmin) { gmin = g; jbest = j; }
            if (w[j] > 0.0 && (kworst < 0 || g > gmax)) { gmax = g; kworst = j; }
        }
        if (jbest < 0 || kworst < 0 || jbest == kworst) break;
        double gap = gmax - gmin;
        if (!(gap > tol)) break;
        double quad = Q[static_cast<size_t>(jbest) * n + jbest]
                    - 2.0 * Q[static_cast<size_t>(jbest) * n + kworst]
                    + Q[static_cast<size_t>(kworst) * n + kworst] + 2.0 * pen;
        double step = (quad > 0) ? gap / (2.0 * quad) : w[kworst];
        if (step > w[kworst]) step = w[kworst];
        if (!(step > 0)) break;
        w[jbest] += step;
        w[kworst] -= step;
        if (w[kworst] < 1e-300) w[kworst] = 0.0;
        for (int j = 0; j < n; ++j)
            Qw[j] += step * (Q[static_cast<size_t>(jbest) * n + j] - Q[static_cast<size_t>(kworst) * n + j]);
    }
    return w;
}

// One adoption-date block: this cohort's treated rows against the never-treated
// donor pool, with t_pre pre-periods. Returns the block ATT.
double block_tau(const Rect& R, const std::vector<int>& co, const std::vector<int>& tr,
                 int t_pre, const SdidConfig& cfg, double* zeta_out) {
    const int nco = static_cast<int>(co.size());
    const int ntr = static_cast<int>(tr.size());
    const int t_post = R.nt - t_pre;

    // sigma: sample sd (n-1) of the control units' first differences over the pre periods.
    double s1 = 0, s2 = 0;
    int64_t nd = 0;
    for (int i = 0; i < nco; ++i)
        for (int t = 1; t < t_pre; ++t) {
            double d = R.at(co[i], t) - R.at(co[i], t - 1);
            s1 += d; s2 += d * d; ++nd;
        }
    double sigma = 0;
    if (nd > 1) {
        double mean = s1 / static_cast<double>(nd);
        double ss = s2 - static_cast<double>(nd) * mean * mean;
        sigma = std::sqrt(std::max(ss, 0.0) / static_cast<double>(nd - 1));
    }
    const double zeta = std::pow(static_cast<double>(ntr) * t_post, 0.25) * sigma;
    if (zeta_out) *zeta_out = zeta;

    // omega: rows = pre periods, columns = control units; target = treated average.
    std::vector<double> A(static_cast<size_t>(t_pre) * nco), bvec(t_pre);
    for (int t = 0; t < t_pre; ++t) {
        for (int i = 0; i < nco; ++i) A[static_cast<size_t>(t) * nco + i] = R.at(co[i], t);
        double s = 0;
        for (int i = 0; i < ntr; ++i) s += R.at(tr[i], t);
        bvec[t] = s / ntr;
    }
    std::vector<double> omega =
        solve_simplex(A, bvec, t_pre, nco, zeta * zeta * t_pre, cfg.max_iter, cfg.tol);

    // lambda: rows = control units, columns = pre periods; target = each control
    // unit's post-period average.
    std::vector<double> A2(static_cast<size_t>(nco) * t_pre), b2(nco);
    for (int i = 0; i < nco; ++i) {
        for (int t = 0; t < t_pre; ++t) A2[static_cast<size_t>(i) * t_pre + t] = R.at(co[i], t);
        double s = 0;
        for (int t = t_pre; t < R.nt; ++t) s += R.at(co[i], t);
        b2[i] = s / t_post;
    }
    const double zl = 1e-6 * sigma;
    std::vector<double> lambda =
        solve_simplex(A2, b2, nco, t_pre, zl * zl * nco, cfg.max_iter, cfg.tol);

    // Weighted two-way DiD. For a block treatment the weighted TWFE fit collapses
    // to this difference of four weighted means (AAHIW21 eq. 2.2-2.3).
    double tr_post = 0;
    for (int i = 0; i < ntr; ++i)
        for (int t = t_pre; t < R.nt; ++t) tr_post += R.at(tr[i], t);
    tr_post /= static_cast<double>(ntr) * t_post;

    double tr_pre = 0;
    for (int t = 0; t < t_pre; ++t) {
        double s = 0;
        for (int i = 0; i < ntr; ++i) s += R.at(tr[i], t);
        tr_pre += lambda[t] * (s / ntr);
    }
    double co_post = 0, co_pre = 0;
    for (int i = 0; i < nco; ++i) {
        double sp = 0;
        for (int t = t_pre; t < R.nt; ++t) sp += R.at(co[i], t);
        co_post += omega[i] * (sp / t_post);
        double sl = 0;
        for (int t = 0; t < t_pre; ++t) sl += lambda[t] * R.at(co[i], t);
        co_pre += omega[i] * sl;
    }
    return (tr_post - tr_pre) - (co_post - co_pre);
}

// The staggered aggregate over `rows` (a multiset of rectangle row indices — the
// bootstrap passes a resample, the point estimate passes every row once).
// Returns false when the sample cannot support an estimate at all.
bool stagger_att(const Rect& R, const std::vector<int>& rows, const SdidConfig& cfg,
                 double* att_out, std::vector<SdidCohort>* cohorts_out) {
    std::vector<int> co;
    std::map<int, std::vector<int>> byadopt;
    for (int r : rows) {
        if (R.adopt[r] < 0) co.push_back(r);
        else byadopt[R.adopt[r]].push_back(r);
    }
    if (static_cast<int>(co.size()) < 2 || byadopt.empty()) return false;

    double num = 0, den = 0;
    std::vector<SdidCohort> cs;
    for (const auto& kv : byadopt) {
        const int t_pre = kv.first;
        if (t_pre < 2 || t_pre >= R.nt) return false;
        SdidCohort c;
        c.adopt = R.times[t_pre];
        c.n_treated = static_cast<int>(kv.second.size());
        c.n_control = static_cast<int>(co.size());
        c.t_pre = t_pre;
        c.t_post = R.nt - t_pre;
        c.tau = block_tau(R, co, kv.second, t_pre, cfg, &c.zeta);
        double w = static_cast<double>(c.n_treated) * c.t_post;
        num += w * c.tau;
        den += w;
        c.weight = w;
        cs.push_back(c);
    }
    if (!(den > 0)) return false;
    for (auto& c : cs) c.weight /= den;
    if (cohorts_out) *cohorts_out = std::move(cs);
    *att_out = num / den;
    return true;
}

SdidResult fail(SdidDiagnostic d, const std::string& msg) {
    SdidResult R;
    R.ok = false;
    R.diagnostic = d;
    R.error = msg;
    R.att = std::nan("");
    R.se = std::nan("");
    R.t = std::nan("");
    R.p = std::nan("");
    R.ci_lo = std::nan("");
    R.ci_hi = std::nan("");
    return R;
}

}  // namespace

SdidResult sdid_run(const double* y, const double* ivar, const double* time,
                     const double* treated, int64_t n_obs, const SdidConfig& cfg) {
    if (n_obs <= 0) return fail(SdidDiagnostic::BadPanel, "sdid: no observations");

    // ---- rectangle ------------------------------------------------------------
    Rect R;
    {
        std::vector<double> us(ivar, ivar + n_obs), ts(time, time + n_obs);
        std::sort(us.begin(), us.end());
        us.erase(std::unique(us.begin(), us.end()), us.end());
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
        R.units = std::move(us);
        R.times = std::move(ts);
        R.nu = static_cast<int>(R.units.size());
        R.nt = static_cast<int>(R.times.size());
    }
    if (R.nu < 2 || R.nt < 3)
        return fail(SdidDiagnostic::BadPanel,
                    "sdid: needs at least 2 panel units and 3 periods (have " +
                        std::to_string(R.nu) + " and " + std::to_string(R.nt) + ")");

    // NaN-filled so a hole in the (unit, period) grid is indistinguishable from a
    // missing outcome — both are the same defect and both must reach the guard.
    R.Y.assign(static_cast<size_t>(R.nu) * R.nt, std::nan(""));
    std::vector<int> first_treated(R.nu, -1), last_untreated(R.nu, -1);
    std::vector<char> seen(static_cast<size_t>(R.nu) * R.nt, 0);
    int64_t dup_cells = 0;
    for (int64_t k = 0; k < n_obs; ++k) {
        int u = static_cast<int>(std::lower_bound(R.units.begin(), R.units.end(), ivar[k]) -
                                 R.units.begin());
        int t = static_cast<int>(std::lower_bound(R.times.begin(), R.times.end(), time[k]) -
                                 R.times.begin());
        size_t idx = static_cast<size_t>(u) * R.nt + t;
        if (seen[idx]) ++dup_cells;
        seen[idx] = 1;
        R.Y[idx] = y[k];
        bool tr = !(std::isnan(treated[k])) && treated[k] != 0.0;
        if (tr) {
            if (first_treated[u] < 0 || t < first_treated[u]) first_treated[u] = t;
        } else {
            if (t > last_untreated[u]) last_untreated[u] = t;
        }
    }
    if (dup_cells > 0)
        return fail(SdidDiagnostic::BadPanel,
                    "sdid: " + std::to_string(dup_cells) +
                        " duplicate (unit, period) observation(s) — sdid needs one row per "
                        "unit-period cell");

    R.adopt = first_treated;
    int n_tr_units = 0, n_co_units = 0;
    for (int u = 0; u < R.nu; ++u) {
        if (R.adopt[u] < 0) { ++n_co_units; continue; }
        ++n_tr_units;
        if (last_untreated[u] > R.adopt[u])
            return fail(SdidDiagnostic::BadPanel,
                        "sdid: treatment is not absorbing for unit " +
                            std::to_string(R.units[u]) +
                            " — it is treated and then untreated again; sdid assumes staggered "
                            "adoption");
        if (R.adopt[u] < 2)
            return fail(SdidDiagnostic::BadPanel,
                        "sdid: unit " + std::to_string(R.units[u]) +
                            " adopts at the panel's first periods — sdid needs at least 2 "
                            "pre-treatment periods per adoption cohort");
    }
    if (n_tr_units == 0)
        return fail(SdidDiagnostic::BadPanel, "sdid: no treated units");
    if (n_co_units < 2)
        return fail(SdidDiagnostic::BadPanel,
                    "sdid: needs at least 2 never-treated control units (have " +
                        std::to_string(n_co_units) + ")");

    // ---- vce() admissibility, BEFORE the rectangle guard ----------------------
    // Order is oracle-pinned: on the FULL (gappy) panel Stata refuses
    // vce(placebo) with r(451) rather than reaching the r(504) missing-ATT
    // failure, so the placebo eligibility test comes first.
    std::string vce = cfg.vce;
    std::transform(vce.begin(), vce.end(), vce.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (vce == "placebo") {
        if (n_co_units <= n_tr_units)
            return fail(SdidDiagnostic::PlaceboInsufficientControls,
                        "sdid: vce(placebo) requires MORE control units than treated units — "
                        "this panel has " + std::to_string(n_co_units) + " control and " +
                        std::to_string(n_tr_units) + " treated units, so treatment cannot be "
                        "permuted among the controls (Stata r(451))");
        return fail(SdidDiagnostic::UnsupportedVce,
                    "sdid: vce(placebo) is not implemented in this release — only its refusal "
                    "when controls do not outnumber treated units is. Use vce(bootstrap)");
    }
    if (vce == "jackknife")
        return fail(SdidDiagnostic::UnsupportedVce,
                    "sdid: vce(jackknife) is not implemented in this release. Use "
                    "vce(bootstrap) or vce(noinference)");
    if (vce != "bootstrap" && vce != "noinference")
        return fail(SdidDiagnostic::UnsupportedVce,
                    "sdid: unknown vce('" + cfg.vce +
                        "') — supported: bootstrap, noinference");

    // ---- the incomplete-rectangle guard (LOAD-BEARING) ------------------------
    // AAHIW21's programs are defined on a complete unit x period rectangle. With a
    // hole anywhere, every downstream mean, weight and difference inherits it and
    // the ATT comes out MISSING — which is precisely what Stata reports on this
    // panel (r(504) "estimates post: matrix has missing values", raised only AFTER
    // all replications have run, and why vce(noinference) "succeeds" with a `.`).
    // Shipping that number, or a plausible-looking substitute computed from a
    // silently reduced sample, is the cardinal defect class here. So: refuse, name
    // the reason, and say how many cells are at fault.
    //
    // cfg.unguard_incomplete_panel deliberately skips this block — see sdid.h. The
    // unit test runs BOTH arms so that deleting the guard turns the suite red.
    if (!cfg.unguard_incomplete_panel) {
        int64_t bad_cells = 0, bad_units = 0;
        for (int u = 0; u < R.nu; ++u) {
            bool any = false;
            for (int t = 0; t < R.nt; ++t)
                if (std::isnan(R.at(u, t))) { ++bad_cells; any = true; }
            if (any) ++bad_units;
        }
        if (bad_cells > 0)
            return fail(SdidDiagnostic::IncompletePanel,
                        "sdid: the (unit, period) rectangle is incomplete — " +
                            std::to_string(bad_cells) + " of " +
                            std::to_string(static_cast<int64_t>(R.nu) * R.nt) +
                            " cells are absent or carry a missing outcome, across " +
                            std::to_string(bad_units) + " of " + std::to_string(R.nu) +
                            " units. sdid requires a complete balanced panel; on an incomplete "
                            "one the estimator returns a MISSING ATT rather than a smaller-sample "
                            "answer (Stata r(504) 'estimates post: matrix has missing values'). "
                            "Drop the units carrying a missing outcome and re-run");
    }

    // ---- point estimate -------------------------------------------------------
    std::vector<int> allrows(R.nu);
    for (int u = 0; u < R.nu; ++u) allrows[u] = u;
    SdidResult out;
    out.n_obs = n_obs;
    out.n_units = R.nu;
    out.n_treated_units = n_tr_units;
    out.n_control_units = n_co_units;
    out.n_periods = R.nt;

    double att = 0;
    if (!stagger_att(R, allrows, cfg, &att, &out.cohorts))
        return fail(SdidDiagnostic::BadPanel,
                    "sdid: the panel does not support an estimate (no usable adoption cohort "
                    "with at least 2 pre-periods and 2 never-treated controls)");
    out.att = att;

    // ---- vce(bootstrap): AAHIW21 Algorithm 2, resampling PANEL UNITS ----------
    // The resampling unit is the unit, not the row: a unit's whole time path is
    // drawn together, so the replicate estimates inherit the within-unit
    // dependence the clustered variance is supposed to price. Draws come from
    // core/rng.h's counter-based Philox — replicate b, position j is a pure
    // function of (seed, b, j), so the replicate log is reproducible and
    // independent of evaluation order.
    if (vce == "noinference") {
        out.se = std::nan("");
        out.t = std::nan("");
        out.p = std::nan("");
        out.ci_lo = std::nan("");
        out.ci_hi = std::nan("");
        return out;
    }

    const int reps = cfg.reps;
    if (reps < 2)
        return fail(SdidDiagnostic::BadPanel,
                    "sdid: vce(bootstrap) needs reps() >= 2 (got " + std::to_string(reps) + ")");
    out.rep_att.reserve(static_cast<size_t>(reps));
    std::vector<int> draw(R.nu);
    for (int b = 1; b <= reps; ++b) {
        bool got = false;
        double tb = 0;
        for (int attempt = 0; attempt < 1000 && !got; ++attempt) {
            const uint64_t base = static_cast<uint64_t>(attempt) * 1000003ull;
            int n_tr = 0, n_co = 0;
            for (int j = 0; j < R.nu; ++j) {
                uint32_t r = Philox::draw32(cfg.seed, static_cast<uint64_t>(b), base + j);
                int pick = static_cast<int>((static_cast<uint64_t>(r) * R.nu) >> 32);
                draw[j] = pick;
                if (R.adopt[pick] < 0) ++n_co; else ++n_tr;
            }
            ++out.reps_redrawn;
            if (n_co < 2 || n_tr < 1) continue;
            if (stagger_att(R, draw, cfg, &tb, nullptr)) got = true;
        }
        if (!got)
            return fail(SdidDiagnostic::BadPanel,
                        "sdid: vce(bootstrap) could not draw a usable resample for replicate " +
                            std::to_string(b) + " (every draw lacked treated or control units)");
        out.rep_att.push_back(tb);
    }
    out.reps_done = static_cast<int>(out.rep_att.size());
    out.reps_redrawn -= out.reps_done;  // count only the REJECTED draws

    double mean = 0;
    for (double v : out.rep_att) mean += v;
    mean /= out.reps_done;
    double var = 0;
    for (double v : out.rep_att) var += (v - mean) * (v - mean);
    var /= out.reps_done;  // AAHIW21 Algorithm 2: 1/B, not 1/(B-1)
    out.se = std::sqrt(var);

    // Large-sample (normal) inference, as the estimator's own output line says.
    const double alpha = (cfg.level == 95.0) ? 0.05 : 1.0 - cfg.level / 100.0;
    const double zc = z_critical(alpha);
    out.t = (out.se > 0) ? out.att / out.se : std::nan("");
    out.p = (out.se > 0) ? std::erfc(std::fabs(out.t) * 0.70710678118654752440) : std::nan("");
    out.ci_lo = out.att - zc * out.se;
    out.ci_hi = out.att + zc * out.se;
    return out;
}

}  // namespace cecon
