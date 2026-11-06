// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/nnmatch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <string>

#include "core/error.h"
#include "core/linalg.h"

namespace cecon {

namespace {

// ---------------------------------------------------------------------------
// H2 — the match-set rule, as MEASURED (REFUTE-0 §3, refute-0/h2_check.log: 29
// runs, every id list and every ATE reproduced to <= 9e-16).
//
// The REPORT's original rule ("tolerance groups anchored at the smallest
// not-yet-taken candidate, whole groups until >= m") picks the WRONG MEMBERS
// when tolerance gaps CHAIN: on controls at relative gaps (0, 1e-8, 2e-8)
// StataBE returns [4 3] — the nearest candidate, at gap 0, is EXCLUDED — and
// the report rule's ATE is 2.17e-2 off. So:
//
//   SIZE    = the number of candidates whose distance is within (1+dtol) of the
//             m-th nearest distance (RELATIVE, on the distance itself);
//   MEMBERS = the first SIZE elements of a sequence built from transitively
//             CHAINED tolerance classes — a class grows while the NEXT sorted
//             candidate is within (1+dtol) of the PREVIOUS MEMBER's distance
//             (chained, not anchored on the class's first element) — classes
//             ascending by distance, and inside a class ASCENDING observation
//             number when the class distance is exactly 0, DESCENDING otherwise.
//
// H2 is a HYPOTHESIS about a mechanism that ships as Mata bytecode; it is
// recorded as a named divergence RISK in internal note METHOD-RECEIPTS.md. Production
// paper 076 matches on exact indicator cells (d = 0), where H2 and the report
// rule coincide, so the 076 pairing does not depend on the open part.
//
// `cand` must already be sorted ascending by (d, index).
std::vector<int> select_h2(const std::vector<double>& d, const std::vector<int>& cand, int m,
                           double dtol) {
    if (cand.empty()) return {};
    const int take = std::min<int>(m, static_cast<int>(cand.size()));
    const double dm = d[cand[static_cast<std::size_t>(take) - 1]];
    std::size_t size = 0;
    for (int j : cand) {
        if (d[j] <= dm * (1.0 + dtol)) ++size;
        else break;  // cand is sorted ascending, so the first miss ends it
    }
    std::vector<int> seq;
    seq.reserve(cand.size());
    std::vector<int> cur{cand[0]};
    auto flush = [&]() {
        if (d[cur.front()] == 0.0) std::sort(cur.begin(), cur.end());
        else std::sort(cur.begin(), cur.end(), std::greater<int>());
        seq.insert(seq.end(), cur.begin(), cur.end());
        cur.clear();
    };
    for (std::size_t k = 1; k < cand.size(); ++k) {
        if (d[cand[k]] <= d[cur.back()] * (1.0 + dtol)) {
            cur.push_back(cand[k]);
        } else {
            flush();
            cur.push_back(cand[k]);
        }
    }
    if (!cur.empty()) flush();
    seq.resize(std::min(size, seq.size()));
    return seq;
}

// Sort candidate indices ascending by (distance, index). The index tiebreak
// only fixes the CHAINING order among exactly-equal distances, which the class
// re-ordering above then overwrites — it is here so the walk is deterministic.
void sort_candidates(const std::vector<double>& d, std::vector<int>& cand) {
    std::sort(cand.begin(), cand.end(), [&](int a, int b) {
        if (d[a] != d[b]) return d[a] < d[b];
        return a < b;
    });
}

}  // namespace

NnmatchResult nnmatch_fit(const double* y, const double* t,
                          const std::vector<std::string>& xnames,
                          const std::vector<const double*>& xcols,
                          const std::vector<std::string>& fvnames,
                          const std::vector<const double*>& fvcols,
                          const std::vector<char>& keep, const NnmatchOpts& o) {
    NnmatchResult R;
    const std::size_t nrows = keep.size();

    // ---- listwise-complete row set (pca.cpp:22-38's shape) -----------------
    std::vector<int64_t> rows;
    for (std::size_t i = 0; i < nrows; ++i) {
        if (!keep[i]) continue;
        if (std::isnan(y[i]) || std::isnan(t[i])) continue;
        bool ok = true;
        for (const double* c : xcols) if (std::isnan(c[i])) { ok = false; break; }
        if (ok) for (const double* c : fvcols) if (std::isnan(c[i])) { ok = false; break; }
        if (ok) rows.push_back(static_cast<int64_t>(i));
    }
    const int n = static_cast<int>(rows.size());
    if (n == 0) throw EngineError("no_obs", "nnmatch: no observations", "r(2000)");

    // ---- treatment: exactly two levels, control = the LOWER one ------------
    std::set<double> levels;
    for (int64_t r : rows) levels.insert(t[r]);
    if (levels.size() != 2)
        throw EngineError("bad_input",
                          "nnmatch: the treatment variable must take exactly two values in the "
                          "estimation sample (found " + std::to_string(levels.size()) + ")",
                          "r(198)");
    R.control_level = *levels.begin();
    R.treated_level = *levels.rbegin();

    std::vector<int> tt(n);       // 0 = control, 1 = treated, in kept order
    for (int i = 0; i < n; ++i) tt[i] = (t[rows[i]] == R.treated_level) ? 1 : 0;
    const int n1 = std::accumulate(tt.begin(), tt.end(), 0);
    const int n0 = n - n1;
    if (n0 == 0 || n1 == 0)
        throw EngineError("no_obs", "nnmatch: a treatment group is empty", "r(2000)");
    const int nmin = std::min(n0, n1);
    R.n = n; R.n0 = n0; R.n1 = n1;

    // ---- option validation, with Stata's own texts (ParseNNeighbor / ParseVCE)
    if (o.nneighbor < 1 || o.nneighbor > nmin)
        throw EngineError("bad_input",
                          "nnmatch: nneighbor(#) must be an integer between 1 and the number of "
                          "observations in the smallest group = " + std::to_string(nmin),
                          "r(198)");
    if (o.vce_nn < 2 || o.vce_nn > nmin)
        throw EngineError("bad_input",
                          "nnmatch: invalid vce() specification; nn(#) must be an integer between "
                          "2 and the number of observations in the smallest group = " +
                              std::to_string(nmin),
                          "r(198)");
    if (o.metric != "mahalanobis" && o.metric != "ivariance" && o.metric != "euclidean")
        throw EngineError("bad_input", "nnmatch: metric() must be mahalanobis, ivariance or "
                                       "euclidean", "r(198)");

    // The vce() bound above is PARSE-TIME and Stata's own is the same one:
    // measured on the golden 200-row fixture (n0=101, n1=99, nmin=99),
    // `vce(robust, nn(100))` and `nn(101)` are both r(198) with exactly the
    // text above, and `nn(1)` is r(198) too. But nn(99) is NOT a parse error in
    // Stata -- it is accepted by ParseVCE and then killed at RUNTIME:
    //
    //   . teffects nnmatch (y x1 x2) (t), vce(robust, nn(99))
    //   fewer than 99 nearest-neighbor matches for observation 4 when in-group
    //   matching for the robust() option; use option osample() to identify all
    //   observations with deficient matches                            [r(459)]
    //
    // because the in-group set Psi(i) draws from the OTHER members of i's own
    // group -- nmin - 1 of them, not nmin. Without this check `select_h2`'s
    // `take = min(m, cand.size())` silently truncates instead, and the run
    // comes back rc 0 with se .1046884 built from 98 others for the 99-member
    // treated group and 99 for the 101-member control group: two different h's
    // inside one variance, under a number the user typed. (Doctrine gate (1).)
    // The legal boundary is parity-clean -- nn(98): PIET .1045807222286699 vs
    // StataBE 1.04580722228670e-01 -- which is why no fixture saw it.
    //
    // Stata reports the FIRST deficient observation by frame order and the
    // number it prints is frame `_n`, not a position in the estimation sample:
    // in that fixture t = mod(floor((_n-1)/3),2), so the first TREATED row is
    // _n == 4 and Stata says "observation 4". `rows[i] + 1` is that number.
    //
    // The Omega side needs no such check: nneighbor(#) is bounded by nmin above
    // and i's OPPOSITE group always holds n0 or n1 >= nmin candidates, so an
    // accepted nneighbor() can never come up short. Nothing is invented here --
    // only the one message that was measured.
    for (int i = 0; i < n; ++i) {
        const int in_group_others = (tt[i] ? n1 : n0) - 1;
        if (in_group_others < o.vce_nn)
            throw EngineError("bad_input",
                              "fewer than " + std::to_string(o.vce_nn) +
                                  " nearest-neighbor matches for observation " +
                                  std::to_string(rows[i] + 1) +
                                  " when in-group matching for the robust() option; use option "
                                  "osample() to identify all observations with deficient matches",
                              "r(459)");
    }

    // ---- design: continuous columns as typed, then i.v expanded ------------
    // e(fvdvarlist) spelling, measured: `0b.f1 1.f1 0b.f2 1.f2 2.f2 ...` — one
    // column per level ABOVE the minimum, ascending, terms in typed order.
    //
    // NAMED DIVERGENCE (METHOD-RECEIPTS.md, nnmatch row, divergence 6): this
    // puts every `--x` continuous column FIRST and the `--fv` factor blocks
    // AFTER, where Stata's order is the TYPED one — measured `x1 0b.a 1.a 0b.b
    // 1.b 2.b` for `(y x1 i.a i.b)`, so the two agree only when the continuous
    // terms happen to be typed first. Distances are invariant to column order
    // for all three metrics WHEN NOTHING IS DROPPED, which is why no fixture
    // can see it; but the order decides which column the left-to-right _rmcoll
    // sweep below drops in a collinear design, and a different dropped column
    // is a different metric under euclidean/ivariance. e(mvarlist) is
    // published in typed order regardless (the PIET handler builds it from the
    // parsed tokens, not from these two vectors).
    std::vector<std::string> names;
    std::vector<std::vector<double>> cols;   // each length n, in kept order
    for (std::size_t j = 0; j < xcols.size(); ++j) {
        names.push_back(xnames[j]);
        std::vector<double> c(n);
        for (int i = 0; i < n; ++i) c[i] = xcols[j][rows[i]];
        cols.push_back(std::move(c));
    }
    for (std::size_t j = 0; j < fvcols.size(); ++j) {
        std::set<double> lv;
        for (int i = 0; i < n; ++i) {
            const double v = fvcols[j][rows[i]];
            if (v < 0 || v != std::floor(v))
                throw EngineError("bad_input",
                                  "nnmatch: factor variable " + fvnames[j] +
                                      " may not contain negative or noninteger values",
                                  "r(452)");
            lv.insert(v);
        }
        bool first = true;
        for (double L : lv) {
            if (first) { first = false; continue; }  // 0b. base = the MINIMUM level
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(L));
            names.push_back(std::string(buf) + "." + fvnames[j]);
            std::vector<double> c(n);
            for (int i = 0; i < n; ++i) c[i] = (fvcols[j][rows[i]] == L) ? 1.0 : 0.0;
            cols.push_back(std::move(c));
        }
    }
    if (cols.empty())
        throw EngineError("bad_input", "nnmatch: at least one matching variable is required",
                          "r(198)");

