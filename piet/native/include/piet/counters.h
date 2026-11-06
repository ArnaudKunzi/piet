// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#ifndef PIET_NATIVE_COUNTERS_H
#define PIET_NATIVE_COUNTERS_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

// FLOOR metric ① (A1-METRICS-RATIFICATION.md §1.1/§1.5/§7) — the byte half of
// the PIET_EMIT_COUNTERS seam (main.cpp, runDoFileHeadless).
//
// WHY BYTES AND NOT A TIMER: `amp(c) = touched_bytes(c) / semantic_bytes(c)` is
// dimensionless, so it is the one gate in the FLOOR set that is machine-free —
// it reads the same on this box and on a porting target, and it sees at any row
// count what a wall profile on a small slice is blind to (the exact trap
// PHASE0-reaccounting fell into: gen reads "0.8 ms/call" on a small-dictionary
// slice while evalRows is 11 % + map 31 % on the full corpus). Deterministic
// integers ⇒ RED-eligible per-merge, which COMMS §4 item 11 mandates for an
// optimization claim and which a wall/RSS budget can never be.
//
// CONVENTION — deliberately the SAME one the four landed counters already
// follow (Datastore::constructCount_, SessionState::cardFoldCount_ /
// residentLiveCount_, CobraSession::postestHitCount_):
//   * process-wide statics, no locking. The --do runner is synchronous and
//     single-threaded; every other mechanism counter in this codebase makes
//     the same assumption, and none of them is read by non-test code.
//   * ALWAYS accumulated, emission env-gated. The accumulate side is a handful
//     of integer adds on paths that are already moving the bytes being counted,
//     so there is no "off" build to keep honest; what $PIET_EMIT_COUNTERS gates
//     is the stdout key. With the env var absent, --do stdout is byte-identical
//     to a build without this header — the freeze-fixture safety property.
//
// WHAT COUNTS AS "TOUCHED": payload bytes the resident plane actually MOVES for
// a command — a column read, a per-row rebind, a frame written to disk, an IPC
// payload handed to the engine. Not allocations, not mmap reservations: a byte
// is touched when this process reads it or writes it. Each call site below
// names which of the four site classes (columnar / map-rebind / snapshot / IPC)
// it belongs to, because the gate's number is only as honest as that mapping.
namespace piet {
namespace counters {

// ---- the accumulators (see the four add* helpers for the site classes) -----
inline int64_t g_touchedBytes = 0;
inline int64_t g_snapshotCount = 0;
// C4-L1 (C4-RESIDENT-READ-SCOPE-brief §6): the byte half of the SNAPSHOT site
// class, split out of g_touchedBytes so the snapshot/rebuild share of metric ①
// is a measured number instead of the §2.2 upper bound (95 × peak-frame). Same
// site as g_snapshotCount — addSnapshotBytes accumulates both.
inline int64_t g_snapshotBytes = 0;
inline int64_t g_setcolBytes = 0;
// PP-L1 (PINGPONG-SCOPE-brief §5) — use-plane split counters, report-only.
// They close the brief's §1 contradiction (GATE23-P2 §4's "6 disk reloads per
// depvar" vs §2's engine-call log showing import-dta ×1): how many `use`
// dispatches actually paid the disk path vs restored from the worker's
// virtual-file table, and how many paid a full .dta→hbin conversion. All three
// are counts of the runUse branch taken (handlers.cpp), not bytes.
inline int64_t g_useDiskLoads = 0;    // disk-open path taken (any format)
inline int64_t g_useVirtualHits = 0;  // virtual-save table restore (loadFromVirtual)
inline int64_t g_dtaConverts = 0;     // import-dta full conversions invoked
// PP-L2B / K1b (PINGPONG-SCOPE-brief §2 K1b) — the resident-stash cache HIT:
// a re-`use` of an unchanged source that restored the worker's stashed frame
// (frameUnstash, O(columns) RAM) instead of paying the disk-open path. Each hit
// is a use_disk_loads that DID NOT happen, so the win reads as use_disk_loads
// falling toward 1 while this climbs — the two are complementary tallies of the
// same `use` dispatches.
inline int64_t g_useStashHits = 0;    // resident-stash restore (useStashRestore)
// SELF-RELOAD counter (report-only). Definition, deliberately narrow so it can
// never be argued about: a `use` that took the DISK-LOAD path on a file THIS
// SESSION ITSELF WROTE with `save`. Those bytes were the session's own frame
// moments earlier, so re-materializing them off disk is work the process is
// doing twice by construction — not an estimate of avoidable work, an instance
// of it.
//
// It was introduced as B5-③'s acceptance witness (a memo that made the next
// `use` of a just-saved file a 0-reload restore drove it 12 → 0 on a save→use
// loop). That memo was measured at −0.2% wall and REVERTED (2026-08-06 maintainer
// adjudication, precedent A4): a native .hbin open is an mmap + header parse
// with no whole-file hash, so the work it removed was already cheap. The
// counter outlived the rung on its own merit — "how often does a session
// re-read its own output" is a real question about the IO plane, and on the
// .dta save→use shape (still a full reload, deliberately: the export/import
// round trip's fixed point is unproven) it is the number that prices it.
inline int64_t g_datastoreReloads = 0;
// FLOOR metric ② DENOMINATOR, running high-water max (B2 NAMED-OWED ③ ·
// PHASE-B-DECISION-MEMO §6-3 ADDENDUM, ratified 2026-07-23 = peak-vs-peak). The
// running maximum of SessionState::liveNamedBytes() over a run's top-level items
// (updated from the do-executor loop via liveNamedBytesPeakSample(), which is
// counter- and cache-neutral). The END snapshot (`live_named_bytes`) under-counts
// a corpus that drops its frame before exit (a2's `exit, clear`) while peak RSS
// occurred while the frame was resident — so the FAITHFUL ② denominator is this
// peak, matching the numerator's own "peak over a run" framing (A1 §2.1). Direction
// `max`; report-only (NO floors.json budget — Eval pen).
inline int64_t g_liveNamedBytesPeak = 0;

// ---- W0 ENGINE ROUND-TRIP LEDGER (P2PERF-WAVE-PLAN §W0) -------------------
// The campaign opened on the belief that by-group work paid a PROCESS SPAWN per
// group. It does not: a persistent `cobra serve` worker is the default plane and
// fork/exec is the fallback. What the loops really pay is one ROUND TRIP per
// group — and a round trip's cost depends entirely on which shape it is: a
// stateless {argv} re-reads --data from disk and writes --out, while a frame-run
// touches neither. Counting "engine calls" as one number would have hidden that
// difference, which is the whole subject of arcs B and C, so the ledger is split
// by request shape. `one_shot_spawns` is the honest witness for the fallback
// plane: it is 0 on a healthy session, and a nonzero value means the worker died
// or PIET_NO_SERVE is set — either way, any timing taken beside it is a
// different experiment.
inline int64_t g_engineReqStateless = 0;  // {"argv":[...]}  — disk in, disk out
inline int64_t g_engineReqFrameUse = 0;   // frame use        — adopt a file as the resident frame
inline int64_t g_engineReqFrameRun = 0;   // frame run        — one verb, no file touched
inline int64_t g_engineReqSnapshot = 0;   // frame snapshot   — the virtual head becoming a file
inline int64_t g_engineReqPostest = 0;    // frame postest    — handle-addressed, no refit
inline int64_t g_engineReqOther = 0;      // stash/unstash/save_virtual/drop/counters
inline int64_t g_oneShotSpawns = 0;       // real fork+exec of the engine binary
inline int64_t g_serveSpawns = 0;         // `cobra serve` worker spawns (1 per healthy session)

inline void addEngineReqStateless() { ++g_engineReqStateless; }
inline void addEngineReqFrameUse() { ++g_engineReqFrameUse; }
inline void addEngineReqFrameRun() { ++g_engineReqFrameRun; }
inline void addEngineReqSnapshot() { ++g_engineReqSnapshot; }
inline void addEngineReqPostest() { ++g_engineReqPostest; }
inline void addEngineReqOther() { ++g_engineReqOther; }
inline void addOneShotSpawn() { ++g_oneShotSpawns; }
inline void addServeSpawn() { ++g_serveSpawns; }

inline int64_t engineReqStateless() { return g_engineReqStateless; }
inline int64_t engineReqFrameUse() { return g_engineReqFrameUse; }
inline int64_t engineReqFrameRun() { return g_engineReqFrameRun; }
inline int64_t engineReqSnapshot() { return g_engineReqSnapshot; }
inline int64_t engineReqPostest() { return g_engineReqPostest; }
inline int64_t engineReqOther() { return g_engineReqOther; }
inline int64_t engineReqTotal() {
  return g_engineReqStateless + g_engineReqFrameUse + g_engineReqFrameRun +
         g_engineReqSnapshot + g_engineReqPostest + g_engineReqOther;
}
inline int64_t oneShotSpawns() { return g_oneShotSpawns; }
inline int64_t serveSpawns() { return g_serveSpawns; }

// Site class 1 — COLUMNAR: a dataset opened / a column materialised from the
// hbin plane. Billed at the bytes the open actually brings in.
inline void addColumnarBytes(int64_t n) {
  if (n > 0) g_touchedBytes += n;
}

// Site class 2 — MAP-REBIND: the per-row namespace rebind in evalRows. This is
// the amplification defect itself (re-binding every column per row instead of
// the referenced ones), so it must be billed per CELL, at the width the
// implementation really moves: 8 B for an f64 cell, and for a factor cell the
// 4 B code PLUS the level string it materialises — billing a factor at a flat
// 8 B would understate precisely the waste ① exists to catch (A1 §1.2).
inline void addRebindBytes(int64_t n) {
  if (n > 0) g_touchedBytes += n;
}

// Site class 3 — SNAPSHOT: the resident frame flushed to its token path. Per
// op this is amp ≡ 1 (copying the frame IS the snapshot's semantic work), so
// the waste ① must catch here is the COUNT, not the ratio — hence the separate
// g_snapshotCount, direction `max`, living in the counter half (A1 §1.3).
inline void addSnapshotBytes(int64_t n) {
  if (n > 0) {
    g_touchedBytes += n;
    g_snapshotBytes += n;  // C4-L1: the SNAPSHOT class's own byte ledger
  }
  ++g_snapshotCount;
}

// PP-L1 use-plane branch tallies (see the three accumulators above). Counts,
// not bytes: the question they answer is WHICH runUse branch ran, the byte
// cost of the disk branch being already billed by the COLUMNAR/IPC classes.
inline void addUseDiskLoad() { ++g_useDiskLoads; }
inline void addUseVirtualHit() { ++g_useVirtualHits; }
inline void addDtaConvert() { ++g_dtaConverts; }
inline void addUseStashHit() { ++g_useStashHits; }
inline void addDatastoreReload() { ++g_datastoreReloads; }  // see g_datastoreReloads

// Site class 4 — IPC: a payload serialised for / read back from a cobra
// subprocess. `setcol`-shaped verbs are classified `semantic_zero` (a ratio
// against a zero denominator is undefined, not infinite — A1 §1.3), so they
// leave metric ① and are asserted here as an ABSOLUTE byte budget instead.
inline void addIpcBytes(int64_t n, bool setcolShaped = false) {
  if (n <= 0) return;
  g_touchedBytes += n;
  if (setcolShaped) g_setcolBytes += n;
}

inline int64_t touchedBytes() { return g_touchedBytes; }
inline int64_t snapshotCount() { return g_snapshotCount; }
inline int64_t snapshotBytes() { return g_snapshotBytes; }
inline int64_t setcolBytes() { return g_setcolBytes; }
inline int64_t useDiskLoads() { return g_useDiskLoads; }
inline int64_t useVirtualHits() { return g_useVirtualHits; }
inline int64_t dtaConverts() { return g_dtaConverts; }
inline int64_t useStashHits() { return g_useStashHits; }
inline int64_t datastoreReloads() { return g_datastoreReloads; }

// Running high-water max of liveNamedBytes(); `v<=0` is a no-op so an
// early empty-session sample never floors the peak below a later real one.
inline void updateLiveNamedBytesPeak(int64_t v) {
  if (v > g_liveNamedBytesPeak) g_liveNamedBytesPeak = v;
}
inline int64_t liveNamedBytesPeak() { return g_liveNamedBytesPeak; }

// ---- T6 PIET-SIDE-SPLIT (report-only, PIET_EMIT_TIMING) -------------------
// The campaign measured the engine's share of a full analysis.do run exactly
// (a serve-proxy shim), but the ~12 remaining minutes were one undivided lump
// labelled "PIET + I/O". This splits it WITHOUT a shim: every blocking call
// into the engine adds its wall to g_engineWaitNs, and the do-executor records
// per top-level item {wall, engine-wait}. PIET-side time is then wall minus
// engine-wait, per verb, from the product's own binary.
//
// Inert unless PIET_EMIT_TIMING is set at the emit site — the accumulators
// themselves are two integer adds per engine call, and nothing reads them
// otherwise. Report-only: no budget, no gate.
inline int64_t g_engineWaitNs = 0;
inline void addEngineWaitNs(int64_t ns) { g_engineWaitNs += ns; }
inline int64_t engineWaitNs() { return g_engineWaitNs; }

struct VerbTiming {
  int64_t count = 0;
  int64_t wallNs = 0;       // total time the item occupied the executor
  int64_t engineNs = 0;     // of which: blocked on the engine
};
inline std::map<std::string, VerbTiming>& verbTimings() {
  static std::map<std::string, VerbTiming> t;
  return t;
}
inline void addVerbTiming(const std::string& verb, int64_t wallNs, int64_t engineNs) {
  VerbTiming& v = verbTimings()[verb];
  ++v.count;
  v.wallNs += wallNs;
  v.engineNs += engineNs;
}

// ---- ATTR-20260812: the two things verbTimings() cannot say ---------------
// Measured 2026-08-12 against this very build, because the comment above
// executeDoSource's ItemTimer claims something the code does not do:
//
//   "Only the TOP-LEVEL owner records ... otherwise a loop body's items would
//    be double-counted inside their header's own total."
//
// There is no such guard. `loops::executeLoop` runs the body through
// `doexec::executeDoSource` (loops.cpp:770), so every body line IS an item and
// IS billed — while the loop header's own item stays open around it. Probe
// (50,000 iterations, body 1 line vs 3 lines, everything else fixed):
//
//   body=1  forvalues n=1 wall=79.21ms   local n=50,000  wall=66.73ms
//   body=3  forvalues n=1 wall=235.39ms  local n=150,000 wall=213.86ms
//
// The header's wall tracks its body's — parents are INCLUSIVE. So the rows of
// verbTimings() sit at mixed nesting levels and MUST NOT be summed: this is
// exactly the trap p0_landings §T6 records as "capture ×23,237 = 59.8 min,
// 행 합산 금지". It is a reading trap built into the instrument.
//
// Rather than change verbTimings() (the 2026-07-24 full-run record is stated
// in its terms and stays comparable), depth is now tracked and the two levels
// are offered SEPARATELY, on higher PIET_EMIT_TIMING levels:
//
//   PIET_EMIT_TIMING=1  timing{}                — unchanged, byte for byte
//   PIET_EMIT_TIMING=2  + timing_items[]        — depth-1 items only, in order.
//                                                 These DO sum: each root item
//                                                 is counted exactly once, its
//                                                 wall inclusive of its body.
//   PIET_EMIT_TIMING=3  + timing_nested{}       — depth>=2 items, verb-summed.
//                                                 The inside-the-loop view.
//
// Levels 2/3 also key on the verb the dispatcher RESOLVED (registry canonical)
// rather than the raw leading token, so `capture noisily regress` bills to
// `regress` and abbreviations collapse onto their canonical spelling. The
// resolution is noted per depth (noteResolvedVerb) and first-one-wins, so a
// body line's resolution can never overwrite its header's slot.
// A plain global rather than a function-local static: the read sits on the
// dispatch path, and a magic static pays a guard-variable acquire load on every
// call where this pays a load and a compare. -1 = not resolved yet.
inline int g_timingLevel = -1;
inline int timingLevel() {
  if (g_timingLevel < 0) {
    const char* e = std::getenv("PIET_EMIT_TIMING");
    if (!e || !*e) {
      g_timingLevel = 0;
    } else {
      const int n = std::atoi(e);
      g_timingLevel = n > 0 ? n : 1;  // any non-numeric truthy value = old meaning
    }
  }
  return g_timingLevel;
}
// Selftest only: the env is read once per process, so a leg that wants to see
// the depth-split surfaces has no other way to arm them. Restore when done.
inline void setTimingLevelForTest(int lvl) { g_timingLevel = lvl; }

// Nesting depth of the doexec item loop. 1 = the items of the do-file the run
// was asked for; 2+ = loop bodies, program bodies, replayed `do` files.
inline int g_doexecDepth = 0;
inline int doexecDepth() { return g_doexecDepth; }

// Resolved-verb slots, indexed by depth. The dispatcher writes; the ItemTimer
// at that depth clears on entry and reads on exit.
inline std::vector<std::string>& resolvedVerbSlots() {
  static std::vector<std::string> s;
  return s;
}
inline void clearResolvedVerb(int depth) {
  auto& s = resolvedVerbSlots();
  if (depth < 0) return;
  if (static_cast<size_t>(depth) >= s.size()) s.resize(static_cast<size_t>(depth) + 1);
  s[static_cast<size_t>(depth)].clear();
}
inline void noteResolvedVerb(const std::string& verb) {
  if (timingLevel() < 2) return;  // one predictable branch when not asked for
  const int d = g_doexecDepth;
  if (d <= 0 || verb.empty()) return;
  auto& s = resolvedVerbSlots();
  if (static_cast<size_t>(d) >= s.size()) s.resize(static_cast<size_t>(d) + 1);
  if (s[static_cast<size_t>(d)].empty()) s[static_cast<size_t>(d)] = verb;  // first wins
}
inline std::string takeResolvedVerb(int depth) {
  auto& s = resolvedVerbSlots();
  if (depth < 0 || static_cast<size_t>(depth) >= s.size()) return std::string();
  return s[static_cast<size_t>(depth)];
}

struct ItemRecord {
  int line = 0;             // 1-based item index within the root do-source
  std::string verb;         // resolved canonical verb, else the raw token
  std::string text;         // the item's own text, truncated — see addItemRecord
  int64_t wallNs = 0;
  int64_t engineNs = 0;
};
inline std::vector<ItemRecord>& itemRecords() {
  static std::vector<ItemRecord> r;
  return r;
}
// `text` is carried because `line` is the ITEM index (the plane stopped_at_line
// reports on), not a file line number: a brace block is one item however many
// lines it spans, so nothing downstream can map an index back to the source
// without re-implementing doexec's own aggregation. Truncated to keep a long
// generated command from dominating the record; one short string per depth-1
// item is not a cost worth a reconstruction step later.
inline void addItemRecord(int line, const std::string& verb, const std::string& text,
                          int64_t wallNs, int64_t engineNs) {
  constexpr size_t kMaxText = 120;
  itemRecords().push_back(ItemRecord{
      line, verb, text.size() > kMaxText ? text.substr(0, kMaxText) : text, wallNs, engineNs});
}
inline std::map<std::string, VerbTiming>& nestedTimings() {
  static std::map<std::string, VerbTiming> t;
  return t;
}
inline void addNestedTiming(const std::string& verb, int64_t wallNs, int64_t engineNs) {
  VerbTiming& v = nestedTimings()[verb];
  ++v.count;
  v.wallNs += wallNs;
  v.engineNs += engineNs;
}

// ---- EXCLUSIVE (self) time — added 2026-08-12 because the corpus demanded it -
// The depth split above answers "which top-level item" and "what runs inside a
// loop", but on the acceptance corpus the first question has a degenerate
// answer: 100.0% of depth-1 wall is one user-defined program, called 49 times.
// Everything interesting is below it, and BOTH the depth-1 and the nested map
// are inclusive of their own children, so neither decomposes anything.
//
// This does. On completion an item subtracts what its children accumulated and
// bills only the remainder to its verb. Exclusive rows sum to the run — that is
// the property the inclusive views cannot have — so "where do the seconds go"
// finally has a table that adds up.
//
// bucket[d] is the wall/engine accumulated by items AT depth d, read and
// cleared by their parent at depth d-1. A depth is entered and left in strict
// LIFO order by one thread, so a plain vector is the whole mechanism.
inline std::vector<int64_t>& childWallNs() {
  static std::vector<int64_t> v;
  return v;
}
inline std::vector<int64_t>& childEngineNs() {
  static std::vector<int64_t> v;
  return v;
}
inline std::map<std::string, VerbTiming>& exclusiveTimings() {
  static std::map<std::string, VerbTiming> t;
  return t;
}
// Returns the (selfWall, selfEngine) it computed — the per-form table needs the
// SAME numbers and cannot recompute them, because this function CONSUMES the
// child buckets (it zeroes cw[kid]). Two readers of one LIFO bucket is how a
// second caller silently gets zeros.
inline std::pair<int64_t, int64_t> addExclusiveTiming(const std::string& verb, int depth,
                                                      int64_t wallNs, int64_t engineNs) {
  auto& cw = childWallNs();
  auto& ce = childEngineNs();
  const size_t need = static_cast<size_t>(depth) + 2;
  if (cw.size() < need) {
    cw.resize(need, 0);
    ce.resize(need, 0);
  }
  const size_t kid = static_cast<size_t>(depth) + 1;
  const int64_t selfWall = wallNs - cw[kid];
  const int64_t selfEng = engineNs - ce[kid];
  cw[kid] = 0;
  ce[kid] = 0;
  if (depth >= 0) {
    cw[static_cast<size_t>(depth)] += wallNs;
    ce[static_cast<size_t>(depth)] += engineNs;
  }
  VerbTiming& v = exclusiveTimings()[verb];
  ++v.count;
  // Clamped: a child that outlives its parent's timer would otherwise show up
  // as negative self time and silently cancel another verb's real seconds.
  v.wallNs += selfWall > 0 ? selfWall : 0;
  v.engineNs += selfEng > 0 ? selfEng : 0;
  return {selfWall > 0 ? selfWall : 0, selfEng > 0 ? selfEng : 0};
}

// ---- ATTR-20260812: coverage fingerprint, mechanised ----------------------
// The 2026-08-12 bar-grade record carries "0 error blocks, 2,074 graph
// emissions" from a whole-file grep of an APPEND-mode log holding every run
// since July — the count could not be re-derived afterwards (nearest candidates
// 2,114 / 2,112 / 2,065). Counted here instead, at the one place a block enters
// the run's output (doexec pushBlock, outermost depth only, before the
// storage-budget trip) so the number is per-run, survives truncation, and does
// not double-count a loop body's blocks as they bubble up.
inline int64_t g_graphEmissions = 0;
inline int64_t g_errorBlocks = 0;
inline void noteEmittedBlockType(const std::string& type) {
  if (type == "graph") ++g_graphEmissions;
  else if (type == "error") ++g_errorBlocks;
}
inline int64_t graphEmissions() { return g_graphEmissions; }
inline int64_t errorBlocks() { return g_errorBlocks; }

// BUG-350: errors a plain `capture` SWALLOWED. `errorBlocks()` above counts what
// the run DISPLAYED, and a plain `capture` displays nothing — so a run could
// swallow a disk-full mid-way, skip a whole analysis, and still report 47/47
// artifacts and exit 0 (measured 2026-08-16, controls=yes arm: one `keep if:
// failed writing scratch CSV` cost the `goalfreq_fp` series and nothing said so).
// Counted at the one place the block is dropped, with the FIRST message kept, so
// the end of the run can say plainly that something was hidden.
inline int64_t g_swallowedErrors = 0;
inline std::string g_firstSwallowedError;

// Campaign-audit follow-up to BUG-350: count + first message is a census, not
// the distribution. PIET_EMIT_SWALLOWED, cached the same way as g_timingLevel
// above (one getenv per process, -1 = not resolved yet), streams EVERY
// swallowed message to stderr as it is dropped, so a long audit run can be
// grepped for the full shape instead of just the census two fields above.
inline int g_emitSwallowedLevel = -1;
inline bool emitSwallowedEnabled() {
  if (g_emitSwallowedLevel < 0) {
    const char* e = std::getenv("PIET_EMIT_SWALLOWED");
    g_emitSwallowedLevel = (e && *e) ? 1 : 0;
  }
  return g_emitSwallowedLevel != 0;
}

inline void noteSwallowedError(const std::string& message) {
  ++g_swallowedErrors;
  if (g_firstSwallowedError.empty()) g_firstSwallowedError = message;
  if (emitSwallowedEnabled()) {
    std::string line = message;
    for (char& c : line) {
      if (c == '\n' || c == '\r') c = ' ';
    }
    if (line.size() > 500) line.resize(500);
    std::fprintf(stderr, "[swallowed] %s\n", line.c_str());
  }
}
inline int64_t swallowedErrors() { return g_swallowedErrors; }
inline const std::string& firstSwallowedError() { return g_firstSwallowedError; }

// ---- T7b ROWLOOP-VECTOR-SCOPING (report-only, PIET_EMIT_TIMING) -----------
// T6 split one do-run into {engine, PIET}. p0_landings §T7 then measured the
// string-path gen/replace at ~42-48 ms/call PIET-SIDE and named the question it
// could NOT answer: of that PIET lump, how much is the compiled row loop
// (strmutate::evalRows) versus the 108k-row CSV hand-off to `cobra setcol`?
// setcol_cost_split.md already decomposed the ENGINE half (114 ms/call, header
// re-parse dominant) — this decomposes the PIET half, on the same seam.
//
// Four phases, one accumulator each (ns), billed inside strmutate.cpp:
//   evalrows  — the compiled row loop: fillSlots + PreboundExpr::eval per row
//   outbuild  — materialising outStrings/outNums from the eval result
//   csvwrite  — csvEscape/csvNum + the ofstream write of the values CSV
//   reload    — session.load(outHbin) + varlist rebuild + nMiss seed, i.e.
//               everything PIET does AFTER the engine returns
// engine-wait for the setcol itself is already billed to g_engineWaitNs, so
// these four plus engine_ms reconstruct the verb's wall.
//
// The first probe run (rowloop_phase_probe.py, 2026-07-29) put ~55% of the PIET
// half in `reload` alone — neither of the two candidates §T7 named. But `reload`
// is three unrelated costs with three unrelated fixes, so it is sub-split; the
// three are a PARTITION of reloadNs (each also billed to reloadNs' scope), and
// `reload - (load+nmiss+varlist)` is their residual:
//   rlLoad    — session.load(outHbin) + invalidateDatastore: re-open the patched
//               hbin (frame-load materialization tax; C3 descriptor carry-over)
//   rlNmiss   — seedNMissForPureAdd: the C1-CARD-REBUILD-REDUCE seed
//   rlVarlist — varlistWithMeta + toJson + updateNMissCache: the card rebuild
//
// Same inertness contract as T6: two integer ops per phase, read only at the
// PIET_EMIT_TIMING emit site, no gate and no budget reads them.
struct StrmutPhases {
  int64_t evalrowsNs = 0;
  int64_t outbuildNs = 0;
  int64_t csvwriteNs = 0;
  int64_t reloadNs = 0;
  int64_t rlLoadNs = 0;     // sub-phases of reloadNs (partition; see comment above)
  int64_t rlNmissNs = 0;
  int64_t rlVarlistNs = 0;
  int64_t calls = 0;      // writeNew invocations (csvwrite/reload denominator)
  int64_t evalrowsCalls = 0;
  int64_t rows = 0;       // rows the row loop actually walked (per-row unit cost)

