// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vcov/vcov.h"

#include "hdfe/dof.h"
#include "core/error.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/linalg.h"

namespace cecon {

bool stata_post_vcov_screen(std::vector<double>& V, int k) {
    // Measured on hand-built 2x2/3x3 matrices posted with `ereturn post b V`
    // (correlation 1 + 9e-11 clamped to 1, 1 + 1e-10 zeroed, either sign, any scale;
    // diag(1, -1e-3) and indefinite matrices with |r| <= .9 posted as given).
    for (int c = 0; c < k; ++c)
        for (int r = c + 1; r < k; ++r) {
            const double vr = V[static_cast<size_t>(r) + static_cast<size_t>(r) * k];
            const double vc = V[static_cast<size_t>(c) + static_cast<size_t>(c) * k];
            if (!(vr > 0.0) || !(vc > 0.0)) continue;
            const double s = std::sqrt(vr * vc);
            const double v = V[static_cast<size_t>(r) + static_cast<size_t>(c) * k];
            // The excess as a difference: the literal 1.0000000001 (zeroed by Stata)
            // is the same double as 1 + 1e-10.
            if (std::abs(v) - s > 1e-10 * s) {
                std::fill(V.begin(), V.end(), 0.0);
                return false;
            }
        }
    for (int c = 0; c < k; ++c)
        for (int r = c + 1; r < k; ++r) {
            const size_t lo = static_cast<size_t>(r) + static_cast<size_t>(c) * k;
            const size_t up = static_cast<size_t>(c) + static_cast<size_t>(r) * k;
            const double vr = V[static_cast<size_t>(r) + static_cast<size_t>(r) * k];
            const double vc = V[static_cast<size_t>(c) + static_cast<size_t>(c) * k];
            if (!(vr > 0.0) || !(vc > 0.0)) continue;
            const double s = std::sqrt(vr * vc);
            if (std::abs(V[lo]) > s) V[lo] = V[up] = std::copysign(s, V[lo]);
        }
    return true;
}

void hc_meat(const double* design, int64_t n, int k, const double* e, double* meat) {
    std::vector<double> W(static_cast<size_t>(n) * k);
    for (int j = 0; j < k; ++j) {
        const double* xj = design + static_cast<int64_t>(j) * n;
        double* wj = W.data() + static_cast<int64_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) wj[i] = xj[i] * e[i];
    }
    la::xtx(W.data(), n, k, meat);
}

void cluster_scores(const double* design, int64_t n, int k, const double* e,
                    const Factor& cl, double* S) {
    const int G = cl.n_levels;
    std::fill(S, S + static_cast<size_t>(G) * k, 0.0);
    const int32_t* g = cl.codes.data();
    for (int j = 0; j < k; ++j) {
        const double* xj = design + static_cast<int64_t>(j) * n;
        double* Sj = S + static_cast<int64_t>(j) * G;
        for (int64_t i = 0; i < n; ++i) Sj[g[i]] += xj[i] * e[i];
    }
}

void cluster_meat(const double* design, int64_t n, int k, const double* e,
                  const Factor& cl, double* meat, int* n_clusters) {
    const int G = cl.n_levels;
    if (n_clusters) *n_clusters = G;
    std::vector<double> S(static_cast<size_t>(G) * k, 0.0);
    cluster_scores(design, n, k, e, cl, S.data());
    la::xtx(S.data(), G, k, meat);  // meat = S'S
}

void multiway_meat(const double* design, int64_t n, int k, const double* e,
                   const std::vector<const Factor*>& cls, double* meat,
                   const std::function<double(int)>* subset_scale) {
    const int d = static_cast<int>(cls.size());
    if (d < 1 || d > kMaxMultiwayClusters)
        throw EngineError("bad_input", "multiway meat needs 1.." +
                                           std::to_string(kMaxMultiwayClusters) +
                                           " cluster variables");
    const size_t kk = static_cast<size_t>(k) * k;
    std::fill(meat, meat + kk, 0.0);
    std::vector<double> m(kk);
    int Gs = 0;
    // Inclusion-exclusion (Cameron-Gelbach-Miller 2011, eq. 2.13): every non-empty
    // subset S of the dimensions contributes its intersection-cluster meat with sign
    // (-1)^(|S|+1). For d = 2 this is m1 + m2 - m12, the two-way form this replaced,
    // in the same order of additions.
    for (unsigned mask = 1; mask < (1u << d); ++mask) {
        int size = 0;
        Factor inter;
        const Factor* f = nullptr;
        for (int j = 0; j < d; ++j) {
            if (!(mask & (1u << j))) continue;
            if (!f) {
                f = cls[j];
            } else {
                inter = intersect_factors(*f, *cls[j]);
                f = &inter;
            }
            ++size;
        }
        cluster_meat(design, n, k, e, *f, m.data(), &Gs);
        double sign = (size % 2 == 1) ? 1.0 : -1.0;
        if (subset_scale) sign *= (*subset_scale)(Gs);
        for (size_t i = 0; i < kk; ++i) meat[i] += sign * m[i];
    }
}