    // ---- _rmcoll: drop perfectly collinear matching columns --
    // Measured (INFO|nn_rmcoll, i.b(4) with a == b>=2, so 1.a == 2.b + 3.b):
    // e(fvdvarlist) = `0b.a 1.a 0b.b 1.b 2.b 3o.b` — the sweep runs LEFT TO
    // RIGHT in design order and drops the LATER redundant column, 3.b, keeping
    // 1.a. Modified Gram-Schmidt on the CENTRED columns (a constant column is
    // therefore also dropped), one re-orthogonalisation pass for stability.
    std::vector<double> mean(cols.size());
    for (std::size_t j = 0; j < cols.size(); ++j) mean[j] = la::sum(cols[j].data(), n) / n;
    std::vector<int> kept;
    {
        std::vector<std::vector<double>> Q;
        for (std::size_t j = 0; j < cols.size(); ++j) {
            std::vector<double> v(n);
            for (int i = 0; i < n; ++i) v[i] = cols[j][i] - mean[j];
            const double ss0 = la::sumsq(v.data(), n);
            for (int pass = 0; pass < 2; ++pass)
                for (const auto& q : Q) {
                    const double p = la::dot(q.data(), v.data(), n);
                    for (int i = 0; i < n; ++i) v[i] -= p * q[i];
                }
            const double ss = la::sumsq(v.data(), n);
            if (ss <= 1e-11 * std::max(ss0, 1e-300)) { R.dropped.push_back(names[j]); continue; }
            kept.push_back(static_cast<int>(j));
            const double inv = 1.0 / std::sqrt(ss);
            for (int i = 0; i < n; ++i) v[i] *= inv;
            Q.push_back(std::move(v));
        }
    }
    if (kept.empty())
        throw EngineError("bad_input", "nnmatch: every matching variable was dropped as collinear",
                          "r(459)");
    const int p = static_cast<int>(kept.size());

