// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Group-level parallel execution — `parallel[, workers(N)] { … }`.
//
// PLEVEL-IMPL-WAVE arc ① (SKELETON). Design authority =
// internal note PLEVEL-DESIGN-brief.md; the User
// rulings this file implements are PENDING-USER #14 (RESOLVED block) + SE-062:
//   · syntax = §2.3 option **B** (a real block command, not the `*! piet`
//     comment directive of option A — A is a later surface on top of this same
//     internal path, never a second implementation);
//   · architecture = §3.1 **A** (a worker is a whole `piet_native` process, so
//     SessionState/scratch/est-handle/frame isolation is free — see §3.3);
//   · default N = `min(6, max(1, cores-2))`, **but SE-062 caps the DEFAULT at
//     kMemoryCapWorkers** (a worker's measured maxRSS is 961–1,125 MB and this
//     is an 8 GB machine — instruments/plevel_rss_scoping.md);
//   · MVP = block form only (§8 #7); loop form (`parallel foreach`) is a
//     separate wave (BACKLOG PLEVEL-LOOPFORM-WAVE);
//   · `_rc` = source-order first failing task + `r(n_failed)` (§8 #4 → (c)).
//
// **What arc ① does and does NOT do.** This arc lands the SKELETON only:
// header/option parsing with honest errors, the worker-count budget, task
// decomposition, the parent task-spec schema, the `--parallel-worker` child
// entry point that consumes that schema — and executes the block **serially,
// in-process**. That serial leg is not a placeholder to be deleted: it is the
// N=1 identity leg the design's G-1/G-2/G-3 byte-identity gates are defined
// against, and it stays the code path whenever the resolved budget is 1 worker.
// Arc ② adds the spawn/splice half (parent forks `--parallel-worker` children
// and replays their buffered blocks in SOURCE order, §2.5); arc ③ adds counter
// summation (G-4), the freeze fixture (G-6) and the seeded-RED certificate
// (G-8). Until arc ② lands, `runParallelBlock` never spawns anything, so the
// feature cannot change any existing output: `parallel` is a NEW keyword that
// no existing .do file or fixture contains.
#ifndef PIET_NATIVE_PARALLEL_H
#define PIET_NATIVE_PARALLEL_H

#include <string>
#include <vector>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::parallel {

using ojson = nlohmann::ordered_json;

// SE-062 (User ruling 2026-07-29): the *default* worker count is bounded by the
// memory budget, not by the core count. A worker's measured high-water is
// 961–1,125 MB and aggregate N=3 reached 1,728.7 MB with the pressure level
// repeatedly at warn (instruments/plevel_rss_scoping.md), so the design's
// arithmetic default of 6 is not reachable on this class of machine. An
// EXPLICIT `workers(N)` is still honoured above this cap (§8 #2: "사용자 명시
// workers() 우선") — the cap governs what we choose for the user, not what we
// let the user choose. Pressure clamping (below) applies to both.
// 2026-08-15 re-key (User directive: fastest option within the configured
// budget): with the post-ladder worker cost above, N=4 measured 2.5-2.6 GB
// aggregate on the reference sweep with zero errors and byte-identical
// artifacts, and its speedup (geomean 1.655x) is statistically tied with N=3
// (1.680x) — both rounds disagree on the winner. 4 is therefore inside the
// measured-safe envelope on the 8 GiB reference box; the budget arithmetic
// above still decides the actual N.
inline constexpr int kMemoryCapWorkers = 4;

