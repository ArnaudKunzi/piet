// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// CobraSession — persistent `cobra serve` worker client (MAXPERF Stage 1b,
// contract_A §25 stateless serve + §26 resident-frame data plane).
//
// A one-shot `cobra <subcmd> ...` fork/exec per verb (cobra_runner.cpp's
// runCobra) pays a process-spawn tax on EVERY dispatch — and, worse, the
// chain-head model rewrites+mmaps a fresh scratch .hbin per mutation (the
// F5 incident: 2,742 mutations each writing a full dataset). CobraSession
// replaces both: ONE worker child spawned via the same fork/exec+pipes
// discipline as runCobra, then line-delimited JSON requests over its stdin/
// stdout for the worker's whole lifetime. Two request families:
//   • sendStateless(argv)  — §25: byte-equal to a one-shot `cobra argv`
//     (reads --data from disk, writes --out to disk). Kills the spawn tax
//     with zero resident-state coupling — the universal safe path.
//   • frameUse/frameRun/frameSnapshot/frameDrop — §26: the dataset stays
//     RESIDENT in worker RAM across mutations; a mutation advances the
//     resident frame in place (no per-verb file write) and reports a small
//     delta the caller folds into a shadow varlist card. state materializes
//     to disk only on demand (snapshot).
//
// This class owns ONLY the worker process + wire protocol. All resident
// bookkeeping (which path the frame corresponds to, dirty bit, shadow card)
// lives on SessionState, which drives this via the runCobra intercept — see
// cobra_runner.cpp and session_state.{h,cpp}.
#ifndef PIET_NATIVE_COBRA_SESSION_H
#define PIET_NATIVE_COBRA_SESSION_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "piet/cobra_runner.h"  // CobraRunResult

namespace piet {

// One column's metadata as reported by a frame `use`/`run` response. Mirrors
// the contract_A §26 frame vars/vars_delta entries: {name, type:"f64"|
// "factor", n_missing, n_levels(factor only)}.
struct CobraFrameVar {
  std::string name;
  std::string type;              // "f64" | "factor"
  int64_t nMissing = 0;
  std::optional<int64_t> nLevels;  // factor only
  // BUG-365: display format / variable label as the engine holds them. Empty means
  // the engine stated none (an older engine states none for every column), so a
  // consumer must treat empty as "no information", never as "%9.0g".
  std::string format;
  std::string label;
};

// Parsed `"frame":{...}` metadata from a frame response. `valid` is false
// for a non-frame response (stateless / handshake error / dead worker).
struct CobraFrameMeta {
  bool valid = false;
  int64_t rows = 0;
  int64_t cols = 0;
  // run-only delta counts (0 for `use`, which reports the full `vars` below).
  int64_t changed = 0;
  int64_t added = 0;
  int64_t dropped = 0;
  // INSPECT-2608: the NAMES behind `dropped`, not just the count. The card-fold
  // completeness test needs them to tell "a column the card knew about went
  // away" (a real restructure — rebuild) from "a scratch column the card never
  // held went away" (nothing the card describes moved — foldable). Empty when
  // the engine reported a bare count instead of an array.
  std::vector<std::string> droppedNames;
  bool structure = false;        // true => column set/types changed materially
  // true unless this run changed the row count or the column set. This is a
  // deliberately DIFFERENT question from `structure`: `structure` is a
  // WRITE-path fact (can a later `save` byte-patch the file? — true whenever
  // the resulting frame carries no source path, which includes a plain row
  // permutation like `sort`), while `card_stable` is the READ-path fact PIET's
  // Stage-1b shadow varlist card actually needs (is my per-column metadata
  // still valid?). `sort` is structure=true but card_stable=true; `keepif`
  // (row count changed) is structure=true and card_stable=false. Default true
  // so a fresh CobraFrameMeta (no frame response parsed yet) reads as valid.
  bool card_stable = true;
  // GA sweep 2026-08-22: a STAGED merge writes no file and prints no stderr notice,
  // so the counts PIET publishes as `merge_counts` travel on the frame response
  // instead. Absent (mergeCountsValid=false) for every other verb and for any engine
  // that predates the key, which is what keeps the resident routing honest: the
  // caller can tell "no counts were sent" from "zero matches".
  bool mergeCountsValid = false;
  int64_t mergeMasterOnly = 0, mergeUsingOnly = 0, mergeMatched = 0;
  std::vector<CobraFrameVar> varsDelta;  // run: the changed/added columns
  std::vector<CobraFrameVar> vars;       // use: the full column list
};

// Parsed `"est":{...}` metadata — the contract_A §31 session estimation store.
// The engine attaches this to exactly two response kinds:
//   • the MINT — a frame `run` whose verb was lincom/nlcom/test and which
//     actually fitted a model. §31 deliberately does NOT mint on stateless
//     §25 requests or one-shot CLI runs, so PIET only ever sees a handle on
//     the resident-frame path it already uses for post-estimation.
//   • every `postest` response (handle echoed, plus the `fits` signal).
// Absent everywhere else, in which case `valid` stays false.
struct CobraEstMeta {
  bool valid = false;
  std::string handle;    // opaque "est:<nonce>:<n>" — §31: NEVER parse/construct/predict it
  int64_t k = 0;         // number of coefficients
  int64_t nObs = 0;      // the fit's N
  std::string vcovType;  // the vcov AS FITTED: "iid"|"hc1"|"cluster"|"twoway"
  // Process-monotonic count of post-estimation model FITS this worker has
  // performed, as opposed to reuses of a stored model. §31 publishes it
  // specifically to be asserted against: one mint followed by N handle calls
  // must report the SAME value on all N+1 responses. If it climbs, the re-fit
  // is back — which no value pin on the numbers can detect, because a
  // resurrected re-fit returns the same answer, just slowly.
  int64_t fits = -1;
};

// Process-global engine-settings epoch. Bumped by settings.cpp's
// applyComputeCores() when CECON_NUM_THREADS changes; every CobraSession
// records the epoch it spawned under and respawns (re-reading the env) when
// it observes a newer one. Decouples the settings writer from the per-session
// worker so no session pointer needs to reach applyComputeCores().
long engineSettingsEpoch();
void bumpEngineSettingsEpoch();

class CobraSession {
 public:
  // Does NOT spawn — construction is cheap; the worker is spawned lazily on
  // the first request (a `display 1+1` command never touches cobra, so it
  // never pays for a worker). `cobraBin` is the resolved binary path
  // (resolveCobraBin()).
  explicit CobraSession(std::string cobraBin);
  ~CobraSession();