    // ---- centred design, scale matrix S, and the transform Z = Xc * S^-1/2 --
    // S = (X - xbar)'(X - xbar)/(n-1), POOLED over both groups (causal.pdf
    // p.645); diag(S) for ivariance; I for euclidean. e(mscale) is the
    // SYMMETRIC square root of S^-1 (verify.py:207-213 measured it against the
    // two Cholesky forms and inv-chol; only the symmetric root matches, 5.2e-12),
    // which is also the transform that makes the distance a plain Euclidean
    // norm — and that is how it is used here, so identical covariate rows give
    // a distance of EXACTLY zero (the production tie class).
    std::vector<double> Xc(static_cast<std::size_t>(n) * p);
    for (int j = 0; j < p; ++j) {
        const auto& c = cols[kept[j]];
        const double mu = mean[kept[j]];
        R.colnames.push_back(names[kept[j]]);
        R.mean.push_back(mu);
        for (int i = 0; i < n; ++i) Xc[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * n] = c[i] - mu;
    }
    std::vector<double> S(static_cast<std::size_t>(p) * p);
    la::xtx(Xc.data(), n, p, S.data());
    for (auto& v : S) v /= static_cast<double>(n - 1);
    for (int j = 0; j < p; ++j) R.sd.push_back(std::sqrt(S[static_cast<std::size_t>(j) + static_cast<std::size_t>(j) * p]));

