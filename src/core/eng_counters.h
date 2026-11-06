// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#ifndef CECON_CORE_ENG_COUNTERS_H
#define CECON_CORE_ENG_COUNTERS_H

#include <cstdint>

// ENGINE-SIDE MECHANISM COUNTERS (P2PERF-WAVE-PLAN §W0).
//
// WHY: the Doctrine's gate ② says deterministic counters are the sworn witness
// and timing the noisy one, but three engine mechanisms this campaign optimises
// had NO counter at all, so every claim about them rested on reading the code by
// hand:
//   * write plane   — whether a save took patch_hbin (CoW clone + header rewrite)
//                     or write_hbin_full (a complete re-serialize). This is the
//                     single number the rename work is judged by.
//   * provenance    — how often hash_file_hex actually ran (it is memoised and
//                     deferred; "it is cheap now" was an unmeasured claim).
//   * dataset opens — Dataset already tallies full-parse vs carried opens, but
//                     nothing ever emitted them, so the tallies were unreadable
//                     from outside the process.
//
// CONVENTION — the same one PIET's counters.h follows and for the same reasons:
// process-wide statics, no locking (serve is a synchronous single-threaded
// loop), ALWAYS accumulated, emission opt-in. The accumulate side is one integer
// increment on paths that are already doing file-sized work, so there is no
// "off" build to keep honest; what gates the numbers is the reader — the serve
// `{"frame":{"op":"counters"}}` request, which nothing but an instrumented run
// ever sends. With no reader, stdout is byte-identical to a build without this
// header, which is the freeze-fixture safety property.
namespace cecon {
namespace engctr {

inline int64_t g_patchHits = 0;    // write_hbin took patch_hbin AND it succeeded
inline int64_t g_patchFulls = 0;   // write_hbin_full ran (patch declined/failed/ineligible)
inline int64_t g_patchFallbacks = 0;  // of g_patchFulls: patch was TRIED first and returned false
inline int64_t g_hashFileCalls = 0;   // hash_file_hex computed a hash (memo miss)
inline int64_t g_hashFileMemoHits = 0;  // hash_file_hex answered from the memo/root-hash bridge

// S06/ARC-MRG (P2-PERF WS-D2). `merge` was NAMED, in the P2-PERF close, as opening
// its files three times; nothing measured it. These two make the claim falsifiable:
//
//   g_dataFileOpens  — every PHYSICAL open of a dataset file, wherever it happens:
//                      map_hbin (.hbin mmap), read_dta (.dta), Frame::from_csv
//                      (.csv/.tsv). One increment per open, at the open itself, so
//                      it cannot drift from what the process actually did.
//   g_mergeFileOpens — the delta of the above across ONE cmd_merge invocation. A
//                      scoped delta rather than its own increments, so it counts
//                      opens performed by code cmd_merge never reads (keepif
//                      scratch, the writer's read-back) exactly as it counts its
//                      own. TARGET = 2: the master and the using file, once each.
inline int64_t g_dataFileOpens = 0;
inline int64_t g_mergeFileOpens = 0;

// COBRA-DD-ACCUM (TOLERANCE §5). Appended LAST, and emitted last by the
// `counters` frame op, so key order stays append-only.
//
//   g_ddAccumUsed — one increment per compensated-accumulation kernel CALL
//                   (la::xtx / xtv / residual / dot / sumsq / sum). This is a
//                   mechanism witness, not a cost meter: it answers "did this
//                   estimation actually go through the double-double path, or
//                   did somebody quietly reintroduce a plain BLAS accumulation
//                   underneath it". A ladder-GREEN run with dd_accum_used == 0
//                   is a gate that proved nothing — the cache-invalidates-test-
//                   proxy trap, one layer down.
inline int64_t g_ddAccumUsed = 0;

// MECHOPT W1 (2026-08-13): the serve session's cached Dataset view. rebuild =
// as_dataset_view(), which deep-copies every factor level dictionary (frame.cpp
// as_dataset_view, the levels `.ref()` materialize) — on the acceptance corpus
// that is 11 factor columns / 135,603 strings per call, and it fires on every
// committed frame run, every frame use, and every unstash. patch = the
// incremental path (only changed/added/dropped columns rewritten). These two
// are the first-class witness for the PATCH_VIEW work: the win must show as
// patches >> rebuilds with request counts UNCHANGED — a per-call-work witness,
// not a fewer-calls witness.
inline int64_t g_viewRebuilds = 0;
inline int64_t g_viewPatches = 0;

inline void addPatchHit() { ++g_patchHits; }
inline void addPatchFull() { ++g_patchFulls; }
inline void addPatchFallback() { ++g_patchFallbacks; }
inline void addHashFileCall() { ++g_hashFileCalls; }
inline void addHashFileMemoHit() { ++g_hashFileMemoHits; }
inline void addDataFileOpen() { ++g_dataFileOpens; }
inline void setMergeFileOpens(int64_t n) { g_mergeFileOpens = n; }
inline void addDdAccum() { ++g_ddAccumUsed; }
inline void addViewRebuild() { ++g_viewRebuilds; }
inline void addViewPatch() { ++g_viewPatches; }

inline int64_t patchHits() { return g_patchHits; }
inline int64_t patchFulls() { return g_patchFulls; }
inline int64_t patchFallbacks() { return g_patchFallbacks; }
inline int64_t hashFileCalls() { return g_hashFileCalls; }
inline int64_t hashFileMemoHits() { return g_hashFileMemoHits; }
inline int64_t dataFileOpens() { return g_dataFileOpens; }
inline int64_t mergeFileOpens() { return g_mergeFileOpens; }
inline int64_t ddAccumUsed() { return g_ddAccumUsed; }
inline int64_t viewRebuilds() { return g_viewRebuilds; }
inline int64_t viewPatches() { return g_viewPatches; }

inline void resetAll() {
  g_patchHits = 0;
  g_patchFulls = 0;
  g_patchFallbacks = 0;
  g_hashFileCalls = 0;
  g_hashFileMemoHits = 0;
  g_dataFileOpens = 0;
  g_mergeFileOpens = 0;
  g_ddAccumUsed = 0;
  g_viewRebuilds = 0;
  g_viewPatches = 0;
}

}  // namespace engctr
}  // namespace cecon

#endif  // CECON_CORE_ENG_COUNTERS_H