  CobraSession(const CobraSession&) = delete;
  CobraSession& operator=(const CobraSession&) = delete;

  // Tri-state frame support. UNKNOWN until the first frame op resolves it:
  // an engine that only speaks §25 answers a frame request with exit 2
  // (unknown request kind), which flips this to false permanently (all
  // routing then uses sendStateless). true once any frame op succeeds.
  enum class Frames { Unknown, Yes, No };
  Frames framesState();

  // True once the worker could not be started at all (old binary without
  // `serve`, or a second consecutive death) — the caller then falls back to
  // one-shot runCobra for everything.
  bool serveUnavailable() const { return serveUnavailable_; }

  // CECON_NUM_THREADS changed (settings.cpp bumped the epoch) since this
  // worker spawned — cobra reads that env only at process start, so the
  // intercept flushes resident state, respawnForEpoch()s, and resyncs.
  bool epochStale() const { return spawned_ && engineSettingsEpoch() != spawnEpoch_; }
  void respawnForEpoch();

  // §25 stateless request — byte-equal to a one-shot `cobra argv`. Never
  // touches resident frame state. On a dead/failed worker returns a
  // CobraRunResult with .error set + exit_code<0 (same shape as a launch
  // failure), so callers can fall back to runCobraOneShot.
  CobraRunResult sendStateless(const std::vector<std::string>& argv);

  // §26 frame ops. `metaOut` (when non-null) receives the parsed frame
  // metadata. frameRun's `argv` must NOT contain --data/--out (the intercept
  // strips them). All return the underlying CobraRunResult (exit/stdout/
  // stderr) so the caller sees engine errors verbatim.
  CobraRunResult frameUse(const std::string& path, CobraFrameMeta* metaOut);
  CobraRunResult frameRun(const std::vector<std::string>& argv, CobraFrameMeta* metaOut);
  // §38: a column subset of the resident frame, written to `out` as a real .hbin.
  // Does not touch the frames_ capability state — see the definition.
  CobraRunResult frameGetcols(const std::vector<std::string>& cols, const std::string& out);
  CobraRunResult frameSnapshot(const std::string& path);
  CobraRunResult frameDrop();
  // W0 (P2PERF §W0): read the worker's own mechanism counters (patch-vs-full
  // writes, provenance hashes, dataset opens). Purely observational — it touches
  // no session state — and sent exactly once, at the PIET_EMIT_COUNTERS emit
  // site, so a normal run never issues it. `stdout_text` is the counters JSON.
  // An engine that predates the op answers exit 2, which the caller reports as
  // an absent block rather than a failure.
  CobraRunResult frameCounters();