    std::vector<double> M(static_cast<std::size_t>(p) * p, 0.0);  // S^{-1/2}
    if (o.metric == "euclidean") {
        for (int j = 0; j < p; ++j) M[static_cast<std::size_t>(j) + static_cast<std::size_t>(j) * p] = 1.0;
    } else if (o.metric == "ivariance") {
        for (int j = 0; j < p; ++j) {
            const double s = R.sd[j];
            if (!(s > 0))
                throw EngineError("bad_input",
                                  "nnmatch: metric(ivariance) needs a nonzero variance for " +
                                      R.colnames[j], "r(459)");
            M[static_cast<std::size_t>(j) + static_cast<std::size_t>(j) * p] = 1.0 / s;
        }
    } else {
        std::vector<double> A = S, w(p);
        if (la::sym_eig(A.data(), p, w.data()) != 0)
            throw EngineError("collinear", "nnmatch: the matching covariance matrix is singular",
                              "r(459)");
        for (int k = 0; k < p; ++k) {
            if (!(w[k] > 0))
                throw EngineError("collinear",
                                  "nnmatch: the matching covariance matrix is singular", "r(459)");
            w[k] = 1.0 / std::sqrt(w[k]);
        }
        for (int j = 0; j < p; ++j)
            for (int i = 0; i <= j; ++i) {
                double v = 0;
                for (int k = 0; k < p; ++k)
                    v += A[static_cast<std::size_t>(i) + static_cast<std::size_t>(k) * p] * w[k] *
                         A[static_cast<std::size_t>(j) + static_cast<std::size_t>(k) * p];
                M[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * p] = v;
                M[static_cast<std::size_t>(j) + static_cast<std::size_t>(i) * p] = v;
            }
        R.mscale = M;
    }
    std::vector<double> Z(static_cast<std::size_t>(n) * p);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < p; ++c) {
            double v = 0;
            for (int k = 0; k < p; ++k)
                v += Xc[static_cast<std::size_t>(i) + static_cast<std::size_t>(k) * n] *
                     M[static_cast<std::size_t>(k) + static_cast<std::size_t>(c) * p];
            Z[static_cast<std::size_t>(i) + static_cast<std::size_t>(c) * n] = v;
        }

    // ---- match sets, imputation, K counts, conditional variance ------------
    std::vector<int> idxT, idxC;
    for (int i = 0; i < n; ++i) (tt[i] ? idxT : idxC).push_back(i);

    std::vector<std::vector<int>> omega(n);
    std::vector<double> te(n), po0(n), po1(n), xi2(n), K(n, 0.0), Kp(n, 0.0);
    std::vector<double> d(n);
    R.dist.assign(n, {});
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double s = 0;
            for (int c = 0; c < p; ++c) {
                const double dz = Z[static_cast<std::size_t>(i) + static_cast<std::size_t>(c) * n] -
                                  Z[static_cast<std::size_t>(j) + static_cast<std::size_t>(c) * n];
                s += dz * dz;
            }
            d[j] = std::sqrt(s);
        }
        std::vector<int> cand = tt[i] ? idxC : idxT;
        sort_candidates(d, cand);
        omega[i] = select_h2(d, cand, o.nneighbor, o.dtolerance);
        double sum = 0;
        for (int j : omega[i]) { sum += y[rows[j]]; R.dist[i].push_back(d[j]); }
        const double yhat = sum / static_cast<double>(omega[i].size());
        const double yi = y[rows[i]];
        po1[i] = tt[i] ? yi : yhat;
        po0[i] = tt[i] ? yhat : yi;
        te[i] = po1[i] - po0[i];

        // Psi(i) = the unit ITSELF plus its h nearest SAME-group others, chosen
        // by the same H2 rule (REPORT §4.6; the manual-literal |Psi| = h reading
        // is 16.7% off on F1, "h others without self" 1.0% off on F2).
        std::vector<int> same;
        for (int j : (tt[i] ? idxT : idxC)) if (j != i) same.push_back(j);
        sort_candidates(d, same);
        std::vector<int> psi = select_h2(d, same, o.vce_nn, o.dtolerance);
        psi.push_back(i);
        double sy = 0;
        for (int j : psi) sy += y[rows[j]];
        const double ybar = sy / static_cast<double>(psi.size());
        double ss = 0;
        for (int j : psi) { const double e = y[rows[j]] - ybar; ss += e * e; }
        xi2[i] = psi.size() > 1 ? ss / static_cast<double>(psi.size() - 1) : 0.0;
    }
    for (int j = 0; j < n; ++j) {
        if (o.atet && tt[j] != 1) continue;   // atet: K counts run over TREATED j
        const double w = 1.0 / static_cast<double>(omega[j].size());
        for (int i : omega[j]) { K[i] += w; Kp[i] += w * w; }
    }

    R.k_nnmin = static_cast<int>(omega[0].size());
    R.k_nnmax = R.k_nnmin;
    for (int i = 0; i < n; ++i) {
        R.k_nnmin = std::min<int>(R.k_nnmin, static_cast<int>(omega[i].size()));
        R.k_nnmax = std::max<int>(R.k_nnmax, static_cast<int>(omega[i].size()));
    }

    // ---- point estimate and Abadie-Imbens variance ------------------------
    if (!o.atet) {
        double s = 0;
        for (int i = 0; i < n; ++i) s += te[i];
        R.tau = s / n;
        double v = 0;
        for (int i = 0; i < n; ++i) {
            const double dte = te[i] - R.tau;
            v += dte * dte + xi2[i] * (K[i] * K[i] + 2 * K[i] - Kp[i]);
        }
        R.V = v / (static_cast<double>(n) * n);
    } else {
        double s = 0;
        for (int i = 0; i < n; ++i) if (tt[i]) s += te[i];
        R.tau = s / n1;
        // Abadie-Imbens (2006) Thm 7: the K-term lives on the CONTROLS. The
        // manual's typeset sigma^2_delta (t_i multiplying everything) is
        // falsified on F10/F13 and on refute_D's continuous covariates.
        double v = 0;
        for (int i = 0; i < n; ++i) {
            if (tt[i]) { const double dte = te[i] - R.tau; v += dte * dte; }
            else v += xi2[i] * (K[i] * K[i] - Kp[i]);
        }
        R.V = v / (static_cast<double>(n1) * n1);
    }

    R.rows = rows;
    R.te = te; R.po0 = po0; R.po1 = po1;
    R.omega.assign(n, {});
    for (int i = 0; i < n; ++i)
        for (int j : omega[i]) R.omega[i].push_back(rows[j] + 1);   // 1-based frame _n
    if (o.metric != "ivariance") R.sd.clear();  // e(mstats) `stddev` is ivariance-only
    return R;
}

}  // namespace cecon