// AUTO DERIVATION — User ruling 2026-09-02 (supersedes #14-b's floor of one
// worker and the #25 "raise it to 2" question, both retired): when the user has
// not written `workers(N)`, the worker count is DERIVED from the budgets and
// from the work, never from a constant. "왜 1이거나 2어야 하는 거야? 예산에 맞추고
// 업무에 맞출 수 있어야지." Every term is a real constraint on this machine and
// this block; the smallest one binds; the result is reported term by term in the
// banner so a clamp is never anonymous:
//
//   cores  = compute_cores > 0 ? compute_cores : perf_cores
//            (auto = the PERFORMANCE-core count on Apple silicon, hw.perflevel0 —
//            the 2026-08-17 ruling that the efficiency-core region is not opened
//            for workers; hardware width elsewhere)
//   memory = budget_mb / per_worker_mb, per_worker_mb = kWorkerBaseMb +
//            kWorkerDataFactor × resident_mb — a worker's cost is not a constant,
//            it scales with the dataset it inherits (measured: in-process physpeak
//            474-490 MB with a 164 MB resident frame; marginal worker 499-543 MB).
//            resident unknown -> the old constant kWorkerRssMb, still conservative.
//   tasks  = the number of independent tasks in the block (a 2-task block cannot
//            use a third worker; the pull queue would only idle it)
//   disk   = (free bytes on the scratch volume × kScratchHeadroom) / per-task
//            scratch, per-task scratch = kTaskScratchFactor × resident_mb — THE
//            term that actually binds on the User's real configuration
//            (controls=yes is scratch-bound; N=4 beat N=2 by 4% and doubled the
//            concurrent scratch, and 47-task runs died of disk at N=4). Measured
//            2026-09-01 (peak47f, 47 tasks serial, 5-s samples of the scratch
//            directory): per-task peak median 470 MB, p90 593 MB, max 857 MB
//            against a 156 MB resident file -> ×3.0 / ×3.8 / ×5.5; re-measured
//            2026-09-02 on the wired binary (5-task slice) — see the selftest pin
//            and BUGLOG. kTaskScratchFactor = 4 sits above the p90.
//   N      = clamp(min(cores, memory, tasks, disk), 1, kMemoryCapWorkers)
//
// kMemoryCapWorkers stays the measured envelope (N ≥ 5 is unmeasured by ruling).
// An explicit `workers(N)` still overrides every term except the critical
// pressure clamp. Constants are RE-KEYED to measurements, never argued with.
struct DerivationInputs {
  int computeCores = 0;            // the setting; 0 = auto
  int perfCores = 1;               // hardware: performance cores (auto width)
  int64_t memoryBudgetMb = 0;      // EFFECTIVE budget (setting, or recommended)
  int64_t residentBytes = 0;       // current dataset payload; 0 = unknown
  int64_t taskCount = 0;           // independent tasks in the block; 0 = unknown
  int64_t scratchFreeBytes = -1;   // free space on the scratch volume; <0 = unknown
};
struct Derivation {
  int n = 1;
  int coresTerm = 1;
  int memTerm = 1;
  int taskTerm = 0;   // 0 = no task information
  int diskTerm = 0;   // 0 = no disk information
  int64_t perWorkerMb = 0;
  int64_t perTaskScratchMb = 0;
  std::string reason;  // the banner text: every term, then the result
};
inline constexpr int64_t kWorkerRssMb = 700;        // resident unknown: the 08-15 constant
inline constexpr int64_t kWorkerBaseMb = 200;       // a worker with no data (piet_native + cobra serve)
inline constexpr int64_t kWorkerDataFactor = 2;     // resident copy + one working copy
inline constexpr int64_t kTaskScratchFactor = 4;    // per-task scratch peak / resident (p90 3.8, 09-01)
inline constexpr int64_t kTaskScratchMinMb = 64;    // floor for tiny datasets (process + graph files)
inline constexpr int kScratchHeadroomPct = 50;      // never plan past half of the free space
Derivation deriveDefaultWorkers(const DerivationInputs& in);

// Live inputs a caller can hand resolveWorkers() when it knows them — the
// block runner knows its task count and the session it runs in; a bare
// resolveWorkers(n) (selftest, banners) knows neither and derives without those
// terms, which is what "unknown" means above.
struct ResolveContext {
  int64_t residentBytes = 0;
  int64_t taskCount = 0;
  std::string scratchDir;  // where the workers' scratch will live; "" = $PIET_WORKDIR / TMPDIR
};
// Cheap dataset-size estimate for ResolveContext: the current head's file size
// (an hbin is the payload plus a 64 KB header), 0 when no data is loaded.
int64_t residentBytesEstimate(const SessionState& session);
// Performance-core count (hw.perflevel0.logicalcpu on Apple silicon), else the
// hardware width, never 0.
int perfCoreCount();
// Free bytes on the volume holding `dir` (statvfs), or -1 when unknown.
int64_t scratchFreeBytes(const std::string& dir);


// Parsed `parallel [, workers(N)]` header (the trailing `{` is stripped by the
// caller, exactly as executeLoop already does for every other block head).
struct HeaderOptions {
  int workers = 0;  // 0 = not specified → resolveWorkers picks the default
  bool isError = false;
  ojson error;  // engine_error block, ready to return as the block's output
};

// Honest-error parsing (§2.3's "오탈자 침묵" trap, stated there for the comment
// directive but binding here for the same reason): a misspelt or unknown option
// STOPS with an error. Silently ignoring `wokers(4)` would run serially while
// the user believes they asked for four workers — a silent-wrong outcome, which
// PIET's honest-error doctrine forbids.
HeaderOptions parseHeader(const std::string& header);

// The resolved worker budget + why it is what it is. `reason` goes into the
// stderr banner so a clamp is never silent (§3.6: "조용히 느려지는 것보다 명시가
// 낫다").
//
// CONTRACT CORRECTED 2026-08-14 (ONESHOT-P9N4 C): this said `reason` was
// non-empty "only when the budget differs from what the header asked for". That
// has not been true since the default path started publishing its derivation —
// ruling #9 (banner + one-line reason) WANTS the derivation named on a run
// nobody clamped, and warn-level pressure names itself with the budget
// deliberately unchanged. Read `reason` as: the human-readable account of how
// `n` was arrived at, empty only when the header's number was taken verbatim
// under normal pressure.
struct WorkerBudget {
  int n = 1;
  int requested = 0;  // 0 = default path
  int cores = 1;
  int pressure = 1;  // kern.memorystatus_vm_pressure_level (1 normal/2 warn/4 critical)
  std::string reason;
  // Engine threads PER WORKER — the N² guard. Computed once here so the two
  // consumers (task-spec field, spawn-time setenv) cannot drift apart, and keyed
  // to `compute_cores` when the user set one: ruling #11 redefined that setting
  // as the TOTAL core budget, so it — not the hardware width — is what gets
  // divided among N workers. compute_cores=0 (auto) keeps the hardware width.
  int engineThreads = 1;
  // Memory budget PER WORKER, in MB — the settings budget (the recommended
  // half-of-RAM when the user left it at auto) divided among N, exported to
  // each worker's engine as CECON_MEMORY_BUDGET_MB so the engine's own fan-out
  // bounds its per-thread scratch to its share (src/core/threads.h
  // threads_for_work). Floor 64 MB. Same single-computation rule as
  // engineThreads: the spec field and the spawn-time setenv read this one value.
  int64_t engineMemoryMb = 0;
};
WorkerBudget resolveWorkers(int requested);
WorkerBudget resolveWorkers(int requested, const ResolveContext& ctx);

