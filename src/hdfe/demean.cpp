// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "hdfe/demean.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

#include "core/linalg.h"
#include "core/threads.h"

namespace cecon {

namespace {

using ParFn = std::function<void(int64_t, int64_t, int)>;
using Par = std::function<void(int64_t, const ParFn&)>;

// One Gauss-Seidel sweep of all FE projections over a single column `col`
// (length n), in place. Returns the worst relative group-mean adjustment
// (max over FE of max_level |mean| / scale).
//
// DETERMINISM: the group sums are reduced over a FIXED number of lanes
// (`nlanes`, a function of n only — see demean()), NOT over the runtime thread
// count, and combined in lane order. So the floating-point reduction order — and
// thus the result to the last bit — is identical regardless of how many workers
// run, satisfying the "same input ⇒ same output" contract across machines.
// `partials` is scratch of size nlanes*maxK; for each FE its first K slots are
// reused to hold the group means.
double sweep_once(double* col, int64_t n, const std::vector<Factor>& fe, const double* w,
                  const std::vector<std::vector<double>>& wsum, double scale, int nlanes,
                  double* partials, int maxK, const Par& par) {
  double worst = 0.0;
  for (size_t fi = 0; fi < fe.size(); ++fi) {
    const Factor& f = fe[fi];
    const int K = f.n_levels;
    const int32_t* codes = f.codes.data();
    const double* denom = w ? wsum[fi].data() : f.count.data();

    // phase 1 — per-lane partial group sums (lane l owns obs [l*n/nlanes, (l+1)*n/nlanes))
    par(nlanes, [&](int64_t l0, int64_t l1, int) {
      for (int64_t l = l0; l < l1; ++l) {
        double* p = partials + static_cast<size_t>(l) * maxK;
        std::fill(p, p + K, 0.0);
        int64_t a = l * n / nlanes, b = (l + 1) * n / nlanes;
        if (w)
          for (int64_t i = a; i < b; ++i) p[codes[i]] += w[i] * col[i];
        else
          for (int64_t i = a; i < b; ++i) p[codes[i]] += col[i];
      }
    });

    // phase 2 — reduce lane partials -> group means (fixed lane order)
    double cmax = 0.0;
    for (int l = 0; l < K; ++l) {
      double s = 0.0;
      for (int t = 0; t < nlanes; ++t) s += partials[static_cast<size_t>(t) * maxK + l];
      double mean = s / denom[l];
      partials[l] = mean;  // safe: lane 0's slot l is no longer needed once summed
      cmax = std::max(cmax, std::fabs(mean));
    }

    // phase 3 — subtract group means (element-wise; order-independent)
    const double* means = partials;
    par(n, [&](int64_t lo, int64_t hi, int) {
      for (int64_t i = lo; i < hi; ++i) col[i] -= means[codes[i]];
    });

    worst = std::max(worst, cmax / scale);
  }
  return worst;
}

// Number of reduction lanes — a deterministic function of n (and maxK only via a
// memory cap), independent of the worker count. n <= LANE_TARGET ⇒ 1 lane (pure
// sequential sum, bit-identical to the historical single-thread path).
int reduction_lanes(int64_t n, int maxK) {
  constexpr int64_t LANE_TARGET = 16384;     // ~obs per lane
  constexpr int64_t MAX_LANES = 1024;
  constexpr int64_t PARTIAL_BUDGET = 8000000;  // doubles (~64 MB) for nlanes*maxK
  int64_t want = (n + LANE_TARGET - 1) / LANE_TARGET;
  int64_t memcap = std::max<int64_t>(1, PARTIAL_BUDGET / std::max(1, maxK));
  int64_t nl = std::min(want, std::min(MAX_LANES, memcap));
  return static_cast<int>(std::max<int64_t>(1, nl));
}

constexpr int AA_DEPTH = 5;  // Anderson history depth

}  // namespace

DemeanReport demean(double* M, int64_t n, int m, const std::vector<Factor>& fe, const double* w,
                    double tol, int maxiter, bool serial) {
  DemeanReport rep;
  if (fe.empty() || n == 0 || m == 0) return rep;

  int maxK = 0;
  for (const Factor& f : fe) maxK = std::max(maxK, f.n_levels);
  const bool single = (fe.size() == 1);
  // Anderson-accelerate 3+ FE. CECON_NO_AA=1 forces plain alternating projections
  // (escape hatch + benchmark control).
  static const bool no_aa = [] {
    const char* e = std::getenv("CECON_NO_AA");
    return e && e[0] && e[0] != '0';
  }();
  // CECON_FORCE_AA=1 engages Anderson immediately (skip the plain-MAP probe) —
  // for tests and known-hard problems.
  static const bool force_aa = [] {
    const char* e = std::getenv("CECON_FORCE_AA");
    return e && e[0] && e[0] != '0';
  }();
  // Allow Anderson for 2+ FE; the adaptive probe below only engages it when plain
  // alternating projections is mixing slowly (e.g. correlated worker-firm FE), so
  // well-conditioned cases pay no overhead.
  const bool use_aa = (fe.size() >= 2) && !no_aa;

  ThreadPool& pool = ThreadPool::global();
  const int nthreads = serial ? 1 : pool.size();
  Par par = [&](int64_t N, const ParFn& body) {
    if (nthreads <= 1)
      body(0, N, 0);
    else
      pool.run(N, body);
  };

  // weighted per-FE denominators (constant across the demean)
  std::vector<std::vector<double>> wsum;
  if (w) {
    wsum.resize(fe.size());
    for (size_t fi = 0; fi < fe.size(); ++fi) {
      wsum[fi].assign(fe[fi].n_levels, 0.0);
      const int32_t* codes = fe[fi].codes.data();
      for (int64_t i = 0; i < n; ++i) wsum[fi][codes[i]] += w[i];
    }
  }

  // Fixed lane count for deterministic reductions (independent of nthreads).
  const int nlanes = reduction_lanes(n, maxK);
  std::vector<double> partials(static_cast<size_t>(nlanes) * maxK);

  // Anderson scratch — allocated lazily on first engagement so the common
  // (fast-converging) case allocates nothing.
  std::vector<double> x_saved, f_cur, g_prev, f_prev, dG, dF;
  bool aa_alloc = false;
  auto ensure_aa = [&] {
    if (aa_alloc) return;
    x_saved.resize(n);
    f_cur.resize(n);
    g_prev.resize(n);
    f_prev.resize(n);
    dG.resize(static_cast<size_t>(AA_DEPTH) * n);
    dF.resize(static_cast<size_t>(AA_DEPTH) * n);
    aa_alloc = true;
  };

  int max_iters = 0;
  double max_resid = 0.0;
  bool all_conv = true;

  for (int c = 0; c < m; ++c) {
    double* col = M + static_cast<size_t>(c) * n;

    double ss = 0;
    for (int64_t i = 0; i < n; ++i) ss += col[i] * col[i];
    double scale = std::sqrt(ss / static_cast<double>(n));
    if (scale <= 0) scale = 1.0;

    int it = 0;
    double worst = 0.0;

    // Phase 1 — plain alternating projections with a convergence-rate probe.
    // Cheap and optimal when the FE are well-conditioned. If progress is slow
    // (residual shrinks by less than ~2.5x per sweep) we hand off to Anderson,
    // whose per-sweep overhead only pays off on hard, slowly-mixing problems.
    bool engage = false;
    double prev = 0.0;
    for (; it < maxiter; ++it) {
      worst = sweep_once(col, n, fe, w, wsum, scale, nlanes, partials.data(), maxK, par);
      if (single) { ++it; break; }
      if (worst < tol) { ++it; break; }
      if (use_aa && (force_aa || (it >= 2 && prev > 0 && worst > prev * 0.40))) {
        engage = true;
        ++it;
        break;
      }
      prev = worst;
    }

    // Phase 2 — Anderson acceleration (type-II, restarted, depth AA_DEPTH),
    // continuing from the current iterate. G = one full sweep; residual
    // f = G(x) - x; we drive ||f|| -> 0.
    if (engage) {
      ensure_aa();
      int hist = 0, start = 0;
      bool first = true;
      double gamma[AA_DEPTH], Amat[AA_DEPTH * AA_DEPTH], bvec[AA_DEPTH];
      int idx[AA_DEPTH];
      for (; it < maxiter; ++it) {
        par(n, [&](int64_t lo, int64_t hi, int) {
          for (int64_t i = lo; i < hi; ++i) x_saved[i] = col[i];  // x_k
        });
        worst = sweep_once(col, n, fe, w, wsum, scale, nlanes, partials.data(), maxK, par);
        par(n, [&](int64_t lo, int64_t hi, int) {
          for (int64_t i = lo; i < hi; ++i) f_cur[i] = col[i] - x_saved[i];  // f_k = g_k - x_k
        });
        if (worst < tol) { ++it; break; }

        if (first) {
          first = false;
          // plain step (col already holds g); seed history anchors
          par(n, [&](int64_t lo, int64_t hi, int) {
            for (int64_t i = lo; i < hi; ++i) { g_prev[i] = col[i]; f_prev[i] = f_cur[i]; }
          });
          continue;
        }

        // push deltas: dg = g_k - g_prev, df = f_k - f_prev
        int slot;
        if (hist == AA_DEPTH) { slot = start; start = (start + 1) % AA_DEPTH; }
        else { slot = (start + hist) % AA_DEPTH; ++hist; }
        {
          double* dg = dG.data() + static_cast<size_t>(slot) * n;
          double* df = dF.data() + static_cast<size_t>(slot) * n;
          par(n, [&](int64_t lo, int64_t hi, int) {
            for (int64_t i = lo; i < hi; ++i) {
              dg[i] = col[i] - g_prev[i];
              df[i] = f_cur[i] - f_prev[i];
            }
          });
        }
        // anchors advance to g_k, f_k (before col is overwritten)
        par(n, [&](int64_t lo, int64_t hi, int) {
          for (int64_t i = lo; i < hi; ++i) { g_prev[i] = col[i]; f_prev[i] = f_cur[i]; }
        });

        const int H = hist;
        for (int j = 0; j < H; ++j) idx[j] = (start + j) % AA_DEPTH;

        // A = dF' dF (H x H, lower tri), b = dF' f_cur — per-lane reduce (same
        // fixed lane decomposition as sweep_once → thread-count-independent).
        const int RW = H * H + H;
        std::vector<double> red(static_cast<size_t>(nlanes) * RW, 0.0);
        par(nlanes, [&](int64_t l0, int64_t l1, int) {
          for (int64_t l = l0; l < l1; ++l) {
            double* r = red.data() + static_cast<size_t>(l) * RW;
            double* rb = r + H * H;
            int64_t i0 = l * n / nlanes, i1 = (l + 1) * n / nlanes;
            for (int64_t i = i0; i < i1; ++i) {
              double fi_ = f_cur[i];
              for (int a = 0; a < H; ++a) {
                double fa = dF[static_cast<size_t>(idx[a]) * n + i];
                rb[a] += fa * fi_;
                for (int b = 0; b <= a; ++b) r[a * H + b] += fa * dF[static_cast<size_t>(idx[b]) * n + i];
              }
            }
          }
        });
        for (int a = 0; a < H; ++a) {
          double bb = 0;
          for (int t = 0; t < nlanes; ++t) bb += red[static_cast<size_t>(t) * RW + H * H + a];
          bvec[a] = bb;
          for (int b = 0; b <= a; ++b) {
            double s = 0;
            for (int t = 0; t < nlanes; ++t) s += red[static_cast<size_t>(t) * RW + a * H + b];
            Amat[a * H + b] = s;
            Amat[b * H + a] = s;
          }
        }
        // Tikhonov regularize, then solve A gamma = b
        double tr = 0;
        for (int a = 0; a < H; ++a) tr += Amat[a * H + a];
        double reg = (tr > 0 ? 1e-12 * tr / H : 1e-30);
        for (int a = 0; a < H; ++a) Amat[a * H + a] += reg;

        bool ok = (la::spd_inverse(Amat, H) == 0);
        bool fin = ok;
        if (ok) {
          for (int a = 0; a < H; ++a) {
            double s = 0;
            for (int b = 0; b < H; ++b) s += Amat[a * H + b] * bvec[b];
            gamma[a] = s;
            if (!std::isfinite(s)) fin = false;
          }
        }
        if (fin) {
          // x_{k+1} = g_k - sum_a gamma_a * dG_a   (col currently = g_k)
          par(n, [&](int64_t lo, int64_t hi, int) {
            for (int64_t i = lo; i < hi; ++i) {
              double acc = 0;
              for (int a = 0; a < H; ++a) acc += gamma[a] * dG[static_cast<size_t>(idx[a]) * n + i];
              col[i] -= acc;
            }
          });
        }
        // else: fall back to the plain step (col stays = g_k)
      }
    }

    max_iters = std::max(max_iters, it);
    max_resid = std::max(max_resid, worst);
    if (!(single || worst < tol)) all_conv = false;
  }

  rep.iters = max_iters;
  rep.resid = max_resid;
  rep.converged = all_conv;
  return rep;
}

}  // namespace cecon