  // §27 session snapshots (contract_A 1.16.0 DRAFT — MAXPERF Stage 2). All are
  // O(columns) RAM captures with NO disk write; the client wiring lives on
  // SessionState (residentPreserve/residentRestore/residentSaveVirtual). None
  // touch the Frames capability flag: a §26-only engine rejects them with
  // exit 2/3 (the caller falls back to the clone/byte-copy plane) WITHOUT
  // disabling the already-proven §26 resident data plane for the session.
  //   • stash        — Stata `preserve`: snapshot the resident under `name`.
  //   • unstash      — Stata `restore`: re-establish `name` as the resident
  //                    (metaOut carries the full §26 `use`-body var list).
  //   • save_virtual — Stata `tempfile`+`save`: register `path` → an O(columns)
  //                    snapshot in the worker's virtual-file table; a later
  //                    frameUse(path) hits it transparently (virtual-first).
  CobraRunResult frameStash(const std::string& name, CobraFrameMeta* metaOut);
  // MUTRES-WIRE (2026-08-22): the resident varlist, no disk. Same {rows,cols,vars}
  // body frameUse parses; a §25-only or older engine answers non-zero and the
  // caller falls back.
  CobraRunResult frameVars(CobraFrameMeta* metaOut);
  CobraRunResult frameUnstash(const std::string& name, bool keep, CobraFrameMeta* metaOut);
  CobraRunResult frameSaveVirtual(const std::string& path, CobraFrameMeta* metaOut);
  // BUG-384: forget a virtual-save registration (tempfile died at its Stata
  // scope boundary). Idempotent; never spawns a worker just to forget.
  CobraRunResult frameVirtualDrop(const std::string& path);
  // §40 (BUG-390/392): forget a named stash without restoring it. Idempotent on
  // the engine (absent name = exit 0, dropped=false); never spawns a worker just
  // to forget. `stashesLeft` (optional) receives the engine's remaining count.
  CobraRunResult frameStashDrop(const std::string& name, int64_t* stashesLeft = nullptr);
  // True once a worker has been spawned this session. Teardown paths use it to
  // avoid spawning a worker merely to tell it to forget something.
  bool hasWorker() const { return spawned_; }

  // -- §31 session estimation store (post-estimation without the re-fit) ----
  //
  // Tri-state capability, exactly parallel to Frames above and adopted for the
  // same reason. It MUST be resolved by trying the op, never by reading a
  // version: the engine branch that implements §31 still advertises
  // `contract_a 1.19.0` in its serve HELLO banner on purpose (Art 5 — the
  // compiled constant moves only at the release stamp, because advertising an
  // unratified version would be advance notice of an unmerged change), and Art
  // 7 means a shipped PIET pins the ARCHIVED engine, which for the window
  // between the two merges has no `postest` op at all. So the banner cannot
  // distinguish "no §31" from "§31, unstamped" — only the attempt can.
  //
  // An engine without §31 rejects the unknown frame op with exit 2. That is an
  // unambiguous capability signal HERE, unlike frameRun's exit 2 (which is a
  // verb usage error), because framePostest's caller owns the entire argv and
  // never sends the two argv shapes §31 itself answers with exit 2 (a verb
  // other than lincom/nlcom/test, or an argv carrying --data/--out). Every
  // other failure — notably exit 3, the explicit stale/unknown-handle error —
  // is a per-request miss, NOT a capability downgrade, and leaves this Yes.
  enum class Postest { Unknown, Yes, No };
  Postest postestState() const { return postest_; }

  // Answer a linear/nonlinear combination from an already-fitted model
  // addressed by `handle` — no data read, no re-fit. `argv[0]` must be
  // lincom/nlcom/test, and `argv` must carry NO --data/--out and NO
  // model-specification flags (--y/--x/--fe/--vcov/--cluster/--keepif). That
  // discipline is the caller's: §31 rejects a violation (exit 2 / exit 3 via
  // the E6 unconsumed-flag net) rather than letting a handle and a partial
  // specification silently race for which one describes the model.
  //
  // On ANY non-zero exit the caller MUST fall back to the full specification.
  // It must never assume, reuse or synthesize an answer — a stale handle
  // becoming a wrong-model answer is the exact failure §31 exists to make
  // unrepresentable, and the handle's per-process nonce only holds up if the
  // client honours the error instead of guessing.
  CobraRunResult framePostest(const std::string& handle, const std::vector<std::string>& argv,
                              CobraEstMeta* metaOut);

  // The `est` block from the MOST RECENT response on this worker, and a
  // counter of responses parsed. Every response rewrites BOTH — one that
  // carries no `est` resets lastEst_ to {valid=false} — so a mint can never be
  // read as belonging to a later call. Callers that need to know "did the call
  // I just made produce this mint?" must compare responseSeq() across the
  // call: a request that never reached this worker at all (serve disabled, a
  // one-shot fallback) leaves the counter untouched, which is the only way to
  // tell it apart from a request that reached the worker and minted nothing.
  const CobraEstMeta& lastEst() const { return lastEst_; }
  long long responseSeq() const { return responseSeq_; }

