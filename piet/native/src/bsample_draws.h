// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// bsample_draws.h -- the bootstrap row draws on the session's Stata stream, shared by
// the `bsample` command (handlers_aer_reshape.cpp) and the `bootstrap` prefix
// (handlers_resample.cpp). Behaviour as measured on StataBE 19.5.
//
// Both routines are simple random sampling with replacement ([R] bsample), of rows
// or of whole clusters. The mapping from uniforms to rows below is what makes
// `set seed` + bsample / bootstrap reproduce StataBE's resamples draw for draw; it was
// established black-box (measured on StataBE: bs_order.log and the selftest/AER pins).
// Each routine consumes exactly 2 uniforms per unit (row or cluster) and nothing
// else -- every later draw in the session depends on that count.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "core/rng_mt64.h"

namespace piet::bsdraw {

namespace detail {

// Stata's int(runiform()*m + 1) for one uniform: the product and the sum are each
// rounded to double before the truncation (no fused multiply-add), so a product just
// below an integer that rounds up when 1 is added lands on the next target. Result
// in 1..m, except that the largest uniform (1 - 2^-53) gives m+1 when m is a power of 2.
inline int64_t stataUnitTarget(double u, int64_t m) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  volatile double scaled = u * static_cast<double>(m);
  const double shifted = scaled + 1.0;
  return static_cast<int64_t>(std::floor(shifted));
}

// m targets (1..m) followed by m sort keys, in that draw order.
inline void drawTargetsAndKeys(cecon::Mt64& g, int64_t m, std::vector<int64_t>& target,
                               std::vector<double>& key) {
  target.resize(static_cast<size_t>(m));
  key.resize(static_cast<size_t>(m));
  for (int64_t i = 0; i < m; ++i) target[i] = stataUnitTarget(g.runiform(), m);
  for (int64_t i = 0; i < m; ++i) key[i] = g.runiform();
}

}  // namespace detail

// The no-option `bsample` over n rows: 0-based input-row indices of the resample, in
// its row order; *nKept (optional) receives the number of leading distinct originals.
//
// Row i draws a target in 1..n and a key. Every occupied target is represented by the
// row in its group with the largest key (ties: the later row) and appears c times,
// c = the group's size. Order: one representative per occupied target in increasing
// target order, then the c-1 extra copies of each, again by target.
inline std::vector<int64_t> srswrFast(cecon::Mt64& g, int64_t n, int64_t* nKept = nullptr) {
  std::vector<int64_t> target;
  std::vector<double> key;
  detail::drawTargetsAndKeys(g, n, target, key);

  // Per target value (index 1..n+1, see stataUnitTarget): representative row (-1 =
  // unoccupied) and group size. A target of n+1 is one more group, ordered last.
  const int64_t maxTarget = n + 1;
  std::vector<int64_t> rep(static_cast<size_t>(maxTarget) + 1, -1);
  std::vector<int64_t> size(static_cast<size_t>(maxTarget) + 1, 0);
  for (int64_t i = 0; i < n; ++i) {
    const int64_t t = target[i];
    ++size[t];
    if (rep[t] < 0 || key[i] >= key[rep[t]]) rep[t] = i;
  }

  std::vector<int64_t> out;
  out.reserve(static_cast<size_t>(n));
  for (int64_t t = 1; t <= maxTarget; ++t)
    if (size[t] > 0) out.push_back(rep[t]);
  if (nKept) *nKept = static_cast<int64_t>(out.size());
  for (int64_t t = 1; t <= maxTarget; ++t)
    for (int64_t c = 1; c < size[t]; ++c) out.push_back(rep[t]);
  return out;
}

// `bsample, cluster()` over rows already sorted by the cluster: clid[i] is row i's
// cluster sequence number (1..G, nondecreasing), G >= 2 the number of clusters.
// Returns 0-based input-row indices of the resample, in its row order.
//
// The 2G draws belong to the first G row positions. Ranking those positions by
// (target, key, position) gives an order sigma; the last rank of each target value
// carries that value's group size, the other ranks 0. Cluster k is taken as many times
// as the count at rank sigma(k). The rows are then laid out as sigma(1..G) followed by
// rows G+1..n, each weighted by its own cluster's multiplicity; the result lists every
// row of positive weight in that layout, then the weight-1 extra copies of each row.
inline std::vector<int64_t> clustSrswr(cecon::Mt64& g, const std::vector<int64_t>& clid,
                                       int64_t G) {
  const int64_t n = static_cast<int64_t>(clid.size());
  std::vector<int64_t> target;
  std::vector<double> key;
  detail::drawTargetsAndKeys(g, G, target, key);

  // byRank[r] = 0-based position holding 0-based rank r.
  std::vector<int64_t> byRank(static_cast<size_t>(G));
  std::iota(byRank.begin(), byRank.end(), int64_t{0});
  std::sort(byRank.begin(), byRank.end(), [&](int64_t a, int64_t b) {
    if (target[a] != target[b]) return target[a] < target[b];
    if (key[a] != key[b]) return key[a] < key[b];
    return a < b;
  });

  // rankCount[r]: group size at the last rank of each target run, 0 elsewhere.
  std::vector<int64_t> rankCount(static_cast<size_t>(G), 0);
  for (int64_t r = 0, runStart = 0; r < G; ++r) {
    const bool lastOfRun = (r + 1 == G) || target[byRank[r + 1]] != target[byRank[r]];
    if (lastOfRun) {
      rankCount[r] = r - runStart + 1;
      runStart = r + 1;
    }
  }

  // Multiplicity of cluster k (1..G): the count at the rank numbered sigma(k), where
  // sigma(k) is the (1-based) position at rank k -- i.e. 0-based rank byRank[k-1].
  std::vector<int64_t> mult(static_cast<size_t>(G) + 1, 0);
  for (int64_t k = 1; k <= G; ++k) mult[k] = rankCount[byRank[k - 1]];

  std::vector<int64_t> layout;
  layout.reserve(static_cast<size_t>(n));
  for (int64_t r = 0; r < G; ++r) layout.push_back(byRank[r]);
  for (int64_t i = G; i < n; ++i) layout.push_back(i);

  std::vector<int64_t> out;
  int64_t total = 0;
  for (int64_t row : layout) total += mult[clid[row]];
  out.reserve(static_cast<size_t>(total));
  for (int64_t row : layout)
    if (mult[clid[row]] > 0) out.push_back(row);
  for (int64_t row : layout)
    for (int64_t c = 1; c < mult[clid[row]]; ++c) out.push_back(row);
  return out;
}

}  // namespace piet::bsdraw