  // ROWLOOP-RLLOAD-SUBSPLIT: the sub-split of rlLoadNs itself.
  // rowloop_vector_scoping.md §4 put rl_load at ~53% of the PIET half — the
  // single dominant term, larger than the row loop and the CSV write combined —
  // but it was one unopened bucket over THREE costs with three different owners
  // and three different fixes. Billed in session_state.h:
  //   rllMaterialize — ensureMaterializedHead("datastore"): the Stage-1b virtual
  //                    resident head becoming a REAL file, i.e. a snapshot write
  //                    (owner: C3 descriptor carry-over / resident chain)
  //   rllOpen        — new Datastore(currentHbin_): the mmap + header parse of
  //                    the frame the engine just patched (owner: frame-load
  //                    materialization tax, memory cobra-frameload-*)
  //   rllReclaim     — reclaimSupersededScratch + flushDeferredReclaim: the
  //                    bounded-scratch GC routed off load() (owner: 2.2.0 item E)
  //   rllClose       — ~Datastore inside load()'s datastore_.reset(): the munmap
  //                    of the PREVIOUS frame. Not one of the three the task
  //                    named, opened because it is the only other candidate
  //                    inside rl_load's scope that can be large at 108k rows,
  //                    and leaving it inside the residual would have made a big
  //                    residual unattributable (see §5 M1 of the instrument).
  // `rl_load - (materialize+open+reclaim+close)` stays a genuine residual and is
  // the completeness check, exactly as in T7b.
  int64_t rllMaterializeNs = 0;
  int64_t rllOpenNs = 0;
  int64_t rllReclaimNs = 0;
  int64_t rllCloseNs = 0;
  int64_t rllMaterializeCalls = 0;
  int64_t rllOpenCalls = 0;
  // Billing window. session.datastore()/load() are called by `use`, by the
  // dispatcher and by other verbs too, so a process-wide sub-total would NOT be
  // a partition of rl_load. The sub-timers bill only while writeNew's rl_load
  // scope has armed this flag (RlArm below).
  bool rlArmed = false;
};
inline StrmutPhases& strmutPhases() {
  static StrmutPhases p;
  return p;
}

// Arms the rl_load sub-phase billing window for its scope. Save-restore rather
// than set-false on exit: a neutral wrapper cannot corrupt an enclosing window
// if these ever nest (BUG-142's half-record class).
struct RlArm {
  bool prev;
  RlArm() : prev(strmutPhases().rlArmed) { strmutPhases().rlArmed = true; }
  ~RlArm() { strmutPhases().rlArmed = prev; }
  RlArm(const RlArm&) = delete;
  RlArm& operator=(const RlArm&) = delete;
};

// Scoped stopwatch for ONE rl_load sub-phase. Reads the arm at construction, so
// an unarmed call site costs one bool test and one clock read and records
// nothing. `calls` may be null when the invocation count is not interesting.
struct RlPhaseTimer {
  int64_t* sink;
  int64_t* calls;
  std::chrono::steady_clock::time_point t0;
  RlPhaseTimer(int64_t* s, int64_t* c)
      : sink(strmutPhases().rlArmed ? s : nullptr),
        calls(strmutPhases().rlArmed ? c : nullptr),
        t0(std::chrono::steady_clock::now()) {}
  ~RlPhaseTimer() {
    if (!sink) return;
    *sink += std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count();
    if (calls) *calls += 1;
  }
  RlPhaseTimer(const RlPhaseTimer&) = delete;
  RlPhaseTimer& operator=(const RlPhaseTimer&) = delete;
};

// P9 arc② — cross-PROCESS counter folding (probe cell G-4).
//
// These accumulators are process-global `inline` variables, which is exactly
// right for a synchronous single-process `--do` run and exactly wrong the
// moment tasks execute in worker processes: the parent would emit its own
// near-zero values while the real work was counted in address spaces it cannot
// see, and floor_gate would go blind on any parallel run while comparing
// "0 == 0" as agreement.
//
// So a worker reports what it accumulated and the parent adds it in. Only the
// ADDITIVE accumulators are listed here. Deliberately absent:
//   * g_graphEmissions / g_errorBlocks — the PARENT counts these itself when it
//     replays the child's blocks (they are keyed off block type), so folding a
//     child delta as well would double-count them.
//   * g_liveNamedBytesPeak — a high-water, not a sum; per-process peaks do not
//     compose into a whole-run peak, and pretending otherwise would be a worse
//     lie than leaving it alone.
//   * timing surfaces — depth semantics do not compose across processes.
inline const std::vector<std::pair<const char*, int64_t*>>& foldableAccumulators() {
  static const std::vector<std::pair<const char*, int64_t*>> v = {
      {"touched_bytes", &g_touchedBytes},
      {"snapshot_count", &g_snapshotCount},
      {"snapshot_bytes", &g_snapshotBytes},
      {"setcol_bytes", &g_setcolBytes},
      {"use_disk_loads", &g_useDiskLoads},
      {"use_virtual_hits", &g_useVirtualHits},
      {"dta_converts", &g_dtaConverts},
      {"use_stash_hits", &g_useStashHits},
      {"datastore_reloads", &g_datastoreReloads},
      {"engine_req_stateless", &g_engineReqStateless},
      {"engine_req_frame_use", &g_engineReqFrameUse},
      {"engine_req_frame_run", &g_engineReqFrameRun},
      {"engine_req_snapshot", &g_engineReqSnapshot},
      {"engine_req_postest", &g_engineReqPostest},
      {"engine_req_other", &g_engineReqOther},
      {"one_shot_spawns", &g_oneShotSpawns},
      {"serve_spawns", &g_serveSpawns},
  };
  return v;
}

// ---- PER-COMMAND counter attribution (INSPECT-2608, PIET_EMIT_TIMING=4) -----
// The gap this closes: wall has been per-verb since ATTR-20260812, but every
// BYTE/mechanism counter above is a process-wide scalar. Nothing paired "regress
// took 45 ms" with "regress touched 12 MB" — per-command counters existed only
// as whole-process differencing in tests/eval/percmd_floor (2 processes x 130
// rows). Doctrine says counters are the sworn witness and timing the noisy one,
// so the sworn half was the one without a per-command view.
//
// Shape, and why each choice:
//   * EXCLUSIVE ONLY. Inclusive per-verb rows are the documented reading trap
//     ("행 합산 금지" — see the ATTR block above). The child-bucket subtraction
//     is the same LIFO mechanism addExclusiveTiming uses, so `percmd` rows SUM
//     to the run. That sum-equality is the instrument's completeness invariant
//     and the selftest asserts it: a leak anywhere shows up as rows that no
//     longer add up.
//   * Keyed by FORM, not verb — `summarize` and `summarize|if` are different
//     work and a per-verb key averages them together. The key is verb + presence
//     FLAGS only (never values), so cardinality stays bounded.
//   * Level 4 rather than a new env var: ItemTimer arming, depth tracking, the
//     resolved-verb slots and the selftest hook all already key off
//     timingLevel(), and a second env would need to re-arm every one of them.
struct PercmdRow {
  int64_t n = 0;
  int64_t selfWallNs = 0;
  int64_t selfEngineNs = 0;
  std::vector<int64_t> self;  // parallel to foldableAccumulators()
};
inline std::map<std::string, PercmdRow>& percmdRows() {
  static std::map<std::string, PercmdRow> r;
  return r;
}
// bucket[d] = counter deltas accumulated by items AT depth d, read and cleared
// by their parent at depth d-1 — the exact twin of childWallNs()/childEngineNs().
inline std::vector<std::vector<int64_t>>& childCounters() {
  static std::vector<std::vector<int64_t>> v;
  return v;
}
// Snapshot of every foldable accumulator, in list order. Cheap: 17 loads.
inline std::vector<int64_t> percmdSnapshot() {
  const auto& acc = foldableAccumulators();
  std::vector<int64_t> out;
  out.reserve(acc.size());
  for (const auto& [name, p] : acc) {
    (void)name;
    out.push_back(*p);
  }
  return out;
}
// `form` is the key; `snap0` the snapshot taken when the item opened.
// selfWallNs/selfEngineNs MUST be the EXCLUSIVE values (addExclusiveTiming's
// return), not the item's raw wall. Billing the raw wall here was the instrument's
// own first defect, caught by reading its first census rather than by a gate: the
// counter columns were exclusive while the wall column was inclusive, so three
// nested wrappers each read ~146 s of the same 146 s and a reader summing the
// column got nonsense under a key literally named `self_wall_ms`.
inline void addPercmdDelta(const std::string& form, int depth, const std::vector<int64_t>& snap0,
                           int64_t selfWallNs, int64_t selfEngineNs) {
  const std::vector<int64_t> now = percmdSnapshot();
  const size_t k = now.size();
  auto& cc = childCounters();
  const size_t need = static_cast<size_t>(depth) + 2;
  if (cc.size() < need) cc.resize(need);
  for (auto& b : cc)
    if (b.size() < k) b.resize(k, 0);
  const size_t kid = static_cast<size_t>(depth) + 1;

  PercmdRow& row = percmdRows()[form];
  if (row.self.size() < k) row.self.resize(k, 0);
  ++row.n;
  for (size_t i = 0; i < k; ++i) {
    const int64_t total = now[i] - (i < snap0.size() ? snap0[i] : 0);
    const int64_t self = total - cc[kid][i];
    cc[kid][i] = 0;
    if (depth >= 0) cc[static_cast<size_t>(depth)][i] += total;
    // Clamped for the same reason the wall version clamps: a negative self
    // would silently cancel another form's real bytes.
    row.self[i] += self > 0 ? self : 0;
  }
  row.selfWallNs += selfWallNs > 0 ? selfWallNs : 0;
  row.selfEngineNs += selfEngineNs > 0 ? selfEngineNs : 0;
}

// Form key: the resolved verb plus presence flags for the qualifiers that change
// the work a command does. Values are deliberately excluded — `if beertax > .5`
// and `if beertax > .9` are the same FORM, and keying on the text would make the
// table unbounded and unreadable.
inline std::vector<std::string>& resolvedFormSlots() {
  static std::vector<std::string> s;
  return s;
}
inline void clearResolvedForm(int depth) {
  auto& s = resolvedFormSlots();
  if (depth < 0) return;
  if (static_cast<size_t>(depth) >= s.size()) s.resize(static_cast<size_t>(depth) + 1);
  s[static_cast<size_t>(depth)].clear();
}
inline void noteResolvedForm(const std::string& form) {
  if (timingLevel() < 4) return;  // one predictable branch below level 4
  const int d = g_doexecDepth;
  if (d <= 0 || form.empty()) return;
  auto& s = resolvedFormSlots();
  if (static_cast<size_t>(d) >= s.size()) s.resize(static_cast<size_t>(d) + 1);
  if (s[static_cast<size_t>(d)].empty()) s[static_cast<size_t>(d)] = form;  // first wins
}
inline std::string takeResolvedForm(int depth) {
  auto& s = resolvedFormSlots();
  if (depth < 0 || static_cast<size_t>(depth) >= s.size()) return std::string();
  return s[static_cast<size_t>(depth)];
}


inline void resetAll() {
  g_engineWaitNs = 0;
  verbTimings().clear();
  // ATTR-20260812: the depth-split surfaces reset with the rest. g_doexecDepth
  // is deliberately NOT reset — it is a live scope counter, not an accumulator,
  // and zeroing it mid-run would mis-level every item after this point.
  itemRecords().clear();
  nestedTimings().clear();
  exclusiveTimings().clear();
  childWallNs().clear();
  childEngineNs().clear();
  resolvedVerbSlots().clear();
  // INSPECT-2608 level-4 surfaces reset with the rest, for the same reason.
  percmdRows().clear();
  childCounters().clear();
  resolvedFormSlots().clear();
  g_graphEmissions = 0;
  g_errorBlocks = 0;
  g_swallowedErrors = 0;
  g_firstSwallowedError.clear();
  strmutPhases() = StrmutPhases{};
  g_touchedBytes = 0;
  g_snapshotCount = 0;
  g_snapshotBytes = 0;
  g_setcolBytes = 0;
  g_useDiskLoads = 0;
  g_useVirtualHits = 0;
  g_dtaConverts = 0;
  g_useStashHits = 0;
  g_datastoreReloads = 0;
  g_liveNamedBytesPeak = 0;
  g_engineReqStateless = 0;
  g_engineReqFrameUse = 0;
  g_engineReqFrameRun = 0;
  g_engineReqSnapshot = 0;
  g_engineReqPostest = 0;
  g_engineReqOther = 0;
  g_oneShotSpawns = 0;
  g_serveSpawns = 0;
}

}  // namespace counters
}  // namespace piet

#endif  // PIET_NATIVE_COUNTERS_H
