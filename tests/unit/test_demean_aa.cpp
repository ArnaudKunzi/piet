// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Validates the Anderson-accelerated demean path (fe.size() >= 3) two ways:
//   (1) the output is correctly within-transformed: every FE group mean ~ 0
//       (the within transform is unique, so this certifies correctness);
//   (2) it agrees element-wise with an INDEPENDENT plain alternating-projections
//       reference implemented here — i.e. Anderson reaches the same fixed point.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/model.h"
#include "hdfe/demean.h"

using namespace cecon;

static int g_fail = 0;

static Factor mkf(std::vector<int32_t> codes, int K) {
  Factor f;
  f.codes = std::move(codes);
  f.n_levels = K;
  f.count.assign(K, 0.0);
  for (int32_t c : f.codes) f.count[c] += 1.0;
  return f;
}

// Worst absolute group mean of `v` over all FE in `fe`.
static double worst_group_mean(const std::vector<double>& v, const std::vector<Factor>& fe) {
  double worst = 0;
  for (const Factor& f : fe) {
    std::vector<double> s(f.n_levels, 0.0);
    for (size_t i = 0; i < v.size(); ++i) s[f.codes[i]] += v[i];
    for (int l = 0; l < f.n_levels; ++l)
      worst = std::max(worst, std::fabs(s[l] / f.count[l]));
  }
  return worst;
}

// Independent reference: plain Gauss-Seidel alternating projections to tight tol.
static std::vector<double> ref_demean(std::vector<double> v, const std::vector<Factor>& fe) {
  const int64_t n = (int64_t)v.size();
  for (int it = 0; it < 200000; ++it) {
    double worst = 0;
    for (const Factor& f : fe) {
      std::vector<double> s(f.n_levels, 0.0);
      for (int64_t i = 0; i < n; ++i) s[f.codes[i]] += v[i];
      for (int l = 0; l < f.n_levels; ++l) s[l] /= f.count[l];
      for (int64_t i = 0; i < n; ++i) v[i] -= s[f.codes[i]];
      for (int l = 0; l < f.n_levels; ++l) worst = std::max(worst, std::fabs(s[l]));
    }
    if (worst < 1e-14) break;
  }
  return v;
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static double urand() {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (double)(rng >> 11) / (double)(1ULL << 53);
}

static void run_case(const char* name, int64_t n, std::vector<Factor> fe) {
  // y = sum of FE effects + noise
  std::vector<double> eff_a;
  std::vector<std::vector<double>> effs;
  for (const Factor& f : fe) {
    std::vector<double> e(f.n_levels);
    for (auto& x : e) x = 10.0 * (urand() - 0.5);
    effs.push_back(std::move(e));
  }
  std::vector<double> y(n);
  for (int64_t i = 0; i < n; ++i) {
    double v = 2.0 * (urand() - 0.5);
    for (size_t fi = 0; fi < fe.size(); ++fi) v += effs[fi][fe[fi].codes[i]];
    y[i] = v;
  }

  std::vector<double> ref = ref_demean(y, fe);
  std::vector<double> aa = y;
  DemeanReport rep = demean(aa.data(), n, 1, fe);  // triggers Anderson for 3+ FE

  double gm = worst_group_mean(aa, fe);
  double maxdiff = 0;
  for (int64_t i = 0; i < n; ++i) maxdiff = std::max(maxdiff, std::fabs(aa[i] - ref[i]));

  bool ok = (gm < 1e-8) && (maxdiff < 1e-7) && rep.converged;
  std::printf("  %-18s iters=%-4d group_mean=%.3e vs_ref=%.3e conv=%d  %s\n", name, rep.iters,
              gm, maxdiff, (int)rep.converged, ok ? "ok" : "FAIL");
  if (!ok) ++g_fail;
}

int test_demean_aa_main() {
  // Force the Anderson path so this test exercises it even on well-conditioned
  // data where the adaptive heuristic would otherwise stay on plain MAP.
#if defined(_WIN32)
  // No setenv in the MSVCRT; _putenv_s is the overwrite-always equivalent.
  _putenv_s("CECON_FORCE_AA", "1");
#else
  setenv("CECON_FORCE_AA", "1", 1);
#endif
  std::printf("test_demean_aa:\n");
  const int64_t n = 6000;

  // 3 crossed FE
  {
    std::vector<int32_t> a(n), b(n), c(n);
    for (int64_t i = 0; i < n; ++i) {
      a[i] = (int32_t)(i % 53);
      b[i] = (int32_t)((i / 7) % 31);
      c[i] = (int32_t)((i * 3 + 1) % 11);
    }
    run_case("3FE", n, {mkf(a, 53), mkf(b, 31), mkf(c, 11)});
  }
  // 6 crossed FE — the field-report pathology
  {
    std::vector<int32_t> a(n), b(n), c(n), d(n), e(n), g(n);
    for (int64_t i = 0; i < n; ++i) {
      a[i] = (int32_t)(i % 47);
      b[i] = (int32_t)((i / 3) % 23);
      c[i] = (int32_t)((i / 5) % 13);
      d[i] = (int32_t)((i * 7 + 2) % 17);
      e[i] = (int32_t)((i * 2 + 5) % 19);
      g[i] = (int32_t)((i / 11) % 7);
    }
    run_case("6FE", n, {mkf(a, 47), mkf(b, 23), mkf(c, 13), mkf(d, 17), mkf(e, 19), mkf(g, 7)});
  }

  std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