namespace {

// Bit j of a membership mask = cluster variable j takes part in the grouping.
using MemberMask = unsigned;

// Listing order of e(kcluster): by size; for equal sizes, the first variable on
// which two masks disagree belongs to the mask listed first.
bool listed_before(MemberMask a, MemberMask b) {
    const size_t size_a = std::bitset<32>(a).count();
    const size_t size_b = std::bitset<32>(b).count();
    if (size_a != size_b) return size_a < size_b;
    const MemberMask differing = a ^ b;
    const MemberMask first_difference = differing & (~differing + 1u);
    return (a & first_difference) != 0;
}

// Number of groups in the intersection of the mask's variables (a non-empty mask).
int groups_in_intersection(const std::vector<Factor>& cluster, MemberMask members) {
    std::vector<const Factor*> involved;
    for (size_t v = 0; v < cluster.size(); ++v)
        if (members >> v & 1u) involved.push_back(&cluster[v]);
    if (involved.size() == 1) return involved.front()->n_levels;
    Factor crossed = intersect_factors(*involved[0], *involved[1]);
    for (size_t m = 2; m < involved.size(); ++m) crossed = intersect_factors(crossed, *involved[m]);
    return crossed.n_levels;
}

std::string joined_member_names(const std::vector<Factor>& cluster, MemberMask members) {
    std::string joined;
    for (size_t v = 0; v < cluster.size(); ++v) {
        if (!(members >> v & 1u)) continue;
        if (!joined.empty()) joined += '#';
        joined += cluster[v].name;
    }
    return joined;
}

}  // namespace

std::vector<ClusterCombo> multiway_cluster_combos(const std::vector<Factor>& cluster) {
    const int p = static_cast<int>(cluster.size());
    if (p < 1 || p > kMaxMultiwayClusters) return {};
    std::vector<MemberMask> masks((1u << p) - 1u);
    std::iota(masks.begin(), masks.end(), MemberMask{1});
    std::sort(masks.begin(), masks.end(), listed_before);
    std::vector<ClusterCombo> combos;
    combos.reserve(masks.size());
    for (MemberMask m : masks)
        combos.push_back({joined_member_names(cluster, m), groups_in_intersection(cluster, m)});
    return combos;
}

int multiway_vcov_rank(const std::vector<double>& V, int k) {
    constexpr double kEigenFloor = 1e-12;
    if (k <= 0) return -1;
    const size_t order = static_cast<size_t>(k);
    if (V.size() != order * order) return -1;
    std::vector<double> workspace(V);  // the solver overwrites its input
    std::vector<double> eigenvalues(order);
    if (la::sym_eig(workspace.data(), k, eigenvalues.data()) != 0) return -1;
    return static_cast<int>(std::count_if(eigenvalues.begin(), eigenvalues.end(),
                                          [](double ev) { return ev > kEigenFloor; }));
}