// §3.4 work unit = ONE top-level statement of the block body. `bodyLines` is
// the already-flattened body doexec hands every block runner (nested blocks are
// re-serialized to `header {` / body / `}` text), so decomposition is a
// brace-depth scan: a task begins at depth 0 and ends when depth returns to it.
//
// This is used for the task-spec and the banner count ONLY — the serial leg
// replays the WHOLE body through doexec::executeDoSource (via loops.cpp's
// runBody, the same helper foreach/capture/quietly use), so a decomposition
// disagreement cannot alter arc ①'s output. Arc ② is where this becomes
// load-bearing, and G-1 is what proves it right there.
std::vector<std::string> decomposeTasks(const std::vector<std::string>& bodyLines);

// §2.4 isolation contract, serialized: what a child inherits (fully expanded
// task text · programs · globals/scalars/matrices · set-state · cwd · a
// read-only path to the parent's snapshotted head) and nothing else. What comes
// back is only the three things in §2.4: files the child wrote, its buffered
// blocks, its exit status. `parentHead` may be empty (arc ①: no snapshot is
// taken because nothing is spawned).
ojson buildTaskSpec(const std::vector<std::string>& tasks, const SessionState& session,
                    const WorkerBudget& budget, const std::string& parentHead);

// Writes `spec` to $PIET_PARALLEL_DUMP_SPEC/<basename> and returns the path, or
// "" when the env var is unset. Arc ①'s only file side effect, and it is
// opt-in: an unconditional write would make every parallel block touch the disk
// before the spawn half exists to justify it. Selftest uses this to pin the
// schema, and arc ② replaces the env gate with the real per-worker handoff.
std::string dumpTaskSpecIfRequested(const ojson& spec);

// `piet_native --parallel-worker <spec>` — the child entry point (§3.2).
// Reads a spec written by buildTaskSpec, runs its tasks through the SAME
// dispatch()/doexec path the parent would have, and writes ONE object to
// stdout: {"ok":true,"tasks":[{"index":i,"blocks":[…],"failed":bool}, …]}.
// Spec and result are CBOR, which keeps a string's bytes exactly when they
// are not UTF-8, as JSON text cannot (BUG-1131).
// stdout is the protocol channel, so the tasks' own output lives INSIDE it as
// data — the child never writes user-visible text to stdout, which is what lets
// the parent replay blocks in source order and keep G-1 byte-identity.
int runParallelWorker(const std::string& specPath);

// -- arc ② · real process fan-out --------------------------------------------

// One task's result, as it came back from the worker that ran it.
struct TaskOutcome {
  bool delivered = false;   // a worker produced a parseable result for this task
  ojson blocks = ojson::array();
  bool failed = false;
  bool hasExitCode = false;
  int exitCode = 0;
  std::string stoppedAtReason;
  ojson countersDelta = ojson::object();  // additive counters this task accumulated (G-4)
  std::string driverError;  // non-empty = the worker itself failed, not the task
};

// Run `tasks` across up to `budget.n` concurrent worker PROCESSES and return
// their outcomes in SOURCE order (index i of the return == task i).
//
// SCHEDULING (§3.4): a dynamic pull queue, never a static split. A worker slot
// takes the next unassigned task the moment it frees up, so one slow task never
// leaves the other slots idle holding a pre-assigned share. It is implemented
// with one short-lived worker process PER TASK rather than N long-lived workers
// fed over a pipe: the pull semantics are identical, and reusing the existing
// `--parallel-worker <spec>` child entry point means there is no second wire
// protocol to keep byte-honest. The cost is one process + one `cobra serve`
// spawn per task instead of per worker; the long-lived pool is the named
// performance follow-up, not a semantic one.
//
// Returns an empty vector when fan-out could not even be attempted (no
// executable path, no scratch dir) — the caller must then fall back to the
// serial leg rather than dropping work.
std::vector<TaskOutcome> runTasksInWorkers(const std::vector<std::string>& tasks,
                                           const SessionState& session,
                                           const WorkerBudget& budget,
                                           const std::string& parentHead,
                                           const std::string& scratchDir);

}  // namespace piet::parallel

#endif  // PIET_NATIVE_PARALLEL_H
