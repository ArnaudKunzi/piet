// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// varstats.js — B14(a): passive per-variable summary-stats cache.
//
// Populated by console.js's appendBlock() whenever a `summarize` block is
// rendered — manual (user typed `summarize`) OR auto-dispatched (B14(b)'s
// "Auto-summarize on load" setting) both funnel through that ONE render
// path, so a single hook there catches both. Read by panels.js's variable
// detail card (B13) to show quick-stats without a new bridge/native route.
//
// A tiny module of its own (rather than living inside console.js or
// panels.js) so the two can share it without importing each other.

const _cache = new Map();   // varname -> { N, mean, sd, min, max, cachedAt }

/**
 * Cache mean/sd/min/max/N per variable from a rendered `summarize` block.
 * Reads the SAME shape console.js's formatSummarize() renders from:
 * `block.stats = { varname: { N, mean, sd, min, max }, ... }`. No-ops on a
 * malformed/missing `stats` field instead of throwing — a cache miss just
 * means the detail card falls back to its "run summarize" hint.
 */
export function cacheSummarizeStats(block) {
  const stats = block && block.stats;
  if (!stats || typeof stats !== 'object') return;
  const cachedAt = Date.now();
  for (const [name, s] of Object.entries(stats)) {
    if (!s) continue;
    _cache.set(name, { N: s.N, mean: s.mean, sd: s.sd, min: s.min, max: s.max, cachedAt });
  }
}

/** Cached stats for one variable, or null if it's never been summarized
 *  (directly or via `summarize` covering it) this session. */
export function getVarStats(name) {
  return _cache.get(name) ?? null;
}

/** Test/reset hook — not wired to any UI action this stint. */
export function clearVarStatsCache() {
  _cache.clear();
}