namespace {

// Replace each off-diagonal pair of the k x k matrix by the pair's average.
void average_off_diagonal_pairs(std::vector<double>& A, int k) {
    const size_t order = static_cast<size_t>(k);
    for (size_t hi = 1; hi < order; ++hi) {
        for (size_t lo = 0; lo < hi; ++lo) {
            double& upper = A[lo + hi * order];
            double& lower = A[hi + lo * order];
            upper = lower = (upper + lower) / 2.0;
        }
    }
}

// The multiway variance of [R] regress, Methods and formulas, Multiway clustering:
// every grouping's one-way sandwich carries its own G/(G-1)·small_sample, the sandwiches
// enter with alternating sign by grouping size, and the symmetrised sum is projected
// onto the PSD cone only when it has a negative eigenvalue. Returns whether it did.
bool signed_sandwich_sum(const double* design, int64_t n, int k, const double* e,
                         const double* bread, const std::vector<const Factor*>& cls,
                         double small_sample, std::vector<double>& V) {
    const std::function<double(int)> own_factor = [small_sample](int groups) {
        return static_cast<double>(groups) / (groups - 1) * small_sample;
    };
    std::vector<double> scaled_meat(V.size());
    multiway_meat(design, n, k, e, cls, scaled_meat.data(), &own_factor);
    la::sandwich(bread, scaled_meat.data(), k, 1.0, V.data());
    average_off_diagonal_pairs(V, k);

    std::vector<double> projected(V);
    const int dropped = la::psd_clip(projected.data(), k);
    if (dropped < 0) throw EngineError("internal", "multiway vcov: eigendecomposition failed");
    if (dropped == 0) return false;
    average_off_diagonal_pairs(projected, k);
    V.swap(projected);
    return true;
}

// reghdfe_fix_psd(V, report_constant = 1) (as reghdfe does) applied in
// reghdfe's standardised units: Vs = V .* (s s'); if min eig(Vs) < 0, Vs is rebuilt
// with the negative eigenvalues set to 0 and its leading (k-1) x (k-1) block is
// replaced by the ORIGINAL block made PSD on its own the same way; V = Vs ./ (s s').
// Returns whether a fix was needed (V untouched otherwise).
bool reghdfe_fix_psd_scaled(std::vector<double>& V, int k, const std::vector<double>& s) {
    const size_t kk = static_cast<size_t>(k) * k;
    std::vector<double> Vs(kk);
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            const size_t i = static_cast<size_t>(r) + static_cast<size_t>(c) * k;
            Vs[i] = V[i] * s[static_cast<size_t>(r)] * s[static_cast<size_t>(c)];
        }
    // `if (!issymmetric(V)) _makesymmetric(V)` as in reghdfe (the lower triangle wins).
    for (int c = 0; c < k; ++c)
        for (int r = c + 1; r < k; ++r)
            Vs[static_cast<size_t>(c) + static_cast<size_t>(r) * k] =
                Vs[static_cast<size_t>(r) + static_cast<size_t>(c) * k];
    auto needs_fix = [](const std::vector<double>& A, int m) -> int {
        std::vector<double> E = A;
        std::vector<double> w(static_cast<size_t>(m));
        if (la::sym_eig(E.data(), m, w.data()) != 0) return -1;
        return w[0] < 0.0 ? 1 : 0;  // ascending: w[0] is the smallest
    };
    const int fix = needs_fix(Vs, k);
    if (fix < 0) throw EngineError("internal", "multiway vcov: eigendecomposition failed");
    if (fix == 0) return false;
    const int ks = k - 1;
    std::vector<double> slope(static_cast<size_t>(ks) * ks);
    for (int c = 0; c < ks; ++c)
        for (int r = 0; r < ks; ++r)
            slope[static_cast<size_t>(r) + static_cast<size_t>(c) * ks] =
                Vs[static_cast<size_t>(r) + static_cast<size_t>(c) * k];
    if (la::psd_clip(Vs.data(), k) < 0)
        throw EngineError("internal", "multiway vcov: eigendecomposition failed");
    if (ks > 0) {
        const int sfix = needs_fix(slope, ks);
        if (sfix < 0) throw EngineError("internal", "multiway vcov: eigendecomposition failed");
        if (sfix == 1 && la::psd_clip(slope.data(), ks) < 0)
            throw EngineError("internal", "multiway vcov: eigendecomposition failed");
        for (int c = 0; c < ks; ++c)
            for (int r = 0; r < ks; ++r)
                Vs[static_cast<size_t>(r) + static_cast<size_t>(c) * k] =
                    slope[static_cast<size_t>(r) + static_cast<size_t>(c) * ks];
    }
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            const size_t i = static_cast<size_t>(r) + static_cast<size_t>(c) * k;
            V[i] = Vs[i] / (s[static_cast<size_t>(r)] * s[static_cast<size_t>(c)]);
        }
    return true;
}

}  // namespace