  // Mechanism instrumentation (process-wide), the direct analogue of
  // SessionState::cardFoldCount() and Datastore::constructCount(): how many
  // post-estimation calls this process answered through a stored model rather
  // than by re-specifying and re-fitting. Gate legs assert on THIS, because a
  // value pin on lincom's numbers stays green when the handle path is dead.
  static long postestHitCount();
  static void resetPostestHitCount();

  // True iff the immediately-preceding request respawned a dead worker (the
  // caller must resync — frameUse the last materialized head — and surface a
  // restart error rather than silently continue). Cleared by the next
  // request that does not respawn.
  bool consumeRespawned() {
    bool r = respawnedSinceLastCheck_;
    respawnedSinceLastCheck_ = false;
    return r;
  }

 private:
  // Sends one already-built request object (id auto-assigned) and returns the
  // parsed response JSON. Handles lazy spawn, epoch-driven respawn, and a
  // single dead-worker respawn. `resp` receives the parsed response;
  // `frameMeta` (optional) is filled from resp["frame"]. Returns a
  // CobraRunResult mirroring resp{exit,stdout,stderr}, or an error result on
  // a hard I/O failure.
  CobraRunResult request(nlohmann::json req, CobraFrameMeta* metaOut);

  bool ensureSpawned();   // spawn if needed (+ handshake). false => serveUnavailable_.
  void spawn();           // fork/exec + handshake; sets serveUnavailable_ on failure.
  void killWorker();      // close pipes, terminate + reap the child.
  bool writeLine(const std::string& line);            // false on EPIPE/short write.
  bool readLine(std::string& out);                    // false on EOF/error.

  std::string cobraBin_;
  std::mutex mu_;         // serializes every request (defensive; verbs are single-threaded).
  bool spawned_ = false;
  bool serveUnavailable_ = false;
  Frames frames_ = Frames::Unknown;
  // §31 state. postest_ deliberately survives respawnForEpoch()/killWorker():
  // it describes the BINARY's capability, which a respawn of the same binary
  // cannot change. lastEst_ is the opposite — a handle is scoped to the worker
  // PROCESS (the nonce), so a respawn silently invalidates every outstanding
  // handle. That needs no explicit reset here: the fresh worker's nonce
  // differs, so a carried-over handle resolves nowhere and returns exit 3,
  // which the caller already treats as "re-run the full specification".
  Postest postest_ = Postest::Unknown;
  CobraEstMeta lastEst_;
  long long responseSeq_ = 0;
  long spawnEpoch_ = 0;
  long long nextId_ = 1;
  int consecutiveDeaths_ = 0;
  bool respawnedSinceLastCheck_ = false;

  // read buffer carried across readLine() calls (a response line can be many
  // MB and arrive split across OS read() chunks).
  std::string readBuf_;

#ifdef _WIN32
  void* childProc_ = nullptr;   // HANDLE
  void* inWr_ = nullptr;        // HANDLE (worker stdin write)
  void* outRd_ = nullptr;       // HANDLE (worker stdout read)
  void* errRd_ = nullptr;       // HANDLE (worker stderr read)
#else
  int childPid_ = -1;
  int inWr_ = -1;               // worker stdin  (we write requests)
  int outRd_ = -1;              // worker stdout (we read responses)
  int errRd_ = -1;              // worker stderr (diagnostics; drained, not parsed)
#endif
  std::thread errDrain_;        // concurrent stderr drain (avoids pipe-buffer deadlock).
};

// The CobraSession that most recently serviced a request on THIS thread, or
// nullptr if none has (serve disabled or unavailable, or nothing has talked to
// the engine yet on this thread).
//
// This exists for the same reason engineSettingsEpoch() above does, and is the
// same shape of answer: it decouples a caller that needs the worker from the
// ownership chain, so no session pointer has to be threaded through. SessionState
// owns its CobraSession privately and drives it from the runCobra intercept; the
// §31 post-estimation call site (estimation_m4.cpp) sits outside that ownership
// and has no route to it — it reaches the engine through runCobraJson, which is
// deliberately argv-shaped and cannot express a frame op.
//
// WHY "most recent on this thread" is sufficient rather than merely convenient:
// the sole consumer is the §31 handle path, and a §31 handle embeds the worker's
// own per-PROCESS nonce. Addressing the WRONG CobraSession therefore cannot
// produce a wrong-model answer — the handle does not resolve, the engine returns
// exit 3, and the caller falls back to the full specification. The worst case is
// a slow correct answer, never a fast wrong one. The correctness of the
// optimization rests on the nonce and on the handle living on LastModel (see
// session_state.h), not on this pointer being the right one.
CobraSession* activeCobraSession();

}  // namespace piet

#endif  // PIET_NATIVE_COBRA_SESSION_H