bool fe_nested_in_cluster(const Factor& fe, const Factor& cl) {
    std::vector<int32_t> map(fe.n_levels, -1);
    const int64_t n = static_cast<int64_t>(fe.codes.size());
    for (int64_t i = 0; i < n; ++i) {
        int32_t f = fe.codes[i], c = cl.codes[i];
        if (map[f] == -1) map[f] = c;
        else if (map[f] != c) return false;
    }
    return true;
}

VcovResult compute_vcov(Vcov vt, const double* design, int64_t n, int k, const double* e,
                        const double* bread, int k_full, double sigma2,
                        const std::vector<Factor>& fe,
                        const std::vector<Factor>& cluster, int cluster_index, double n_eff,
                        bool ml_cluster_factor, bool keep_nested_fe_dof,
                        bool multiway_subset_factors, const std::vector<double>* clip_scale,
                        bool no_psd_fix) {
    VcovResult out;
    out.V.assign(static_cast<size_t>(k) * k, 0.0);
    // See the header: n_eff <= 0 means "the observation count IS n" — every
    // unweighted call, and every aw/pw call, lands there.
    const double nd = (n_eff > 0.0) ? n_eff : static_cast<double>(n);

    if (vt == Vcov::IID) {
        for (size_t i = 0; i < out.V.size(); ++i) out.V[i] = sigma2 * bread[i];
        out.type = "iid";
        return out;
    }
    if (vt == Vcov::HC1) {
        std::vector<double> meat(static_cast<size_t>(k) * k);
        hc_meat(design, n, k, e, meat.data());
        double factor = nd / (nd - static_cast<double>(k_full));
        la::sandwich(bread, meat.data(), k, factor, out.V.data());
        out.type = "HC1";
        return out;
    }

    // Cluster (one-way) or Multiway (CGM, k-way over every cluster variable given)
    if (cluster.empty()) throw EngineError("bad_input", "no cluster variable provided");
    std::vector<const Factor*> cls;
    if (vt == Vcov::Cluster) {
        cls.push_back(&cluster.at(cluster_index));
    } else {
        if (cluster.size() < 2)
            throw EngineError("bad_input", "multiway vcov needs >= 2 cluster variables");
        // Every dimension takes part. Before k-way support only cluster[0..1] were read,
        // so a third name passed to --cluster was dropped without a word.
        if (cluster.size() > static_cast<size_t>(kMaxMultiwayClusters))
            throw EngineError("bad_input",
                              "multiway vcov supports at most " +
                                  std::to_string(kMaxMultiwayClusters) +
                                  " cluster variables, got " + std::to_string(cluster.size()));
        for (const Factor& c : cluster) cls.push_back(&c);
    }

    // FU-365: Stata's built-in multiway convention (see the header). Only a multiway
    // vcov reads the switch; a one-way cluster keeps its single factor either way.
    const bool subset_factors = multiway_subset_factors && cls.size() > 1;

    int Gmin = cls[0]->n_levels;
    for (const Factor* c : cls) Gmin = std::min(Gmin, c->n_levels);
    // E5 (COUNCIL-20260710, User ruling 2026-07-10): cluster-robust SEs are undefined
    // with a single cluster — the G/(G-1) small-sample factor below divides by G-1=0,
    // yielding a non-finite (JSON-null) SE. Refuse explicitly instead of emitting a
    // degenerate result. NOTE: Stata itself runs G=1 clustering and reports a missing
    // SE (rc=0); this is a deliberate doctrine-owner override — an honest error beats
    // bug-for-bug fidelity here, and it matches wildboot.cpp's existing G<2 refusal.
    if (Gmin < 2)
        throw EngineError("bad_input",
                          "insufficient clusters: vce(cluster) requires at least 2 clusters, "
                          "but the cluster variable has " + std::to_string(Gmin));
    int k_adj = k_full;
    // BUG-568: the nested-FE exemption is reghdfe's / xtreg,fe's convention; areg
    // (keep_nested_fe_dof) charges every absorbed parameter. See the header. Under the
    // built-in multiway convention areg charges every absorbed parameter (the caller
    // passes keep_nested_fe_dof) and xtreg,fe only those not nested in a cluster, which
    // is this exemption.
    //
    // With an effect nested in a cluster the absorbed count is reghdfe's: k_full
    // carries compute_dof's df_a (no nesting), which is swapped for the df_a that
    // drops the nested effects and re-runs the pairwise redundancy over the rest,
    // plus reghdfe's nested_adj of one (as reghdfe does). Swapping a
    // per-effect (K-1) instead misses the redundancy between the effects left over:
    // absorb(geo_id sido_month ym weather) cluster(geo_id) charged ym's 12 mobility
    // groups with sido_month as 1, 11 parameters too many (Wind IV, 2026-11-05).
    if (!keep_nested_fe_dof) {
        std::vector<char> nested(fe.size(), 0);
        for (size_t g = 0; g < fe.size(); ++g) {
            for (const Factor* c : cls)
                if (fe_nested_in_cluster(fe[g], *c)) { nested[g] = 1; break; }
            if (nested[g]) out.nested_fe.push_back(fe[g].name);
        }
        if (!out.nested_fe.empty())
            k_adj = k_full - absorbed_dof(fe, {}).df_a + absorbed_dof(fe, nested).df_a + 1;
    }
    out.n_clusters = Gmin;
    out.cluster_dof_k = k_adj;
    // BUG-175: likelihood commands drop the regression (N-1)/(N-K) term — Stata's
    // ML cluster multiplier is the bare G/(G-1). Measured against the P2 answerbook
    // (`poisson ycount x1 x2 i.region, vce(cluster id)`, N=4671 G=200 K=7): the pure
    // factor lands within 4.6e-06 relative on all seven SEs, while carrying the
    // regression term misses by 6.5e-04 — exactly sqrt((N-1)/(N-K)). See BUG-175 for
    // the full candidate sweep. k_adj still drives the REPORTED cluster_dof_k and the
    // nested-FE accounting above either way; only the multiplier changes.
    //
    // Multiway, reghdfe's convention (the default): ONE factor, built from the smallest
    // single-dimension G, multiplies the whole inclusion-exclusion meat — not a G_S/(G_S-1) per subset. Measured on
    // w2b_estim.log (N=60, absorb(id) nested in cluster id, K=3): Gmin/(Gmin-1)·(N-1)/(N-K)
    // reproduces reghdfe_cluster3 se[x1] .0575950937310231 (G = 10,6,4; Gmin 4) and
    // reghdfe_cluster2_ctl .060463723825293 (Gmin 6) to ~1e-15 relative on the oracle's
    // float-stored data, while the per-subset variant misses by 5.2% (3-way) and 1.7%
    // (2-way). e(df_r) = Gmin-1 (3 and 5 there) follows from n_clusters = Gmin.
    // Stata's built-in regress/areg multiway is the subset_factors branch (FU-365);
    // its e(df_r) is the minimum over every combination less 1, which is Gmin-1 too
    // (an intersection never has fewer levels than its components).
    std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
    if (subset_factors) {
        // FU-365. Every intersection has at least Gmin >= 2 groups, so each factor is finite.
        const double regression_term = (nd - 1.0) / (nd - static_cast<double>(k_adj));
        out.npd_fix = signed_sandwich_sum(design, n, k, e, bread, cls,
                                          ml_cluster_factor ? 1.0 : regression_term, out.V);
    } else {
        int Gdummy = 0;
        if (cls.size() == 1) {
            cluster_meat(design, n, k, e, *cls[0], meat.data(), &Gdummy);
        } else {
            multiway_meat(design, n, k, e, cls, meat.data());
        }
        double factor = static_cast<double>(Gmin) / (Gmin - 1);
        if (!ml_cluster_factor) factor *= (nd - 1.0) / (nd - static_cast<double>(k_adj));
        la::sandwich(bread, meat.data(), k, factor, out.V.data());
        if (cls.size() > 1 && !no_psd_fix) {
            if (clip_scale != nullptr && clip_scale->size() == static_cast<size_t>(k))
                out.npd_fix = reghdfe_fix_psd_scaled(out.V, k, *clip_scale);  // FU-389
            else
                out.npd_fix = (la::psd_clip(out.V.data(), k) > 0);
        }
    }
    if (cls.size() == 1) {
        out.type = "cluster(" + cls[0]->name + ")";
    } else {
        // Two dimensions keep the historical `twoway(a,b)` spelling (byte-identical);
        // three or more read `multiway(a,b,c)`.
        out.type = cls.size() == 2 ? "twoway(" : "multiway(";
        for (size_t d = 0; d < cls.size(); ++d) out.type += (d ? "," : "") + cls[d]->name;
        out.type += ")";
    }
    return out;
}

}  // namespace cecon
