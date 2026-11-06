// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState — full C++ port of piet/backend/piet/session.py's kernel
// state object (the M2 "session/datastore" milestone; replaces the M1
// EvalSession stub as the dispatcher-facing state).
//
// Design: EvalSession (piet/eval/session.h) stays exactly as M1 left it —
// the macro engine only depends on its accessors — and SessionState EMBEDS
// one as `eval` so macros.cpp keeps working unmodified. Fields the oracle
// keeps on SessionState but the macro engine never reads (workdir,
// current_hbin, var_meta, value_labels, last_model, history, ...) live
// directly on SessionState.
//
// Ordering: OrderedDict/OrderedFloatDict (from session.h) are reused for
// every dict-shaped field that participates in macro substitution ordering
// (locals/globals/scalars/r_values/r_strings/e_scalars) — Python dict
// insertion order is part of the observable behavior (see session.h's
// top-of-file note) and this must NOT regress at M2.
#ifndef PIET_NATIVE_SESSION_STATE_H
#define PIET_NATIVE_SESSION_STATE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/rng_mt64.h"       // Stata-exact session RNG stream (SeedSlot::stream)
#include "piet/cobra_session.h"  // Stage 1b: persistent `cobra serve` worker + frame metadata
#include "piet/counters.h"       // liveNamedBytesPeakSample() save/restore of the counter globals
#include "piet/datastore.h"
#include "piet/eval/session.h"
#include "piet/serset.h"  // serset-twoway SS: the graph-owned serset store
#include "piet/strutil.h"  // W2 esttabnames: globMatch for expandEstimatesNamelist
#include "vendor/nlohmann/json.hpp"

namespace piet {

// -- do-executor worker-thread marker (BUG-087/D9, BETA Phase F) -----------
// True only on the ONE background thread main.cpp's runDoInBackground()
// spawns per do/do_string call; false everywhere else (the UI thread, the
// --do headless synchronous thread, --selftest, ...). thread_local rather
// than a SessionState member because it identifies a THREAD, not a session
// — a single AppState's one SessionState is visited by both the UI thread
// (the synchronous "command" bindRoute, used by the single-line #cmd-input)
// and the do-worker thread (do/do_string) over its lifetime.
//
// Why this exists: SessionState::requestUserInput() (below) blocks the
// calling thread until the frontend answers or the run is cancelled.
// Calling it from the UI thread would deadlock the whole app — a bind()
// route handler executes AS PART OF the UI thread's run-loop processing
// (shell.h's threading note), so blocking inside one stalls the very loop
// that would need to keep pumping to (a) deliver the postEvent() push that
// shows the modal and (b) ever process the user's click. display_hardening.
// cpp's _request() handling checks isCurrentThreadADoWorker() before ever
// calling requestUserInput(), refusing with an honest error instead.
void setCurrentThreadIsDoWorker(bool isWorker);
bool isCurrentThreadADoWorker();

// C4-L2A (D1 card plane) escape hatch: PIET_NO_RESIDENT_READ=1 forces every
// D1-converted consumer (runCount bare, runConfirm, runSaveData's entry —
// C4-RESIDENT-READ-SCOPE-brief §4) back onto the pre-D1 datastore() path.
// Same convention as C1's PIET_NO_NMISS_CACHE: a plain per-call getenv, NOT a
// static-cached read, so a selftest leg can setenv/unsetenv around a single
// dispatch to A/B the two planes in one process. Deliberately NOT consulted
// by shadowVarlistIfCurrent() itself: the PRE-C4 card consumers
// (sort-validate-lazy entry lookups, the mutation-card fold/render) predate
// D1 and belong to the escape arm's BASELINE — disabling them too would make
// the escape arm reproduce some older build's counters, not pre-D1's, and the
// floor_gate RED arm (escape arm = old-count reproduction = corpus-reach
// proof) would stop meaning anything. Defined in session_state.cpp.
bool residentReadDisabled();

// B1 (1.16.0) run-progress schema — C++ port of session.py's RunProgress.
// Frozen wire schema (CONTRACT.md v1.11.0, contract_B). M3a: doexec.cpp's
// executor publishes this per top-level item; a headless (--run-block /
// gate) run publishes to the SAME struct but nothing reads it — a live
// GET /api/run_progress consumer arrives at the M4 frontend swap.
struct RunProgress {
  bool running = false;
  int currentLine = 0;
  int totalLines = 0;
  std::string currentCmd;             // truncated to ~120 chars
  double startedAt = 0.0;              // monotonic seconds at run start
  long long elapsedMs = 0;             // frozen at completion
  int blocksReady = 0;
  std::optional<std::string> doPath;   // resolved abs path; unset for do_string
  bool cancelRequested = false;
  std::optional<std::string> stoppedReason;  // "completed"|"cancelled"|"error"

  nlohmann::ordered_json asDict() const;
};

// session.py's LastModel — PIET's e() equivalent, sufficient for
// test/lincom/predict/margins/estat to reconstruct the cobra call.
struct LastModel {
  std::string cobraCmd;
  // BUG-458 (2026-09-08): the STATA spelling of the command that produced this
  // model ("regress", "reghdfe"), where cobraCmd is the engine's ("feols").
  // e(cmd) is published from THIS field when it is set and from cobraCmd when
  // it is not, so a handler that has not learned its Stata name keeps printing
  // exactly what `ereturn list` printed before. Never read by the replay
  // machinery (test/lincom/margins/estat keep keying on cobraCmd).
  std::string stataCmd;
  // False for an e-class program that never ran `ereturn post` (rddensity:
  // `ereturn clear` + scalars): its e() is live, but
  // estimates store / eststo refuse it r(301) "last estimates not found"
  // (estimates store on an unposted e()). runEststo reads it.
  bool ePosted = true;
  std::string y;
  std::vector<std::string> x;
  std::vector<std::string> fe;
  std::string vcov = "iid";
  // Stata's spelling of e(vce) when it is not the engine-derived one
  // setLastModel() publishes from `vcov` (areg/reghdfe `ols`, ivreghdfe
  // `robust cluster`, xtpoisson/xtlogit fe `oim`; "" erases e(vce)). Carried on
  // the model so a later setLastModel() on the same model (setEstHandleM4,
  // reached by test/lincom/predict) republishes it instead of `iid`.
  std::optional<std::string> eVce;
  std::vector<std::string> cluster;
  std::optional<std::string> ivar;
  std::string depvar;
  std::string dataPath;
  // W:P5 fit-sample fidelity (BUG-163): the restriction and weights the FIT
  // actually ran under, so post-estimation refits (test/lincom/nlcom/margins/
  // boottest) reconstruct the fitted sample instead of silently refitting on
  // the full dataPath — measured: after `regress ... if region<=4 in 1/4000`
  // (n=3,106) a `test` refit answered from n=4,649. Two filter forms, matching
  // the two styles this codebase uses:
  //  - fitDataPath: a materialized if/in scratch file (runRegress's
  //    applyIfAndInFilter output). Empty => the fit ran on dataPath itself.
  //  - fitKeepif: the --keepif expression passed straight to the engine
  //    (buildEstimationArgvM4 style). Empty => none.
  // fitWeight/fitWtype mirror --weight/--wtype for the same reason (replayed
  // only into engine CLIs that accept them). setLastModel() replaces the
  // struct wholesale, so none of these can outlive the model they describe
  // (same safety argument as the §31 handle below).
  std::string fitDataPath;
  std::string fitKeepif;
  std::string fitWeight;
  std::string fitWtype;
  // The weight expression as typed, for e(wexp) (Stata stores "= " + the text
  // after `=`, leading blanks removed: `[aw=  wf ]` -> "= wf ", w2aj_qualifiers
  // qf_rw_aw_wide). Empty => fitWeight. setLastModel() fills it from
  // commandWexp when the fit left it empty.
  std::string fitWexp;
  // csdid / DiD post-estimation context (not exercised by M2's vertical
  // slice — csdid handlers land at M3 — but the fields are ported now so
  // LastModel is a complete, not partial, struct).
  std::optional<std::string> timevar;
  std::optional<std::string> gvar;
  std::optional<std::string> method;
  std::optional<std::string> comparison;
  std::optional<std::string> pscoretrim;
  // csdid `long2` (e(typebase) long2): the `estat event, window()` re-run must
  // forward it, or the long-difference cells silently become the short ones
  // (w2ag_csdid_drdid.log:670-683 ag_l2ny_estore/ev).
  bool long2 = false;
  std::optional<nlohmann::ordered_json> result;  // cached raw cobra output JSON
  // IV/LIML endogenous-regressor / instrument role split (BETA Phase G,
  // estat endogenous/overid wiring). `x` above conflates endog+exog into
  // ONE list (endog first, per Stata's own coeftable order) for the
  // OLS-refit approximation test/lincom/margins/predict already share —
  // see runIvRegress's own comment — which loses the information needed to
  // reconstruct a genuine `cobra iv-diag` call (a SEPARATE cobra command
  // from `cobra iv`, computing Durbin/Wu-Hausman/Sargan/Basmann under the
  // classical iid VCE — apps/cmds/cmds_diag.cpp::cmd_ivdiag on the cobra
  // side). These three preserve the ORIGINAL (endog = instruments) block's
  // role split so `estat endogenous`/`estat overid` can invoke iv-diag
  // with the right --endog/--x/--instruments. Populated by runIvRegress
  // for both ivregress and ivreghdfe/liml (cheap to always fill; the
  // cobraCmd=="iv" gate in runEstat's endogenous/overid branch is what
  // actually restricts iv-diag to plain `iv`, not liml, matching
  // estat firststage's existing gate).
  std::vector<std::string> ivEndog;
  std::vector<std::string> ivExog;
  std::vector<std::string> ivInstruments;

  // contract_A §31 session-estimation-store handle ("est:<nonce>:<n>"), or
  // empty when this model has none. OPAQUE — never parsed, constructed or
  // predicted. Filled by estimation_m4.cpp's post-estimation path from the
  // `est.handle` the engine mints on the FIRST lincom/nlcom/test after a fit;
  // every subsequent combination on the same model addresses the stored
  // (names, β, V) tuple through it instead of re-specifying the model and
  // making the engine fit it again.
  //
  // Living on LastModel — rather than on the session or on the worker — is the
  // load-bearing safety property, not a convenience. setLastModel() replaces
  // this struct WHOLESALE, so fitting a new model necessarily resets the handle
  // to empty: a handle can never outlive the model it was minted for and be
  // spent on a different one. That is the single way this optimization could
  // return a confidently wrong answer (model A's combination reported for model
  // B), and the struct's own lifetime is what forecloses it. The engine's
  // per-process nonce is the second, independent guard: a handle from an
  // earlier worker resolves nowhere rather than resolving to the wrong model.
  std::string estHandle;

  // e(sample) T2 (internal note esample/SPEC.md §4.2). Whether this
  // model's estimation sample lives on the working frame as the hidden f64
  // column `__piet_esample__` (piet/datastore.h kEsampleColumn):
  //   kColumn      -- the fit handler wrote the engine's admitted rows
  //                   (--esample-out) into that column BEFORE setLastModel.
  //                   An admitted row holds the install's SERIAL
  //                   (esampleSerial below), any other row 0; rows added later
  //                   carry missing. The row reading is
  //                   `(__piet_esample__==SERIAL)`, which reads both as 0.
  //   kUnavailable -- the handler has no engine mask (not wired, or a
  //                   restored/used model whose mask was not carried): e(sample)
  //                   is a named r(198), never a guess.
  //   kStale       -- `estimates restore` of results whose `_est_NAME` variable
  //                   is no longer on the data (T3); a named r(198).
  //   kNone        -- the state of "no LastModel at all", whose producers are a
  //                   fresh session and SessionState::lastModel_reset()
  //                   (`ereturn clear`, `ereturn post`, `clear all`/`clear
  //                   results`), and the one model state `estimates use` sets:
  //                   results read from a file carry no e(sample) (T3). e(sample)
  //                   is then 0 (w2_esample.log A1-A5, G11, :1531 G6 `0|0|0|28|`).
  //                   See SessionState::esampleReading.
  // The DEFAULT is kUnavailable: setLastModel() replaces the struct wholesale,
  // so a fit handler that never sets the field must not answer 0 or 1.
  enum class EsampleState { kNone, kColumn, kUnavailable, kStale };
  EsampleState esample = EsampleState::kUnavailable;
  // kColumn only: which install wrote the mask (SessionState::
  // esampleColumnInstalls at that install, >= 1). The column follows the rows,
  // so the working frame can come to carry ANOTHER fit's mask while this model
  // is live -- a resampling prefix leaves its last replicate's model and
  // returns to the frame it started from (handlers_resample.cpp). The serial in
  // the cells is what tells the two apart (esampleReading refuses a mismatch).
  unsigned long long esampleSerial = 0;
  // kColumn only: the working frame the install produced (the fit's frame plus
  // the mask column). dataPath stays the frame the fit READ -- the path every
  // refit (predict/test/lincom/margins, `estimates save`/`use`) replays -- so
  // the install does not change what those verbs see; this second path only
  // lets predict tell "the head is still the fit's frame" (handlers' headMoved).
  std::string esampleInstallPath;
  // The fit kept its singleton fixed-effect groups (`--keep-singletons`): areg
  // and xtreg,fe always do, as Stata does (w2_esample.log:897 D3 e(N) 59 / N_g
  // 11, :925 D4 59, :1957 H8 58), and reghdfe with `keepsingletons`. Every
  // engine refit forwards it (estimation_m4c.cpp fitConventionFlagsM4) so the
  // refit's sample is the fit's.
  bool keepSingletons = false;
  // BUG-1067: the fit omitted the LATER member of each collinear set
  // (`--collinear-forward`), which implements reghdfe, areg and xtreg,fe per StataBE
  // probes (z = 2*x: `x o.z`; regress keeps the engine's pivoted `o.x z`). Every
  // engine refit forwards it (fitConventionFlagsM4) so the refit's coefficients
  // are the fit's.
  bool collinearForward = false;

  // eb-order T-P3 part 0 (internal note eb-order/SPEC.md):
  // which fit this e() is. A fresh construction mints a new serial and a copy
  // keeps it, so setLastModel can tell a NEW e() (a fit; clears the previous
  // fit's estimation block and posted e(b)/e(V)) from a copy-modify-set of the
  // live one (setEstHandleM4, the `estat event` re-run, erepost, estimates
  // restore of the live store), which keeps them.
  std::uint64_t fitSerial = nextFitSerial();
  static std::uint64_t nextFitSerial() {
    static std::atomic<std::uint64_t> seq{0};
    return ++seq;
  }
};

struct HistoryEntry {
  std::string cmd;
  double ts = 0.0;
  bool success = false;
};

// program.py's ProgramDef — a captured `program define … end` block.
//   name   — canonical name (case as written in `program define`).
//   rclass — true if declared `, rclass` (flushes returnStaging on return).
//   body   — post-preprocess lines (continuation-joined, comment-stripped;
//            empty lines preserved for approximate line-number context).
//   ado    — the command whose `<ado>.ado` autoload defined it; empty for a
//            program defined in a do-file or interactively.
struct ProgramDef {
  std::string name;
  bool rclass = false;
  std::vector<std::string> body;
  std::string ado;
};

// Per-variable editor metadata (session._var_meta[name]): label / format /
// stata_type — whichever keys are present (mirrors the Python dict-of-dict).
struct VarMeta {
  std::optional<std::string> label;
  std::optional<std::string> format;
  std::optional<std::string> stataType;
};

// One Stata `char` entry — `char varname[charname] value` or the dataset-level
// `char _dta[charname] value` (varname literal "_dta"). Flat triple, byte-for-
// byte the same shape as cobra's import-dta --out-meta / export-dta --in-meta
// `characteristics` JSON array (apps/cmds/cmds_data.cpp) and src/io's
// DtaChar/DtaWriteChar, so the round trip needs no reshaping at either hop.
struct DtaCharacteristic {
  std::string varname;
  std::string charname;
  std::string value;
};

// preserve_restore.py's PreserveFrame — one snapshot on the LIFO preserve
// stack. hbinPath is a file-copy of the hbin at snapshot time (deep copy);
// the display-layer maps are value-copied (std::map's copy ctor is already a
// deep copy for these plain-value maps, mirroring Python's
// `{k: dict(v) for k, v in ...}` comprehensions).
struct PreserveFrame {
  std::string hbinPath;   // clone/byte-copy fallback plane: a real file snapshot.
  std::string stashName;  // §27 resident plane: the worker's stash key (no file).
  // BUG-1139: `preserve` with no dataset loaded preserves the empty dataset
  // (StataBE 19.5: rc 0, and `restore` brings back _N 0 with no variables), so
  // the frame holds no snapshot and restoring it clears what was built since.
  bool noData = false;
  std::map<std::string, VarMeta> varMeta;
  // The implicit float of an untyped creation (SessionState::implicitFloatVars)
  // is the other half of a variable's type claim, so it is kept with varMeta.
  std::set<std::string> implicitFloatVars;
  std::map<std::string, std::map<std::string, std::string>> labelDefs;
  std::map<std::string, std::string> varLblname;
  std::map<std::string, std::map<int, std::string>> valueLabels;
  std::string dataLabel;                          // dataset label (Stata `label data`)
  std::vector<DtaCharacteristic> characteristics;  // Stata `char` table
  // BUG-922: the preserve level (SessionState::preserveLevel) that ran
  // `preserve`. A frame belongs to that level for its whole life:
  //   I1 only pushPreserveSnapshot writes this field, with the level of the
  //      `preserve` (a `restore, preserve` re-arms with the popped frame's
  //      level, never the current one);
  //   I2 levels never decrease from the bottom of preserveStack to the top
  //      and never exceed preserveLevel(), so "this level holds a copy" is
  //      SessionState::ownPreserveFrame() != nullptr;
  //   I3 when a program returns or a nested do/run file or `parallel` child
  //      concludes, on success or error, every frame deeper than the caller's
  //      level is restored and popped by program::restorePreservesDeeperThan,
  //      the one unwinder.
  // Only the owning level may restore, cancel or re-arm its frame: a deeper
  // level's `restore` in any form is r(622) and leaves the caller's copy alone
  // (oracle w2ad_prog_preserve_nest pn_*_restore_caller, pn_*_not_caller,
  // pn_*_rp_caller: in1 622:4:30). 0 = the top-level run.
  int level = 0;
};

// postfile.py's PostHandle — an open `postfile` collector. names/types are
// parallel arrays (types: byte|int|long|float|double|str<N>|strL, lowercase).
// rows accumulate as (kind, text) pairs — kind "numeric" stores the Python
// repr()-equivalent text already formatted by the post-expr evaluator (M3b:
// postfile.cpp is the C++ arithmetic-evaluator port), "string" stores the
// raw string. postclose() factorizes string columns and writes an hbin via
// a `cobra convert` round-trip (native hbin WRITE has no direct API in this
// monorepo's linked columnar.h — read-only; see postfile.cpp's notes).
struct PostHandle {
  std::vector<std::string> names;
  std::vector<std::string> types;
  std::string target;
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;  // (kind, text) per cell

  bool isStringCol(size_t idx) const {
    if (idx >= types.size()) return false;
    std::string t = types[idx];
    return t.size() >= 3 && (t[0] == 's' || t[0] == 'S') && (t[1] == 't' || t[1] == 'T') &&
           (t[2] == 'r' || t[2] == 'R');
  }
  size_t nCols() const { return names.size(); }
};

// What `log off` / `log on` / `log close` asked of an SMCL log. Stata records
// the command line and only then the paused/closed footer (or, for `log on`,
// the resumed header without the line); PIET's blocks reach logTee() AFTER the
// handler ran, so each request waits in LogState::ops until the tee meets the
// command's own echo line (SessionState::logTee). Text logs never use this:
// they stop and start at once.
enum class LogOp { Pause, Resume, Close };

// BUG-028 / BS-006: one open `log using` file. path is the resolved
// absolute path (post handlers_m3.cpp::runLog's cwd-relative resolution);
// stream is held open for the log's whole lifetime (SessionState::openLog/
// closeLog own it); paused mirrors Stata's `log off`/`log on` (the stream
// stays open but writes are suspended while true). The text format keeps its
// original plain transcript; the SMCL format (`smcl` below) is written by the
// writer in session_state.cpp -- see the comment above logTee() there for the
// byte layout and what was identified against StataBE.
// W:P8: `name` is Stata's log name — empty for the unnamed log opened by a
// plain `log using`, non-empty for `log using FILE, name(X)`. Several logs may
// be open at once (Stata tees output to all of them), which is why
// SessionState holds a vector rather than a single slot.
struct LogState {
  std::string path;
  std::ofstream stream;
  bool paused = false;
  std::string name;
  // Dispatch depth at which this log was opened. The unnamed log keeps the
  // original "outermost frame only" rule (depth 1) so every pre-existing
  // fixture is byte-unchanged; a NAMED log records the frames at its own
  // depth, which is what makes `log using …, name(X)` work inside a do-file:
  // the outermost tee fires once, AFTER the whole file has run, by which time
  // a log opened and closed mid-file is long gone. Frames deeper than `depth`
  // are not teed separately — their blocks are already flattened into the
  // depth-`depth` frame's result, so they would double-write.
  int depth = 1;
  // -- SMCL format only (all inert for a text log) ----------------------------
  bool smcl = false;
  // The {txt}/{res}/{com}/{err} mode the stream is currently in, as the
  // letters t/r/c/e ('?' = unknown): Stata writes a mode tag only where the
  // mode changes.
  char smclMode = 'r';
  // The next command echo at this log's depth is the `log using` line itself,
  // which Stata does not record (the log was not on yet).
  bool skipNextEcho = false;
  // Requests from the handlers (`log off` / `log on` / `log close`) whose
  // command line the tee has not reached. For an SMCL log `paused` is the
  // state the TEE has written up to; the handlers' effective state is `paused`
  // with these applied in order.
  std::vector<LogOp> ops;
};

// BS-124 (BETA campaign C4): one `timer` stopwatch slot (Stata's own
// `timer on/off/clear/list N`, N in 1..100). Monotonic clock only (never
// wall-clock) — matches Stata's own elapsed-time semantics. `running` +
// `startedAt` are transient (only meaningful between an `on` and its
// matching `off`); `totalSeconds`/`count` accumulate across on/off cycles
// so `timer list` can report total / count = average, oracle-pinned
// format (see handlers_m3.cpp::runTimer's doc comment for the exact
// column widths).
struct TimerSlot {
  bool running = false;
  std::chrono::steady_clock::time_point startedAt;
  double totalSeconds = 0.0;
  long long count = 0;
};

// PP-L2A / K1a (PINGPONG-SCOPE-brief §2 K1a): one memo of a completed
// `.dta`/`.csv` -> scratch-hbin conversion, keyed in useConvCache_ by the
// RESOLVED source path. `statKey` is the whole guard: (inode, size, mtime)
// of the source at conversion time — a re-`use` reuses hbinPath/metaPath only
// when a fresh stat of the source produces the byte-identical key. Anything
// else (a stat failure, any field moved, a vanished scratch file, the
// PIET_NO_USE_CACHE escape) falls through to the ordinary conversion, so the
// cache is a pure fast path with no semantic surface of its own.
struct UseConvCacheEntry {
  std::string statKey;   // "" is never stored — an unkeyable source is not cached
  std::string hbinPath;  // conversion output inside workdir_ (read-only re-open)
  std::string metaPath;  // .dta label sidecar; "" for the .csv/.tsv branch
};

// PP-L2B / K1b (PINGPONG-SCOPE-brief §2 K1b): one memo of a `use`d source whose
// resident frame was stashed in the worker's RAM (§27 frameStash) right after
// the disk load. A re-`use` of the UNCHANGED source restores it via frameUnstash
// (O(columns) RAM, no disk open, no conversion, no re-hash) instead of the
// convert+load path — the layer K1a (UseConvCacheEntry) cannot reach because K1a
// still re-opens the hbin from disk. Keyed in useStashCache_ by the RESOLVED
// source path; `statKey` is the WHOLE guard, identical to K1a's (inode,size,
// mtime) — any mismatch, a stat failure, a dead stash (worker restart), or the
// PIET_NO_USE_CACHE escape all fall through to the disk path, so the cache adds
// no semantic surface. `stashName` is a client-owned "__usecache:<src>" name
// (STABLE per source so a changed source re-stashes in place, freeing the old
// frame's RAM — the engine has no drop-stash-by-name op, and unstash-to-drop
// would restore it as a side effect; the stat guard lives entirely here on the
// PIET side, so the name need not carry the stat). `hbinPath` is the byte-twin
// on disk the restore re-adopts as a CLEAN head (zero-write card build — the
// residentRestore +7% lesson) and the crash-recovery floor if the stash dies.
struct UseStashCacheEntry {
  std::string statKey;    // "" is never stored — an unkeyable source is not cached
  std::string stashName;  // "__usecache:<resolved-src>" — the worker-RAM stash key
  std::string hbinPath;   // on-disk byte-twin: clean-head re-adopt + recovery floor
  std::string metaPath;   // .dta label sidecar to re-apply; "" for the .csv/.tsv/.hbin path
};

class SessionState {
 public:
  // AER scope-2 B2-3 (review 2026-09-09): resetProcessCaches() is the ONE
  // out-of-line hook a new session gets. Today it clears the business-calendar
  // cache (bcal.cpp), which is a process singleton because the expression
  // surfaces that read it carry no session — so without this call two sessions
  // in one process (the --run-block harness, every selftest leg) would see each
  // other's `bcal create`. It is defined in session_state.cpp so this header
  // does not have to pull in bcal.h.
  explicit SessionState(std::string workdir) : workdir_(std::move(workdir)) {
    resetProcessCaches();
    // BUG-512: the macro layer's `_N` (eval/session.h, EvalSession::obsCount).
    // Capturing `this` is sound because a SessionState can be neither copied
    // nor moved (its std::mutex members delete both), and nothing in the tree
    // ever reassigns `eval` wholesale.
    eval.obsCount = [this]() -> long long { return macroObsCount(); };
    // The macro layer's runiform()/runiformint() seed (EvalSession::rngSeed),
    // same capture rationale as obsCount above.
    eval.rngSeed = [this]() -> eval::EvalSession::RngSeedReading {
      return eval::EvalSession::RngSeedReading{seed.get(), seed.epoch()};
    };
    eval.rngMt64 = [this]() -> cecon::Mt64* { return &seed.stream(); };
    // e(sample) T2: the macro layer's observation-1 value (esampleObs1 below),
    // same capture rationale as obsCount above.
    eval.esampleObs1 = [this](std::string* why) -> std::optional<int> {
      return esampleObs1(why);
    };
    // BUG-718: the macro layer's dataset subscript `VAR[i]` (macroVarObs below),
    // same capture rationale as obsCount above.
    eval.varObs = [this](const std::string& name,
                         long long idx) -> std::optional<eval::EvalSession::VarObsReading> {
      return macroVarObs(name, idx);
    };
    // BUG-719: the inline `: type VAR' refreshes the plane it reads
    // (EvalSession::syncVarPlane), same capture rationale as obsCount above.
    eval.syncVarPlane = [this]() { syncEvalVarLabels(); };
    // `: label lblname #` reads the value-label sets (EvalSession::valueLabelSet),
    // same capture rationale as obsCount above.
    eval.valueLabelSet =
        [this](const std::string& lblname) -> const std::map<std::string, std::string>* {
      auto it = labelDefs.find(lblname);
      return it == labelDefs.end() ? nullptr : &it->second;
    };
    // BUG-1006: the macro layer's `` `c(name)' `` (EvalSession::cValue,
    // macroCValue below), same capture rationale as obsCount above.
    eval.cValue = [this](const std::string& name) -> std::optional<std::string> {
      return macroCValue(name);
    };
    // EM13 / LBL-CHAR: `: char NAME[CHAR]` (EvalSession::characteristic), same
    // capture rationale as obsCount above.
    eval.characteristic = [this](const std::string& target,
                                 const std::string& name) -> std::optional<std::string> {
      for (const auto& c : characteristics)
        if (c.varname == target && c.charname == name) return c.value;
      return std::nullopt;
    };
    // LBL-DATALABEL: `: data label` (EvalSession::dataLabel), same capture rationale.
    eval.dataLabel = [this]() -> std::string { return dataLabel; };
    // `: sortedby` (EvalSession::sortedBy): the head's engine sort claim.
    eval.sortedBy = [this]() -> std::string {
      if (!hasData()) return std::string();
      try {
        const Datastore* d = datastore();
        return d != nullptr ? d->sortedBy() : std::string();
      } catch (...) {
        return std::string();
      }
    };
    // eb-order T-P3: the one coefficient source (EvalSession::estBlock) and the
    // e()-is-live signal (EvalSession::modelLive), same capture rationale as
    // obsCount above. An `ereturn post` e() writes no block; its coeftable
    // mirror in eval.lastModelResult (handlers_ereturn.cpp) is that e()'s own,
    // and it is read only while the posting is live.
    eval.estBlock = [this]() -> const nlohmann::ordered_json* {
      if (lastEstBlock.has_value()) return blockPostsEb(*lastEstBlock) ? &*lastEstBlock : nullptr;
      if (ePostedLive && !lastModel_.has_value() && eval.lastModelResult.has_value())
        return &*eval.lastModelResult;
      return nullptr;
    };
    eval.modelLive = [this]() -> bool {
      return lastModel_.has_value() || lastEstBlock.has_value() || ePostedLive;
    };
    eval.eNameListsComplete = [this](const std::string& kind) -> bool {
      if (ePostedLive || eval.eStrings.get("cmd") == "estpost" ||
          (!lastModel_.has_value() && !lastEstBlock.has_value())) return true;
      // PCA publishes its entire matrix contract, while its scalar/macro
      // lists still have separate implicit metadata and remain unclaimed.
      if (kind != "matrices" || eval.eStrings.get("cmd") != "pca") return false;
      for (const char* name : {"means", "components", "C", "Psi", "Ev", "L"})
        if (!eval.findClassMatrix('e',name)) return false;
      return eval.eStrings.get("Ctype") != "correlation" || eval.findClassMatrix('e',"sds");
    };
    eval.eNameOrder = [this](const std::string& kind) { return eNameOrderFor(kind); };
    registerLiveSession();
  }
  // Leaves the live-session registry sweepOrphanScratch reads (below).
  ~SessionState();

  // -- dataset -------------------------------------------------------------
  bool hasData() const { return currentHbin_.has_value(); }
  const std::optional<std::string>& currentHbin() const { return currentHbin_; }

  // -- current file (P8 "Honest Cockpit", U1) -------------------------------
  // The resolved path of the last successfully-loaded dataset (`use`/
  // `sysuse`) — distinct from currentHbin_, which may point at a SCRATCH hbin
  // conversion (.dta/.csv sources go through a `cobra convert`/`import-dta`
  // step first). currentFile_ always holds the user-facing ORIGINAL path, so
  // the varlist route and the open-action result can tell the frontend
  // "what's actually loaded" instead of a painted-but-empty status bar. Set
  // only on a successful load (handlers.cpp's runUse); never auto-cleared —
  // out of this item's minimal additive scope.
  // BETA A4 (BS-091): a successful `use` is the one point that resets
  // c(changed) to false — see load()'s corresponding true-set below.
  void setCurrentFile(const std::string& path) {
    currentFile_ = path;
    datasetChanged = false;
  }
  const std::string& currentFile() const { return currentFile_; }

  // See the constructor: process-level caches a new session must not inherit.
  static void resetProcessCaches();

  // Set a new dataset path; invalidates any cached datastore (session.load).
  //
  // 2.2.0 item E (SPEC II-0 speed-ladder rung b — bounded scratch budget with
  // oldest-superseded eviction): load() is the single choke-point where the
  // live dataset pointer moves, so the hbin it replaces is exactly "the
  // predecessor a new scratch hbin supersedes". reclaimSupersededScratch()
  // REGISTERS it as reclaimable and evicts the oldest superseded files only
  // when the total exceeds the scratch budget — so a short mutation burst
  // (well under budget) does NO deletion I/O on the critical path (Doctrine Ⅱ,
  // Gate ② — the eager per-mutation unlink regressed p2_load3gen ~0.9s), while
  // a long session's scratch stays bounded (132 GB precedent). Safety
  // exclusions: NEVER a user file (only files inside workdir_), NEVER a live
  // preserve snapshot. (session_state.cpp; env PIET_NO_SCRATCH_GC disables it,
  // PIET_SCRATCH_BUDGET_MB tunes the ceiling.)
  // ALLOC-C3A-ARC1-CARRY: descriptor carry-over across ONE mutation.
  //
  // `load()` below resets datastore_, which destroys the parse whose factor
  // dictionaries the next open is about to rebuild identically. CarryArm takes
  // those dictionaries out FIRST and hands them to the next datastore() call.
  //
  // Explicit hand-off, never a global "last descriptor" cache: the arm is a
  // scoped object owned by the one call site that knows both the delta and the
  // file it is about to load, and it disarms on the way out — including on an
  // early return or a throw. A process-wide cache would silently apply itself
  // to loads nobody vetted, and would make every construct-count test a
  // silent-SKIP surface ([[cache-invalidates-test-proxy]]).
  class CarryArm {
   public:
    CarryArm(SessionState& s, const std::string& target, const cecon::CarryDelta& delta)
        : s_(s) {
      // Nothing loaded, or the current parse is for a different file than the
      // one that was patched: there is no descriptor worth carrying.
      if (!s_.datastore_ || !s_.currentHbin_.has_value()) return;
      s_.carried_ = s_.datastore_->releaseCarry();  // the columns are spent…
      // ALLOC-C3A-ARC2A-PREVMAP: …but the MAPPING is not. ARC 1 dropped it right
      // here, which made the previous header bytes unavailable to the one place
      // that needs them — `open_carried` — and so blocked V3's byte identity
      // proof outright (instruments/ALLOC-C3A-ARC2-PREBYTES-20260730.md §2).
      // Hold the spent Datastore instead, purely to keep its mmap alive, and let
      // datastore() hand [map_base(), map_base()+map_size()) to the carried open
      // before releasing it. "검증 후 해제" — brief §4 ⚠, which specified this
      // reorder as V3's precondition.
      //
      // THE MUNMAP DOES NOT BECOME FREE BY MOVING: releaseCarryPrev() carries the
      // rllClose timer to the new release point, so the cost stays billed to the
      // same counter. Leaving the timer behind here would delete a measurement
      // and read as a saving ([[instrument-capture-order-halfrecord]]) — and it
      // would flatter V3 by exactly one munmap.
      //
      // PIET_NO_CARRY_DEFER=1 restores ARC 1's immediate release: same binary,
      // one branch apart, so the deferral's RSS cost can be A/B'd without
      // rebuilding (the COBRA_NO_CARRY precedent — a comparison of the change,
      // not of two builds that also differ in link order,
      // [[runaway-attribution-3arm-control]]). Per-call getenv, NOT a
      // static-cached read — this file's PIET_NO_RESIDENT_READ convention, so a
      // selftest leg can setenv/unsetenv around ONE dispatch and exercise both
      // lifetimes in a single process. The read costs nothing next to the
      // munmap it decides the timing of.
      if (std::getenv("PIET_NO_CARRY_DEFER") != nullptr) {
        counters::RlPhaseTimer _rllClose(&counters::strmutPhases().rllCloseNs, nullptr);
        s_.datastore_.reset();
      } else {
        s_.carryPrev_ = std::move(s_.datastore_);   // mapping stays alive
        s_.carrySrc_ = s_.carryPrev_->carrySource();
      }
      s_.carryTarget_ = target;
      s_.carryDelta_ = delta;
      s_.carryArmed_ = true;
    }
    ~CarryArm() {
      s_.carryArmed_ = false;
      s_.carryTarget_.clear();
      s_.carried_.clear();
      s_.carried_.shrink_to_fit();
      // ALLOC-C3A-ARC2A-PREVMAP backstop: datastore() releases the previous
      // mapping the moment the carried open is done with it, so normally there is
      // nothing left here. This catches the paths where the carry was never
      // consumed at all — an early return, a throw out of the open, a target that
      // never got loaded — so the deferral can never outlive the arm that raised
      // it. Both release points are inside strmutate's RlArm scope (the arm is
      // declared inside the rl_load block), so rllClose bills either way.
      s_.releaseCarryPrev();
    }
    CarryArm(const CarryArm&) = delete;
    CarryArm& operator=(const CarryArm&) = delete;

   private:
    SessionState& s_;
  };

  // ALLOC-C3A-ARC2A-PREVMAP: release the deferred previous mapping. Idempotent,
  // and the ONE place the deferral ends — so the `munmap` this reorder postponed
  // is billed to rllCloseNs here exactly as ARC 1 billed it in CarryArm's
  // constructor. The timer travels WITH the release: that is the whole discipline
  // of this row ([[instrument-capture-order-halfrecord]]).
  void releaseCarryPrev() {
    if (!carryPrev_) {
      carrySrc_ = {};
      return;
    }
    counters::RlPhaseTimer _rllClose(&counters::strmutPhases().rllCloseNs, nullptr);
    carryPrev_.reset();
    carrySrc_ = {};
  }

  // Report-only: what the last carried open decided. Read by the G9 fallback
  // instrumentation; cecon::Dataset's own counters hold the running totals.
  cecon::CarryOutcome lastCarryOutcome() const { return lastCarryOutcome_; }

  // What a load() call MEANS about the resident chain — BUG-193's lesson turned
  // into a parameter instead of a rule callers have to remember.
  //
  // load() reads `path == residentToken_` as "this load continues the resident
  // chain, leave the worker's frame alone". That is right for a mutation
  // reloading its own virtual --out and WRONG for a `use` reading a file off
  // disk — and the two are byte-indistinguishable whenever the resident token
  // happens to be a real, user-named file (which K1b makes routine). The fix
  // shipped as a discipline: runUse calls residentDropToken() first. A
  // discipline is exactly what nobody notices is missing at the next call site,
  // so the intent is now stated at the call:
  //
  //   ChainContinues — the caller MINTED `path` as its own output (every
  //                    mutation verb). Historic behavior, and the default so
  //                    that all ~90 such sites stay byte-identical.
  //   FreshRead      — the caller is replacing the dataset from a file it did
  //                    not just write (a `use`). Drops the token, so the chain
  //                    check cannot read a coincidence as a continuation.
  //
  // FreshRead is not "load() plus a courtesy call": it is the only way to say
  // this, and saying it wrong is a silent wrong answer (browse and summarize
  // disagreeing about the same dataset), which is why the selftest keeps a
  // seeded-RED leg on it.
  enum class LoadIntent { ChainContinues, FreshRead };

  void load(const std::string& path, LoadIntent intent = LoadIntent::ChainContinues) {
    // e(sample) T2: the mask install's own load keeps the pre-install head
    // pinned; any later head move releases it (pinPreInstallHead below).
    const bool installLoad = preInstallLoadPending_;
    preInstallLoadPending_ = false;
    struct ReleaseAfter {
      SessionState& s;
      bool release;
      ~ReleaseAfter() {
        if (release) s.releasePreInstallPins();
      }
    } releaseAfter{*this, !installLoad && !preInstallPins_.empty()};
    if (intent == LoadIntent::FreshRead) residentDropToken();
    std::optional<std::string> superseded = currentHbin_;
    // BUG-381 DIAGNOSTIC (PIET_SCRATCH_TRACE): head transitions, so an orphan
    // found by scratchCensusJson can be traced to the moment it stopped being
    // the head -- or shown never to have been one. Report-only, env-gated.
    // The gate is tested BEFORE the arguments are built: load() is the row-loop
    // hot path, and value_or() would construct a std::string on every call with
    // tracing off. An off-by-default diagnostic that costs an allocation per
    // mutation is not off.
    if (scratchTraceOn()) scratchTrace("LOAD", path, superseded.value_or(std::string("-")));
    currentHbin_ = path;
    ++headLoadCount;
    {
      // ROWLOOP-RLLOAD-SUBSPLIT: ~Datastore = the munmap of the PREVIOUS frame.
      counters::RlPhaseTimer _rllClose(&counters::strmutPhases().rllCloseNs, nullptr);
      datastore_.reset();
    }
    // BETA A4 (BS-091): every mutating verb (gen/replace/drop/sort/egen/...)
    // funnels through this SAME choke point to reload its rewritten scratch
    // hbin, so this is where c(changed) flips true. A fresh `use` ALSO calls
    // load() (before setCurrentFile()'s reset below runs), so the net effect
    // after `use` is still false — only a genuine mutation leaves it true.
    datasetChanged = true;
    // Stage 1b resident-frame bookkeeping. The runCobra intercept sets
    // residentToken_ to the (virtual) --out just before a mutation handler
    // calls load(out), so `path == residentToken_` uniquely marks "this load
    // continues the resident chain — the head is a VIRTUAL frame, no file
    // exists". Every OTHER load (use/restore/import, or a legacy real --out
    // from append/merge stateless) moved the head onto a REAL file the worker
    // is now out of sync with. When frames are unused, residentToken_ is
    // always empty => always the else branch => byte-identical to before.
    if (!residentToken_.empty() && path == residentToken_) {
      // Virtual head: defer reclaiming the (possibly real) predecessor — it is
      // the crash-recovery floor until the next materialization (§3).
      if (superseded.has_value() && *superseded != path) {
        // ROWLOOP-RLLOAD-SUBSPLIT: the scratch-GC half (virtual-head arm — a
        // push_back, expected ~0; billed anyway so the arm is not assumed free).
        counters::RlPhaseTimer _rllRec(&counters::strmutPhases().rllReclaimNs, nullptr);
        deferredReclaim_.push_back(*superseded);
      }
    } else {
      releaseResidentToken("load-real");  // worker holds a different frame -> next verb frame-uses `path`
      lastMaterializedHead_ = path;  // real file (use/restore/import/legacy --out) — the new floor
      {
        // ROWLOOP-RLLOAD-SUBSPLIT: the scratch-GC half (real-head arm — this is
        // the one that can do deletion I/O once the scratch budget is exceeded).
        counters::RlPhaseTimer _rllRec(&counters::strmutPhases().rllReclaimNs, nullptr);
        flushDeferredReclaim();      // moved off the virtual chain — reclaim what it pinned
      }
      residentComputedColClear();    // C4-L4: the chain the cache described is over
      if (superseded.has_value() && *superseded != path) {
        counters::RlPhaseTimer _rllRec2(&counters::strmutPhases().rllReclaimNs, nullptr);
        reclaimSupersededScratch(*superseded);
      }
    }
  }

  // Lazy-init Datastore for currentHbin_. Returns nullptr if no data loaded.
  // Throws (propagates cecon::EngineError / std::runtime_error) if the hbin
  // fails to open — same as the Python property raising inside datastore().
  Datastore* datastore() {
    {
      // ROWLOOP-RLLOAD-SUBSPLIT (report-only, armed only inside writeNew's
      // rl_load scope — counters.h RlArm). The snapshot-write half.
      counters::RlPhaseTimer _rllMat(&counters::strmutPhases().rllMaterializeNs,
                                     &counters::strmutPhases().rllMaterializeCalls);
      ensureMaterializedHead("datastore");  // Stage 1b: a virtual resident head becomes a real file before mmap.
    }
    // BUG-124: a FAILED materialization (snapshot ENOSPC / dead worker) leaves
    // the head a dirty virtual token with NO file (the failed snapshot's
    // partial is unlinked). Opening it would throw a bare "cannot open <token>"
    // that hides the real cause — surface it honestly instead. Only the
    // token==head case is the unmaterialized-virtual-head state; the divergent
    // skip path (token != head) leaves dirty set with a REAL head on disk.
    if (residentDirty_ && currentHbin_.has_value() && residentToken_ == *currentHbin_) {
      throw std::runtime_error(
          "could not materialize the working dataset to disk (disk full or I/O error)");
    }
    if (!datastore_ && currentHbin_.has_value()) {
      {
        // ROWLOOP-RLLOAD-SUBSPLIT: the mmap + header-parse half.
        counters::RlPhaseTimer _rllOpen(&counters::strmutPhases().rllOpenNs,
                                        &counters::strmutPhases().rllOpenCalls);
        // ALLOC-C3A-ARC1-CARRY: consume the carry only for the exact file the arm
        // was raised for, and only on the stateless plane. A resident head is a
        // virtual frame whose materialization this descriptor never described
        // (brief §4/§5) — residentToken_ is already empty here for every load
        // that moved the head onto a real file, so the test is a guard, not a
        // new state machine.
        if (carryArmed_ && *currentHbin_ == carryTarget_ && residentToken_.empty()) {
          carryArmed_ = false;  // one carry per arm, even if this open throws
          datastore_ = std::make_unique<Datastore>(*currentHbin_, std::move(carried_),
                                                   carryDelta_, &lastCarryOutcome_, carrySrc_);
          carried_.clear();
        } else {
          try {
            datastore_ = std::make_unique<Datastore>(*currentHbin_);
          } catch (...) {
            // BUG-384: name the state instead of letting a bare ENOENT out. The
            // honest-error guard above only fires while residentToken_ still
            // EQUALS the head; every site that clears the token while
            // currentHbin_ still names an unwritten virtual head lands here
            // instead, and "cannot open <path>" tells the next reader nothing
            // about why a file the session believes in was never written. This
            // is BUG-349's documented class ("it declares a DIRTY VIRTUAL head
            // clean, and the bytes that only ever existed in the worker are then
            // unreachable"), and the message now says so.
            //
            // Cost is on the FAILURE path only — the stat runs after an open has
            // already thrown, never on the hot success path.
            std::error_code sec;
            const bool missing =
                !std::filesystem::exists(*currentHbin_, sec);
            const bool ours = !workdir_.empty() &&
                              currentHbin_->rfind(workdir_, 0) == 0;
            if (missing && ours) {
              throw std::runtime_error(
                  "the working dataset names a scratch file that was never written (" +
                  *currentHbin_ +
                  "). This is a virtual resident head whose bytes existed only in the "
                  "compute worker and were never snapshotted to disk — see BUG-349/384. "
                  "Re-run from your last saved dataset.");
            }
            throw;
          }
        }
      }
      // ALLOC-C3A-ARC2A-PREVMAP: the deferral ends HERE, on every path — the
      // carried open has either proved its identity or fallen back, and either way
      // nobody may read the previous bytes again. Bounding the window to a single
      // open is what keeps the two-mapping RSS double-count to one carried open
      // instead of the whole verb (~CarryArm is only the backstop). Two placement
      // rules, both load-bearing: it runs OUTSIDE the carry branch (an open of
      // some OTHER file spends the arm too, so its mapping must not linger), and
      // OUTSIDE the _rllOpen scope just closed above — billing this munmap inside
      // rllOpen would make rllClose a subset of rllOpen instead of a sibling, and
      // would silently inflate the very phase ARC 2 is measured against
      // (counters.h RlArm: the sub-timers must PARTITION rl_load).
      releaseCarryPrev();
    }
    return datastore_.get();
  }

  void invalidateDatastore() { datastore_.reset(); }

  // STALE-FRAME FIX — a LIVE silent wrong answer, not a hypothetical.
  //
  // load() treats `path == residentToken_` as "this load CONTINUES the resident
  // chain" and therefore leaves the worker's frame in place. That is right for
  // a mutation reloading its own virtual --out and wrong for a `use` reading a
  // file off disk, and the two are indistinguishable whenever the resident
  // token is a real, user-named file. K1b makes exactly that routine: for an
  // `.hbin` SOURCE the use-stash memo records hbinPath = the source itself
  // (there is no conversion output to point at), so a memo hit adopts the
  // user's own file as the token. Then:
  //
  //   use X.hbin → <mutation> → use X.hbin (memo PUT) → use X.hbin (HIT,
  //   token := X.hbin) → X.hbin edited outside PIET → use X.hbin
  //
  // reloads the new bytes into the Datastore while the WORKER keeps serving the
  // pre-edit frame: `browse` shows 11/21/31 and `summarize` reports mean 20.
  // Reproduced and RED-certified against this build with the line below
  // removed (selftest.cpp, "stale-resident-token gate (SEEDED-RED)");
  // everything else on that path is untouched shipped code, so the defect is
  // present in the shipped B.9.0 build too.
  //
  // A `use` from disk never continues a chain — by definition it replaces the
  // dataset from a file — so runUse drops the token unconditionally before
  // load(). In the ordinary case (loadPath is a freshly minted conversion
  // output) the token already differs and this is a no-op.
  // BUG-384 instrumentation. Every site that clears the resident token (or
  // drops dirty) goes through here so a PIET_SCRATCH_TRACE run can name WHO
  // cleared it; the audit below then names WHEN the session first held a head
  // with no file and no guarded token — the state whose next reader gets
  // ENOENT (BUG-349/384 class). Behavior of the clear itself is unchanged.
  void releaseResidentToken(const char* site) {
    if (scratchTraceOn())
      scratchTrace((residentDirty_ && currentHbin_.has_value() &&
                    residentToken_ == *currentHbin_)
                       ? "CLEAR(dirty-head)"
                       : "CLEAR",
                   site, residentToken_.empty() ? std::string("-") : residentToken_);
    residentToken_.clear();
    residentDirty_ = false;
  }
  void residentDropToken() { releaseResidentToken("drop-token"); }

  // BUG-384: forget a tempfile's virtual-save registration in the worker (and
  // this side's bookkeeping). Called from the two Stata-scope reapers: program
  // return (program.cpp) and the top-level run boundary (doexec.cpp).
  void residentVirtualDrop(const std::string& path) {
    virtualSaves_.erase(path);
    if (cobraSession_) (void)cobraSession_->frameVirtualDrop(path);
  }

  // BUG-384: the invariant itself, checked at the outermost dispatch boundary.
  // A head inside the session workdir must be either a real file or a GUARDED
  // virtual token (residentToken_ == head && residentDirty_ — the state
  // ensureMaterializedHead/datastore() know how to snapshot). Anything else is
  // an unbacked head: no reap, no guard, and the next open is ENOENT. One stat
  // per NEW head value (memoized), so the corpus pays ~one stat per mutation.
  // PIET_TOKEN_GUARD=abort turns the warning into an immediate abort so a
  // failing run names the minting command instead of dying 4,000 ops later.
  void auditHeadBacked(const char* where) {
    if (!currentHbin_.has_value() || workdir_.empty()) return;
    const std::string& head = *currentHbin_;
    if (head.rfind(workdir_, 0) != 0) return;            // user files: not ours to audit
    if (residentDirty_ && residentToken_ == head) return;  // guarded virtual head
    if (head == lastHeadAudited_) return;                // already checked this head
    std::error_code aec;
    if (std::filesystem::exists(head, aec)) {
      lastHeadAudited_ = head;
      return;
    }
    ++tokenOrphanWarns_;
    std::fprintf(stderr,
                 "[piet_native] WARN head-unbacked at %s: currentHbin_=%s has no file and no "
                 "guarded token (token=%s dirty=%d virtual-save=%d) — the next reader of this "
                 "path gets ENOENT (BUG-384)\n",
                 where, head.c_str(), residentToken_.empty() ? "-" : residentToken_.c_str(),
                 residentDirty_ ? 1 : 0, virtualSaves_.count(head) ? 1 : 0);
    static const bool hardAbort = [] {
      const char* e = std::getenv("PIET_TOKEN_GUARD");
      return e != nullptr && std::string(e) == "abort";
    }();
    if (hardAbort) std::abort();
    lastHeadAudited_ = head;
  }
  long long tokenOrphanWarns() const { return tokenOrphanWarns_; }

  // fix/sort-validate-lazy: cheap, side-effect-free name lookup — the
  // CURRENT dataset's column list (name/dtype/nMissing/nLevels) from the
  // resident shadow card, WITHOUT opening/mmap'ing a Datastore. Returns
  // nullptr unless BOTH shadowValid_ is true AND the card is tagged
  // (shadowCardHbin_) for currentHbin_ ITSELF — never a stale card left
  // over from a `use`/`clear`/restore that swapped datasets out from under
  // it (load()/clearData() do not touch the shadow card at all — only
  // seedShadowFromFrameVars/reseedShadowFrom/tryResidentMutationCard's fold
  // do, and each of those tags shadowCardHbin_ with the exact path the
  // card now describes), a DIFFERENT file a post-estimation command
  // resynced the resident worker to (runViaCobraSession's resync branch
  // tags with residentToken_, not currentHbin_, precisely so that case
  // reads as "foreign" here), or a respawned/rolled-back worker
  // (finishSessionOp/rollbackToFloor clear shadowValid_). A caller MUST
  // fall back to session.datastore() when this returns nullptr — trusting
  // a stale/foreign card would answer "variable not found" (or wrongly
  // "found") instead of just answering slowly.
  const std::vector<VarInfo>* shadowVarlistIfCurrent() const {
    if (!shadowValid_ || !currentHbin_.has_value() || shadowCardHbin_ != *currentHbin_) {
      return nullptr;
    }
    return &shadowCard_;
  }

  // C4-L2A (D1 card plane): shadowVarlistIfCurrent() behind the
  // PIET_NO_RESIDENT_READ escape. Consumers CONVERTED by the C4 wave use THIS
  // accessor, never shadowVarlistIfCurrent() directly, so the escape arm
  // forces exactly the D1 conversions — and only them — back onto the pre-D1
  // datastore() path (see residentReadDisabled()'s doc for why the pre-C4
  // card consumers stay out of the escape's blast radius). Same honest-
  // fallback contract: nullptr means "answer from session.datastore()", and a
  // non-null card is authoritative for name/dtype presence — answering
  // "not found" off a CURRENT card is exactly as honest as answering it off
  // the datastore, which is the whole point of the currency check.
  const std::vector<VarInfo>* residentReadCard() const {
    if (residentReadDisabled()) return nullptr;
    return shadowVarlistIfCurrent();
  }

  // C4-L2A (D1 card plane): the CURRENT dataset's row count off the resident
  // card, without opening/mmap'ing a Datastore (and so, on a dirty virtual
  // head, without the EMH frame snapshot that open would force). Same
  // currency rule as shadowVarlistIfCurrent(), same escape gate as
  // residentReadCard(), plus one more honesty bar: the row count must have
  // actually been CAPTURED at the card's own writer (a CobraFrameMeta with
  // valid=false seeds the card but leaves rows unknown → -1 here → nullopt).
  // A caller MUST fall back to session.datastore()->nRows() on nullopt.
  std::optional<int64_t> residentRowsIfCurrent() const {
    if (residentReadDisabled()) return std::nullopt;
    if (!shadowValid_ || !currentHbin_.has_value() || shadowCardHbin_ != *currentHbin_) {
      return std::nullopt;
    }
    if (residentRows_ < 0) return std::nullopt;
    return residentRows_;
  }

  // BUG-512: `_N` as a macro-context expression sees it -- the live frame's
  // observation count, 0 with no data in memory (Stata: `clear` then
  // `display _N` is 0). Card first, for the reason every D1 consumer gives: a
  // current card answers without opening a Datastore, and so without the EMH
  // snapshot a dirty virtual head would pay for one. The datastore() fallback
  // is the contract residentRowsIfCurrent() states for a nullopt, the same
  // pair loops.cpp's condition evaluator and rowsBeforeForRN already use.
  long long macroObsCount() {
    if (std::optional<int64_t> rows = residentRowsIfCurrent()) return *rows;
    Datastore* ds = datastore();
    return ds != nullptr ? static_cast<long long>(ds->nRows()) : 0;
  }

  // -- C4-L4 / C2(a): the resident COMPUTED-COLUMN cache ---------------------
  // ONE column, the SOURCE-OF-TRUTH row values PIET itself computed for the
  // most recent resident setcol commit (destring's parsed f64s, tostring's
  // rendered strings). Purpose: the C2 §9.2 chain blocker — a setcol handler
  // that needs its INPUT column's values right after a resident setcol left
  // the head dirty-virtual would otherwise session.datastore() and pay the
  // EMH whole-frame snapshot. When the input IS the column the previous
  // setcol computed, PIET already had every row value in RAM — this cache
  // hands them back with zero disk, zero engine round-trip, zero
  // factorization assumptions (for a factor column it stores the ROW STRINGS
  // the engine's own CSV factorization round-trips losslessly: levels[
  // codes[r]] == the CSV line == strs[r] by construction, so a cache read is
  // byte-equivalent to the codes→levels lookup it replaces).
  //
  // Honesty rules, same family as every C4 read: consult ONLY when the entry
  // is provably current — name matches, the row count matches the CURRENT
  // card's captured rows (which also transits the PIET_NO_RESIDENT_READ
  // escape gate, so the escape arm never reads the cache), and no fold since
  // the put has touched the name (the fold erases it; a real-file load()
  // clears it). Anything less → nullptr → the caller falls back to
  // session.datastore() verbatim (the honest flush).
  struct ResidentComputedCol {
    std::string dtype;              // "f64" | "factor" — as committed via setcol
    int64_t nRows = 0;
    std::vector<double> f64;        // dtype=="f64": the committed values (NaN = missing)
    std::vector<std::string> strs;  // dtype=="factor": the committed row strings ("" = missing)
  };
  const ResidentComputedCol* residentComputedCol(const std::string& name) const {
    if (!computedColName_.has_value() || *computedColName_ != name) return nullptr;
    std::optional<int64_t> rows = residentRowsIfCurrent();
    if (!rows.has_value() || *rows != computedCol_.nRows) return nullptr;
    return &computedCol_;
  }
  void residentComputedColPut(const std::string& name, ResidentComputedCol col) {
    computedColName_ = name;
    computedCol_ = std::move(col);
  }
  void residentComputedColInvalidate(const std::string& name) {
    if (computedColName_.has_value() && *computedColName_ == name) {
      computedColName_.reset();
      computedCol_ = ResidentComputedCol{};
    }
  }
  void residentComputedColClear() {
    computedColName_.reset();
    computedCol_ = ResidentComputedCol{};
  }

  // C2-F13a incremental nMissing: per-column missing-counts carried across
  // mutations so a pure-add doesn't re-scan (and page-fault) every column.
  // Populated after each varlist build; consumed by seedNMissForPureAdd().
  const std::unordered_map<std::string, int64_t>& nMissCache() const { return nMissCache_; }
  void updateNMissCache(const std::vector<VarInfo>& vars) {
    for (const auto& v : vars) nMissCache_[v.name] = v.nMissing;
  }
  // Seed `ds` with cached nMissing for every column a mutation left BYTE-
  // IDENTICAL, so nMissing() skips its O(nrows) rescan (+ whole-column mmap
  // fault-in) for those columns. CONSERVATIVE: any case it cannot prove
  // byte-identical falls to a full rescan (seeds nothing) — never a stale
  // count. Handles (F9, B.3.0, keeping the C2-F13a name/contract):
  //   • pure-ADD {gen,egen,duplicates_tag,recode-gen}: carry all but newVars;
  //   • replace-style (action "gen", empty newVars): carry all but the
  //     rewrittenVars target(s) — DORMANT until the call site passes them
  //     (handlers.cpp::runReplace still passes none, so it rescans fully);
  //   • variable drop/keep (action "drop"/"keep": a column subset, rows
  //     untouched): carry the survivors, drop the removed keys;
  //   • rename (action "rename"): move the cached count old->new.
  // Row-changing verbs (collapse/duplicates_drop/reshape/append/merge/keepif/
  // set_obs/tsfill/… and the reorderers sort/order) are never handled — full
  // rescan. rewrittenVars is a trailing defaulted param so the existing 3- and
  // 4-arg callers stay source-compatible. Defined in session_state.cpp (needs
  // the full Datastore type).
  void seedNMissForPureAdd(Datastore* ds, const std::string& action,
                           const std::vector<std::string>& newVars,
                           const std::vector<std::string>& droppedVars = {},
                           const std::vector<std::string>& rewrittenVars = {});

  // `clear` (session.py's clear_data, dataset-only half): drop the current
  // dataset entirely — unlike load(), which always sets a NEW path,
  // currentHbin_ goes back to nullopt so hasData()/currentHbin() report "no
  // data loaded" exactly like a fresh session.
  void clearData() {
    // BUG-381: `clear` abandons the head like every other direct assignment
    // here, and load() is not involved, so the reclaim has to be explicit.
    // Nothing can read the dropped head afterwards; a live preserve snapshot,
    // frame or `use` memo is spared by reclaimSupersededScratch's exclusions.
    if (currentHbin_.has_value()) reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
    currentHbin_.reset();
    datastore_.reset();
    currentFile_.clear();  // P8 U1: honest cockpit — no stale source_file after `clear`
    datasetChanged = false;  // BETA A4 (BS-091): nothing loaded -> nothing changed
  }

  // eb-order T-P3: whether an estimation block stands for an e(b). An estpost
  // posting is a block (esttab renders its cells(mean) from it) but Stata posts
  // no e(b) or e(V) there: estpost fills `b`/`V` only before Stata 9
  // (~/Library/Application Support/Stata/ado/plus/e/estpost.ado), so after
  // `estpost summarize` `_b[x1]` is r(111), `matrix list e(b)` r(111) and
  // `matrix b = e(b)` a 0x0 matrix (w2v_estpost_ematrix.log ep13_local_bx1,
  // ep13_list_eb, ep13_b_shape). postPseudoEstimate marks such a block with
  // `"posts_e_b": false`; `erepost b = M` on it posts M and drops the mark.
  static bool blockPostsEb(const nlohmann::ordered_json& block) {
    if (!block.is_object()) return true;
    const auto it = block.find("posts_e_b");
    return it == block.end() || !it->is_boolean() || it->get<bool>();
  }

  // eb-order T-P1: the fit's full covariance, as the engine sent it under
  // `--emit-v` (`e_V` = {names, matrix}, apps/cmds/common.cpp add_e_v), joins
  // the live estimation block -- the one source canonicalEMatrix builds e(V)
  // from. Called right after a fit writes lastEstBlock. Only the session's
  // block carries it: the block a handler returns to the console (and every
  // freeze fixture pins) is a separate copy and stays byte-identical, and
  // eststo's snapshot (makeSnapshot) copies named fields only. A reply with no
  // `e_V` (a fit whose engine command has no --emit-v) leaves the block as it
  // is, and e(V) is then a named refusal, never a diagonal.
  void attachFitCovariance(const nlohmann::ordered_json& raw) {
    if (!lastEstBlock.has_value() || !lastEstBlock->is_object() || !raw.is_object()) return;
    const auto it = raw.find("e_V");
    if (it == raw.end() || !it->is_object()) return;
    (*lastEstBlock)["e_V"] = *it;
  }

  // `clear all`'s macro/model-state half — resets lastModel_ (the fields
  // this milestone's handler surface actually populates; clear_data's full
  // oracle scope also touches last_graph/named_graphs/graphics_off/timers,
  // which have no C++ session-state counterpart yet — M4 concern).
  void lastModel_reset() {
    lastModel_.reset();
    lastEstBlock.reset();
    eval.lastModelResult.reset();
    // AER-SCOPE1 bundle E: the estadd bag is keyed to e(); when e() is reset
    // the bag has nothing to belong to. The fingerprint check in `runEststo`
    // is the OTHER half of this (it catches a REPLACED e(), which never comes
    // through here), not a substitute for it.
    estAddedClear();
    activeEstimatesName.clear();  // W2 esttabnames: e(_estimates_name) goes with e()
    eOnFitNames.clear();  // W2D-T01 round 3: every caller has emptied the e() stores
    ePostedEsampleSerial = 0;  // a posted e(sample) goes with the posted e()
    eNameContract.reset();  // the published name lists go with e()
  }

  const std::string& workdir() const { return workdir_; }

  // -- cwd (BUG-023, FIXED: earlier milestones had the `use` handler
  // bug-for-bug rewrite this to the loaded file's directory, matching a
  // frozen 1.x oracle defect. The 2.0 core no longer does this — `use`
  // leaves cwd untouched, matching real Stata.) ----------
  std::optional<std::string> cwd;  // unset -> caller falls back to getcwd()
  // True once the user has picked the working directory: a `cd`, or a default
  // working directory set in Settings. Opening a do-file then leaves it alone.
  bool cwdChosenByUser = false;

  // -- log using/close (BUG-028, BS-006: real file-backed log) --------------
  // `log using FILE [, replace|append] [text]` really opens FILE now; every
  // subsequent OUTERMOST dispatch() call's finalized blocks get rendered to
  // plain text and appended (dispatcher.cpp's dispatch() wrapper calls
  // logTee() exactly once per top-level command — see its doc comment for
  // why nested/recursive dispatch() calls, e.g. inside `capture`/`quietly`/
  // `by` prefix-stripping or a replayed do-file/loop/program body, must NOT
  // individually re-tee). All PARSING/VALIDATION (replace vs append, the
  // text-vs-smcl format and file-extension rule, "file already exists") lives
  // in handlers_m3.cpp's runLog — this class only owns the open stream + the render-and-write
  // mechanics (session_state.cpp). logIsOpen() is the first thing every one
  // of these checks, so a session that never calls `log using` — the
  // overwhelming majority of fixtures — pays zero cost and stays
  // byte-identical to before this feature (freeze safety).
  bool logIsOpen() const { return !logs_.empty(); }

  // W:P8: `log using FILE, name(X)` opens a SECOND (third, …) transcript
  // alongside whatever is already open — the P8 target opens a named log while
  // its own outer unnamed log is running, and Stata writes the payload to
  // both. These four take the log's name ("" = the unnamed log) so a caller
  // can address exactly one of them; logTee() below fans out to all of them.
  bool logIsOpen(const std::string& name) const {
    for (const auto& l : logs_) {
      if (l->name == name) return true;
    }
    return false;
  }

  // Opens `resolvedPath` (already cwd-resolved by the caller) for writing —
  // append mode when `appendMode`, else truncate. Closes any prior open log
  // first (defensive only: runLog itself refuses a second `log using` while
  // one is already open, matching real Stata, so in normal use logIsOpen()
  // is already false here). Returns false — leaving no log open — if the
  // stream failed to open (bad path/permissions); the caller turns that
  // into an honest error block. Defined in session_state.cpp (needs
  // io/win_compat.h's cecon::to_native_path, kept out of this header).
  //
  // `smcl` selects the SMCL writer (header at open, tagged lines, footer at
  // `log off` / `log close`); false is the plain-text transcript, unchanged.
  bool openLog(const std::string& resolvedPath, bool appendMode,
               const std::string& name = std::string(), int depth = 1, bool smcl = false);

  // Current dispatch recursion depth (1 = outermost). runLog reads it so a
  // named log knows which frames belong to it.
  int dispatchDepth() const { return dispatchDepth_; }

  // Closes one log by name; returns false when no log by that name is open, so
  // the caller can raise Stata's own "log NAME not found". A text log closes at
  // once (ofstream's dtor flushes + closes). An SMCL log keeps its stream until
  // logTee() has written the command's own echo and the closed-on footer (see
  // LogOp); a paused one closes silently, as Stata does.
  bool closeLog(const std::string& name = std::string());
  void closeAllLogs();
  std::vector<std::string> logNames() const {
    std::vector<std::string> out;
    out.reserve(logs_.size());
    for (const auto& l : logs_) out.push_back(l->name);
    return out;
  }
  bool pauseLog(const std::string& name = std::string());
  bool resumeLog(const std::string& name = std::string());

  // Renders one dispatch() frame's finalized blocks array into every open log
  // of that frame's depth -- as the plain-text transcript, or as SMCL for a log
  // opened with the smcl format -- and appends it to the log's stream. Silent
  // no-op when no log is open or the log is paused (`log off`). Defined in
  // session_state.cpp (the generic JSON->text renderer wants <sstream>,
  // kept out of this widely-included header).
  void logTee(const nlohmann::ordered_json& blocks, int depth = 1);
  // tabstat's text table as Stata prints it (the dispatcher's tabstat arm
  // renders it; the emitted block stays the structured one). logTee writes it
  // in place of the generic dump of the next tabstat block and clears it.
  std::string pendingTabstatLogText;

  // -- dispatch-depth guard (BS-006 log tee) --------------------------------
  // dispatcher.cpp's dispatch() wrapper increments this on entry and
  // decrements on exit around EVERY call — including its own many internal
  // recursive calls (capture/quietly/by-prefix stripping, nested do-file/
  // loop/program-body replay) — so it can tell whether a given invocation
  // is the genuine OUTERMOST one (enterDispatch() returning 1): the only
  // call whose finalized return value should ever reach logTee() (see
  // dispatcher.cpp's dispatch()/dispatchImpl() doc comments for the full
  // rationale). Plain int, not atomic/mutex-guarded — a single SessionState
  // is only ever driven by one synchronous call stack at a time (dispatch()
  // recursion is depth-first on one thread; this class already assumes
  // single-threaded access elsewhere, e.g. datastore()/log_).
  // Stage 1b: this pair also brackets the dispatch-active-session install
  // (cobra_runner.h). enterDispatch pushes `this` so the free runCobra() —
  // reached from 90+ handler sites with no SessionState& in scope — routes to
  // this session's persistent worker; exitDispatch pops it. The push/pop nest
  // 1:1 with the depth counter (recursive capture/quietly/by/do-file replay
  // re-push the same session, harmless), and a thread_local stack keeps the UI
  // thread and the do-worker thread independent.
  int enterDispatch() {
    pushActiveSession(this);
    return ++dispatchDepth_;
  }
  int exitDispatch() {
    popActiveSession();
    return --dispatchDepth_;
  }

  // A handler's own temp-column cleanup (`capture drop __touse`) runs under
  // this count. Stata drops an ado's tempvars without running `drop`, so the
  // cleanup leaves r() as the handler left it: the dispatcher's drop/keep r()
  // posting (BUG-1417) skips while it is positive.
  int tempDropDepth = 0;
  struct TempDropScope {
    explicit TempDropScope(SessionState& s) : s_(s) { ++s_.tempDropDepth; }
    ~TempDropScope() { --s_.tempDropDepth; }
    TempDropScope(const TempDropScope&) = delete;
    TempDropScope& operator=(const TempDropScope&) = delete;

   private:
    SessionState& s_;
  };

  // -- xtset / tsset panel/time-series context (session.py's panelvar/
  // timevar) — M3a: read by translate.cpp's TS-context injection
  // (_inject_ts_context) when a regress/feols varlist token uses an L./F./
  // D./S. time-series operator. Unset by default (matches session.py's
  // `panelvar: str | None = None`).
  std::optional<std::string> panelvar;
  std::optional<std::string> timevar;
  // P7 — survival declaration (`stset TIMEVAR, failure(FAIL) id(ID)`). The
  // ANALYSIS variables `_t0/_t/_d/_st` are real columns the engine's stset verb
  // materializes into the dataset (so every later command sees them as ordinary
  // variables, exactly as in Stata); what lives here is only the DECLARATION, so
  // stcox/streg can honest-reject when the data was never stset and so
  // `vce(robust)` can cluster on the subject id the way Stata's does.
  struct StContext {
    std::string timevar;
    std::string failvar;
    std::string idvar;  // empty when stset was given no id()
  };
  std::optional<StContext> stContext;
  // P7 — survey design (`svyset PSU [pweight=W], strata(S)`). Held here rather
  // than rewritten into each `svy:` command line: the design is session state in
  // Stata too, and threading it as context keeps the engine stateless.
  struct SvySettings {
    std::string psu;
    std::string pweight;
    std::string strata;
  };
  std::optional<SvySettings> svyDesign;
  // P7 — multiple-imputation declaration (`mi set STYLE`, `mi register imputed V`).
  // The mlong ROWS are real data written by the engine; this is only the
  // declaration, which is what lets `mi impute` reject honestly when the data was
  // never `mi set` or the target variable was never registered.
  struct MiState {
    std::string style;                     // "mlong" (the only style implemented)
    std::vector<std::string> imputed;      // registered as imputed
    int m = 0;                             // imputations currently present
  };
  std::optional<MiState> miState;
  // P7 — the last `var` fit, kept whole so `vargranger` and `irf create` are pure
  // re-renders of it rather than second implementations of the same mathematics.
  std::optional<nlohmann::ordered_json> lastVar;
  // BS-030 (BETA campaign C2): the user's tsset/xtset `delta()` — read by
  // translate.cpp's injectTsContext to append cobra's own `--tsdelta` flag
  // (apps/cmds/common.cpp's parse_tsmeta) alongside --xtset/--tsset, so
  // L./F./D./S. operators step by this many index units instead of the
  // engine's implicit default of 1. Reset (not just left stale) on every
  // tsset/xtset call, matching real Stata's "each tsset call fully replaces
  // the prior ts state" semantics — see handlers_m3.cpp::runTsDeclare/runXtset.
  std::optional<double> delta;

  // -- frames (P6 wave) -----------------------------------------------------
  // Stata `frame`s are ADDITIONAL named datasets living beside the current one.
  // They are a PIET session concept only: each frame is materialized as its own
  // hbin inside this session's PID-unique workdir, and the engine is never told
  // that "frames" exist — every frame operation is expressed with engine verbs
  // that already exist (eval/merge), so no contract surface moves.
  //
  // NOT to be confused with the engine's serve-mode "frame use", which is an
  // internal hbin-adoption mechanism with an unrelated meaning.
  //
  // The current frame is always `default` in this build: `frame create`,
  // `frame change` and `pwf` are honest-rejected, because "which frame is
  // current" is a product decision with UI consequences (which frame does
  // browse show?) that this wave deliberately does not settle. What IS
  // supported: frame put/drop, the `frame NAME:` prefix, frlink and frget.
  std::map<std::string, std::string> frames;  // frame name -> hbin path
  // frlink's generated link variable -> the frame name it links INTO. `frget`
  // needs this to know which frame to copy a variable out of: its own syntax
  // (`frget NEW=VAR, from(LINKVAR)`) names the link, never the frame.
  std::map<std::string, std::string> frameLinks;

  // -- transient scratch pins (P6 wave) --------------------------------------
  // Files the CURRENTLY RUNNING command still needs, even though the session
  // has since loaded something else. The scratch reclaimer (session_state.cpp,
  // reclaimSupersededScratch) already refuses to evict preserve snapshots, the
  // two `use` memos and live frames; those are all LONG-lived pins keyed by
  // their owning structure. A resampling prefix needs the same protection for
  // the length of ONE command: it writes several scratch files per replicate,
  // which pushes the reclaim budget over, and the oldest files at that moment
  // are exactly its own base dataset and the estimation-sample scratch it is
  // still drawing from. Measured: without this, a 200-replicate permute dies
  // partway through with "cannot open .../ifin_if_*.hbin".
  //
  // Use ScratchPin (below) rather than touching this directly, so a pin can
  // never outlive the command that took it.
  std::set<std::string> scratchPins;

  // RAII pin. Registers paths on construction, removes them on destruction —
  // including on the throw path, which matters because the resampling driver
  // dispatches arbitrary user commands inside its loop.
  class ScratchPin {
   public:
    ScratchPin(SessionState& s, std::vector<std::string> paths)
        : s_(s), paths_(std::move(paths)) {
      for (const auto& p : paths_) {
        if (!p.empty()) s_.scratchPins.insert(p);
      }
    }
    ~ScratchPin() {
      for (const auto& p : paths_) {
        // W2 esttabnames: a path a STORED model also pins stays pinned.
        if (!p.empty() && s_.storedModelPinRefs.count(p) == 0) s_.scratchPins.erase(p);
      }
    }
    ScratchPin(const ScratchPin&) = delete;
    ScratchPin& operator=(const ScratchPin&) = delete;

   private:
    SessionState& s_;
    std::vector<std::string> paths_;
  };
  // BUG-389 (2026-09-02): a handler that FORGETS a scratch file (`frame drop`)
  // hands it to the one reclaim primitive so every pin and the budget policy
  // apply to it, instead of leaving an orphan no ledger can reach. Public
  // forwarder; the primitive stays private (its other callers reason about the
  // head, this one about a path the caller has just un-registered).
  void reclaimScratchFile(const std::string& path) { reclaimSupersededScratch(path); }

  // BUG-389 (2026-09-02): the ORPHAN sweep. At every outermost dispatch exit,
  // after the projection ledger has been drained, every regular file left under
  // workdir_ that no ledger owns — not the head, not a preserve twin, not a
  // `use` memo (hbin or its label sidecar), not a frame, not a running command's
  // ScratchPin, not a superseded/projection/tempfile ledger entry, not the last
  // model's fit data, not an open postfile target or file handle — is dead by
  // definition and is unlinked. Subdirectories carrying another owner's
  // `.piet_owner_pid` (parallel workers) are skipped whole. Runs only when a
  // mint happened since the previous sweep (scratchMintSeq), so a command that
  // minted nothing pays nothing. PIET_NO_ORPHAN_SWEEP=1 is the A/B arm.
  // Returns the number of files removed; the census at exit (PIET_SCRATCH_CENSUS)
  // is the gate: ORPHAN must read 0 on every probe shape of the LEAK AUDIT §1.
  //
  // e(sample) T2: the files another LIVE SessionState of this process owns are
  // not orphans either. The mint registry is process-wide, so two sessions
  // sharing one workdir (the selftest harness's blocks: an outer session and
  // short-lived siblings beside it) saw each other's on-disk head as unowned:
  // the sibling's first dispatch exit unlinked it, and the outer session's
  // next Datastore open died on "a scratch file that was never written".
  // Latent until the estimation-sample install put a fresh pinned head on disk
  // after every fit. The registry (registerLiveSession/~SessionState) is the
  // spare; a session that has ended owns nothing and is swept as before.
  int sweepOrphanScratch(const char* site);
  int lastOrphanSweepRemoved() const { return lastOrphanSweepRemoved_; }
  void registerLiveSession();

  // BUG-390 (2026-09-02): the `use` memos are bounded by BYTES. Cap =
  // PIET_USE_CACHE_MB, else CECON_MEMORY_BUDGET_MB / 2, else 1024 MB; least
  // recently `use`d entries are evicted first (conv hbin → scratch GC; stash →
  // engine `stashdrop`). The most recent entry always survives.
  std::uintmax_t useCacheBytes() const;
  std::size_t useCacheEntryCount() const { return useConvCache_.size() + useStashCache_.size(); }
  static std::uintmax_t useCacheCapBytes();
  // Forget every `use` memo (conv + stash) and release their files/stashes.
  void dropUseCaches();

  // BUG-392 (2026-09-02): the rest of Stata's `clear all` — stored estimates,
  // named graphs, open postfiles (discarded, never written: `postutil clear`),
  // open file handles (closed), every frame and link (`frames reset`), the
  // `use` memos (BUG-390) and the engine's resident frame. NOT the preserve
  // stack (StataBE 19.5: `restore` after `clear all` is rc 0) and not the logs.
  void clearAllSessionObjects();

  // -- c()/set registry (BETA A4: BS-046/047/048/091/101) -------------------
  // Session-scoped `set` state (resets each session, unlike piet::settings'
  // process-persistent app-shell config) — the single source both the `set`
  // dispatch arm (dispatcher.cpp) WRITES and the c()-class resolver
  // (piet/cclass.h) + the estimation `--level` flow (buildEstimationArgvM4 /
  // handlers.cpp::runRegress / runTtest) READ.
  //
  // level: `set level #` / `set clevel #` (BS-048) — the session default
  // confidence level consulted ONLY when a command's own `level(#)` option
  // is absent. Defaults to 95 (Stata's/cobra's own factory default), so
  // every caller's fallback ("append --level only when session.level != 95")
  // is a byte-identical no-op until the user actually runs `set level`.
  int level = 95;
  // The macro-expanded text of the command the innermost dispatch tokenized
  // (dispatcher.cpp), cleared at every dispatch entry. The fit hook's default
  // e(cmdline) reads it: the line a prefix re-dispatches can still be the
  // unexpanded `c' (handlers_aer_estim.cpp postDefaultCmdline).
  std::string leafCmdExpanded;
  // BUG-471 T2: `set type float|double` — the storage type an UNTYPED
  // generate / egen / bysort-gen takes (Stata's c(type); factory default
  // float, w2e_float_default.log settype_ctype_default :1236). Session-scoped
  // like `level`: `clear` / `clear all` leave it alone, and `, permanently`
  // writes no settings file (a headless do-file must not reconfigure the
  // app). piet/storage_type.h is the one reader that turns it into a store.
  std::string defaultStorageType = "float";
  // EM1: `set more on|off` -> c(more). Stata 19 starts with more off
  // (w2af_macro_text_file.log:328 mt_cmore_init `<off>`).
  bool moreOn = false;
  // BUG-471 T2: how many `replace`s in this session resolved their store type
  // through the unrecorded-creator fallback (no varMeta stataType and not on
  // implicitFloatVars -> double, no rounding). The count measures how many
  // creators still record nothing (SPEC.md 4.3's sibling list).
  int64_t unrecordedReplaceFallbacks = 0;
  // AER-SCOPE1 bundle D (ENV, 2026-09-03): `sysdir set NAME "path"` and
  // `adopath + path` are the first two lines of a great many AER do-files --
  // twenty-seven of the ninety packages between them. They tell `findfile`, and
  // the ado autoload (BUG-442), where the package expected its own files to be,
  // which is what downstream lines actually read back. Recording them costs two fields and
  // turns an accept-and-forget note into an accept-and-remember one.
  //
  // Seeded with Stata's own directory names so `sysdir` bare prints the six
  // rows a user expects; adopath starts as Stata's default search order with
  // "." meaning the session cwd, resolved at lookup time rather than here
  // (cwd moves).
  std::map<std::string, std::string> sysdir;
  std::vector<std::string> adopath;
  // graphicsOff: `set graphics on|off` (BS-047) — read by graph_m4.cpp's
  // twoway/graph_combine builders to populate the wire-schema `suppressed`
  // field (previously hardcoded false; see graph_m4.cpp's own comment).
  bool graphicsOff = false;
  // seed: `set seed #` (BS-046) — persisted for c(seed) readback. No cobra
  // command in this build's current dispatch surface consumes a session-
  // level default seed yet (wildboot/mediate/bootstrap land in later D-phase
  // waves per the wildboot fixed-seed+distribution parity strategy) — this
  // is state-tracking + c(seed) exposure only, inert otherwise until then.
  //
  // It reads exactly like the std::optional<long long> it used to be (every
  // reader: has_value(), value_or(), `*seed`, assignment from a long long),
  // plus epoch(): a count of `set seed` assignments. The macro layer's
  // runiform() (EvalSession::rngSeed) needs it to tell `set seed 7` issued
  // AGAIN from no `set seed` at all -- the value alone cannot, and Stata
  // restarts the stream on every `set seed`.
  //
  // stream(): Stata's session RNG itself (core/rng_mt64.h, mt64 = MT19937-64,
  // bit-exact against StataBE). Every `set seed` re-initializes it
  // (init_genrand64), and every consumer that draws the way Stata's session RNG
  // does -- generate/replace/egen/keep if with runiform()/runiformint()/
  // uniform()/rnormal(), the macro layer's runiform(), bsample, boottest, the
  // lasso CV folds -- takes its draws from here and advances it, so draws
  // continue across commands exactly as Stata's stream does. Before any
  // `set seed` it is Stata's start-up stream (seed 123456789).
  class SeedSlot {
   public:
    SeedSlot& operator=(long long v) {
      value_ = v;
      ++epoch_;
      stream_.seed(static_cast<uint64_t>(v));
      return *this;
    }
    cecon::Mt64& stream() { return stream_; }
    const cecon::Mt64& stream() const { return stream_; }
    bool has_value() const { return value_.has_value(); }
    explicit operator bool() const { return value_.has_value(); }
    long long operator*() const { return *value_; }
    long long value_or(long long fallback) const { return value_.value_or(fallback); }
    const std::optional<long long>& get() const { return value_; }
    unsigned long long epoch() const { return epoch_; }

   private:
    std::optional<long long> value_;
    unsigned long long epoch_ = 0;
    cecon::Mt64 stream_;
  };
  SeedSlot seed;
  // B3(c): a per-call counter that rides beside the seed so two successive
  // `gen x = runiform()` lines do not draw the same column. The engine is a
  // subprocess and remembers nothing, so the session owns this; a run is
  // reproducible from `set seed` because the counter advances deterministically.
  long long rngStream = 0;
  // datasetChanged: c(changed) (BS-091) best-effort dirty bit. Sourced from
  // the ONE existing choke point every mutation already funnels through:
  // load() sets it true (every gen/replace/drop/.../mutation reloads via
  // load()); a fresh `use` (setCurrentFile(), called only on success) and a
  // successful `save` (runSaveData) reset it false, matching Stata's own
  // "changed since load/save" semantics for the common cases. `clear` also
  // resets it (nothing loaded -> nothing changed).
  bool datasetChanged = false;

  // -- last model / last estimation block -----------------------------------
  const std::optional<LastModel>& lastModel() const { return lastModel_; }

  // e(sample) T2 (internal note esample/SPEC.md §4.2, §5): the ONE
  // reading of the token `e(sample)` that every layer consults -- the row
  // rewrite (dispatcher.cpp rewriteEsampleToken) and the macro/arith layer's
  // obs-1 hook (EvalSession::esampleObs1, installed in the constructor below).
  //   kZero    -- no estimation results (no LastModel: a fresh session or
  //               lastModel_reset(), the only producer of "no model"; a
  //               posted e() has no LastModel either), or a model whose mask
  //               column has left the data (`use`, `clear`, `collapse`,
  //               `reshape`, save + use): Stata answers 0 (w2_esample.log
  //               A1-A5, E9-E11, E14, E15, G5, G11).
  //   kColumn  -- the column `__piet_esample__` on the working frame is this
  //               model's mask.
  //   kRefused -- the model has no mask PIET can vouch for
  //               (LastModel::EsampleState kUnavailable/kStale): `refusal` is
  //               the r(198) text, naming the command.
  struct EsampleReading {
    enum class Kind { kZero, kColumn, kRefused };
    Kind kind = Kind::kZero;
    std::string refusal;
    unsigned long long serial = 0;  // kColumn: the value an admitted row holds
  };
  EsampleReading esampleReading();
  // Observation 1 of e(sample) -- Stata's value for a variable in a scalar
  // expression (`display e(sample)`, `local s = e(sample)`, `if e(sample) {`;
  // w2_esample.log B16-B19, B29-B29c). nullopt = refused, `why` gets the text.
  std::optional<int> esampleObs1(std::string* why);
  // BUG-718: `VAR[idx]` for the macro layer (EvalSession::varObs) -- the value
  // `display VAR[idx]` prints, read by display_hardening's one reader. Defined in
  // session_state.cpp so this header does not pull in display_hardening.h.
  std::optional<eval::EvalSession::VarObsReading> macroVarObs(const std::string& name,
                                                               long long idx);
  // BUG-1006: `` `c(name)' `` for the macro layer (EvalSession::cValue) -- the
  // raw text of piet::resolveCValue. Defined in session_state.cpp so this header
  // does not pull in cclass.h (which includes this one).
  std::optional<std::string> macroCValue(const std::string& name);
  // e(sample) T3/T4: re-point the LIVE model's e(sample) without reinstalling
  // it. setLastModel() is a new model (it drops the e() keys written on top of
  // the fit, e(_estimates_name), a posted e()); `estimates restore`, `estimates
  // esample:`, `estimates use` and the ML mask install that follows a logit/
  // probit fit (dispatcher.cpp) change only which rows e(sample) is. `dataPath`
  // moves only when the install names the on-disk base of an unmoved frame
  // (handlers.cpp esampleInstallOnHead); pass the model's own otherwise.
  void setLastModelEsample(LastModel::EsampleState state, unsigned long long serial,
                           std::string installPath, std::string dataPath) {
    if (!lastModel_.has_value()) return;
    lastModel_->esample = state;
    lastModel_->esampleSerial = serial;
    lastModel_->esampleInstallPath = std::move(installPath);
    lastModel_->dataPath = std::move(dataPath);
  }
  // How many times a fit installed the mask column in this session
  // (handlers.cpp esampleInstallColumn). Zero means no frame of this session
  // can carry it, so a writer that must leave it out (save) skips the check.
  unsigned long long esampleColumnInstalls = 0;
  // Every load() of a new head, counted (the install's equivalence test below).
  unsigned long long headLoadCount = 0;
  // The last plain mask install (handlers.cpp esampleInstallOnHead): the frame it
  // produced, the ON-DISK frame it was built from, and headLoadCount right after
  // its load. While the head is still that frame (same path, no load since), it
  // IS the base frame plus the hidden column -- same rows, same values -- so:
  //  * the next fit names the base as its frame (LastModel::dataPath) instead of
  //    writing the install frame to disk first; and
  //  * a read-only engine call replaying the base (test/lincom/margins after the
  //    fit) is served from the resident head (esampleBaseAlias), not by flushing
  //    the head and re-reading the base.
  // Before this, every wired fit wrote the whole working frame (review 2026-10-13:
  // 20 x `regress` on 1M rows, 56 -> 240 ms per fit, a 32 MB flush each).
  // The weight expression of the command being dispatched (ParsedCommand::
  // weightExp), set by the dispatcher after tokenize: a fit's LastModel takes it
  // as fitWexp in setLastModel(), which no fit handler has to pass along.
  std::string commandWexp;
  std::string esampleLastInstallFrame;
  std::string esampleLastInstallBase;
  unsigned long long esampleLastInstallLoads = 0;
  // ... and its serial and the engine's 0/1 file (2 bytes a row): a next fit on
  // that unmoved frame whose file is byte-identical installs nothing.
  unsigned long long esampleLastInstallSerial = 0;
  std::string esampleLastInstallBody;
  long long esampleLastInstallRows = 0;  // the install frame's rows (lines of that file)
  // The install adds only the HIDDEN column, which no card lists (cobra_session.cpp
  // skips it at seeding), so a card current for the frame it was built on
  // describes the new frame too: same rows, same user columns. Retagged, so a
  // later verb reads the card instead of opening the install frame as a
  // Datastore (which writes it to disk: `replace x2 = ...` after every fit,
  // review p9). The setcol's delta folds into no card.
  void carryCardAcrossHiddenInstall(const std::string& from, const std::string& to) {
    if (shadowValid_ && shadowCardHbin_ == from) shadowCardHbin_ = to;
    pendingDeltaValid_ = false;
  }
  bool esampleHeadIsInstallOf(const std::string& base) const {
    return !esampleLastInstallFrame.empty() && !base.empty() &&
           base == esampleLastInstallBase && currentHbin_.has_value() &&
           *currentHbin_ == esampleLastInstallFrame && headLoadCount == esampleLastInstallLoads;
  }
  // The install moves the working frame to a new path (the old frame plus the
  // mask column). Before it, no estimation command ever moved the head, and a
  // driver that captured the head path around an inner fit relies on that:
  // bootstrap/jackknife/permute record `startHbin`, dispatch the command once
  // for the observed statistic, then resample from and return to that path
  // (handlers_resample.cpp). The supersede inside the install's load() would
  // reclaim it there ("run failed: cannot open .../esample_install_*.hbin",
  // measured in selftest W2C(postest) `bootstrap ...: regress`). So the
  // pre-install head is PINNED: spared by reclaimSupersededScratch and the
  // orphan sweep until the head next moves for any other reason (load()) or the
  // outermost dispatch exits (dispatcher.cpp), when it is offered to the
  // ordinary reclaim -- which still spares it if anything else owns it.
  void pinPreInstallHead() {
    if (currentHbin_.has_value() && !currentHbin_->empty()) {
      preInstallPins_.push_back(*currentHbin_);
      preInstallLoadPending_ = true;
    }
  }
  // True when `canonicalPath` (weakly canonical, u8string) is the on-disk base of the live install
  // frame (esampleHeadIsInstallOf): the file the next fit and the read-only
  // replays alias, so scratch reclaim and the orphan sweep must not take it.
  bool isLiveEsampleBase(const std::string& canonicalPath) const;
  void releasePreInstallPins() {
    if (preInstallPins_.empty()) return;
    std::vector<std::string> pins;
    pins.swap(preInstallPins_);
    for (const auto& p : pins) {
      if (currentHbin_.has_value() && *currentHbin_ == p) continue;  // the head again
      reclaimSupersededScratch(p);
    }
  }

  // BUG-733 (W2D-T01 round 2): the lifetime of a POSTED e(). `ereturn post`
  // (handlers_ereturn.cpp) and an estpost pseudo-estimate (handlers_aer_tables.cpp
  // postPseudoEstimate; dispatcher.cpp sets the flag after a successful
  // estpost) install a whole new e() into eval.eScalars / eStrings / eMatrices
  // with no fitted model behind it, and every e() reader prefers those stores
  // over the model result (e(N), e(df_r), `matrix c = e(b)`, colsof(e(b))).
  // Stata replaces ALL of e() on the next estimation, so the next model
  // install must drop the posting: before this, `ereturn post p, obs(999)`
  // then `regress` on 60 obs answered e(N) 999 and the posted 1 x 2 e(b) in
  // both reader layers, and an `estpost` on 60 rows answered e(N) 60 for a
  // later fit on 40 (w2d_uncovered.log:2501 records 40).
  //
  // ePostedLive is true from a posting until the next setLastModel (or
  // `ereturn clear`). The clear sits at the top of setLastModel because no
  // ordinary fit writes the e() stores before it calls setLastModel; the three
  // that do (runPca, runOaxaca, runTeffects) are wrapped in dispatcher.cpp
  // (runFitWritingEFirst), which ends the posting and the recorded names below
  // BEFORE the fit writes anything, puts both back when no model was installed
  // (lastModelInstalls unchanged), and on a successful install records every
  // e() name the fit left (FU-375) so the NEXT install drops them too. The
  // dispatcher's runPostestKeepingE keeps a posting and the recorded names
  // across test/testparm/lincom/nlcom/estat/erepost, which may re-enter
  // setLastModel on the SAME model. Still OPEN (owned elsewhere): estadd's
  // live writes on top of a fit (the BUG-467 estadd half) and the eclass
  // program's `ereturn scalar` flush (program.cpp) are not recorded, so they
  // outlive the next fit.
  bool ePostedLive = false;
  // Stata's ordered e() name lists (`: e(scalars)`, `: e(macros)`,
  // `: e(matrices)`) for ONE fit, published by the fit handler for the paths
  // whose whole Stata namespace it posts (estimation_m4.cpp
  // installENameContractM4). Stata lists names in its e() store's own order
  // (StataBE 19.5: macros newest-first, matrices `b V` then newest-first,
  // scalars the store's order), which PIET's lazily resolved e() cannot
  // reproduce from its stores, so the fit names the order. A kind left nullopt
  // is not claimed and keeps the honest refusal. `missingScalars` are listed
  // names Stata posts with a missing value (ivregress's e(df_r)), which
  // resolve to `.` without a store entry. Read only through eNameOrderFor.
  struct ENameContract {
    std::uint64_t fitSerial = 0;
    std::string cmd;
    std::optional<std::vector<std::string>> scalars, macros, matrices;
    std::vector<std::string> missingScalars;
  };
  std::optional<ENameContract> eNameContract;
  // The contract's list for `kind`, only while the live e() is still exactly
  // that fit's posting: the same fit serial, no `ereturn post`/estpost on top,
  // the same e(cmd), every listed name resolving, and no store holding a name
  // the list lacks (an estadd'ed scalar, say). Otherwise nullopt, and the
  // caller falls back to the completeness rule above. session_state.cpp.
  std::optional<std::vector<std::string>> eNameOrderFor(const std::string& kind) const;
  // `set rng NAME` (dispatcher.cpp), read back by c(rng).
  std::string rngSetting = "default";
  // `ereturn post ..., esample(VAR)` (handlers_ereturn.cpp runPost): the posted
  // e() has no LastModel, so its e(sample) is the mask column installed under
  // THIS serial (the same `__piet_esample__` column and serial scheme a fit
  // uses, SessionState::esampleColumnInstalls). 0 = the posting has no mask
  // (e(sample) is 0, as Stata answers after a post without esample()). Read
  // only while ePostedLive; reset with every e() replacement (lastModel_reset,
  // supersedePostedE) and carried by estimates store/restore (StoredModel).
  unsigned long long ePostedEsampleSerial = 0;
  unsigned long long lastModelInstalls = 0;
  // W2D-T01 round 3: the e() names an eclass program wrote with `ereturn local`
  // while no posting was live -- on top of the installed fit, or of an empty e().
  // Stata keeps them until the next model is fit (help return: estimation results
  // persist until the next model is fit) and leaves them alone on
  // test/lincom/nlcom/estat/erepost of the same model (dispatcher.cpp
  // runPostestKeepingE). Dropped at every install (setLastModel, and before the
  // pre-install writers in runFitWritingEFirst); emptied with the stores
  // (lastModel_reset); carried by `estimates store`/`restore` (StoredModel).
  // runFitWritingEFirst also records here every e() name a pca/oaxaca/teffects
  // fit wrote before its install (FU-375). A name is erased from all three
  // stores: the e() namespace is one namespace (eval/session.h setEString /
  // setEScalar), and at a new install every old entry under it is stale.
  std::vector<std::string> eOnFitNames;
  void noteEOnFit(const std::string& name) {
    if (std::find(eOnFitNames.begin(), eOnFitNames.end(), name) == eOnFitNames.end())
      eOnFitNames.push_back(name);
  }
  void dropEOnFit() {
    for (const auto& n : eOnFitNames) {
      eval.eStrings.erase(n);
      eval.eScalars.erase(n);
      eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                          [&](const auto& p) { return p.first == n; }),
                           eval.eMatrices.end());
    }
    eOnFitNames.clear();
  }
  void supersedePostedE() {
    if (!ePostedLive) return;
    eval.eScalars = eval::OrderedFloatDict();
    eval.eStrings.clear();
    eval.eMatrices.clear();
    ePostedLive = false;
    ePostedEsampleSerial = 0;
  }

  // Mirrors the result JSON into eval.lastModelResult: the macro layer's
  // _b[]/_se[]/e() resolver (macros.cpp resolveEstScalarRefs) reads the
  // embedded EvalSession, not lastModel_, so scalar/local/if expressions
  // must see the same estimation the display path sees.
  void setLastModel(LastModel m) {
    if (m.fitWexp.empty() && !m.fitWeight.empty()) m.fitWexp = commandWexp;
    supersedePostedE();  // BUG-733: a posted e() ends here (see above)
    dropEOnFit();        // round 3: so do keys written on top of the previous fit
    ++lastModelInstalls;
    // eb-order T-P3 part 0: a NEW e() (another fit serial, or none live) owns
    // no estimation block and no e(b)/e(V) until it writes its own. Every fit
    // that has an e(b) writes its block AFTER this call (csdid, oaxaca and
    // teffects included) and posts its e(b)/e(V) after it, so a fit that has
    // none (pca, rdrobust, rddensity) leaves no block and the previous fit's
    // can never answer for it. A copy of the live model keeps both.
    const bool sameFit = lastModel_.has_value() && lastModel_->fitSerial == m.fitSerial;
    if (!sameFit) {
      // A new fit replaces e() whole, so what `estadd` attached to the previous
      // e() goes with it -- also when the new fit repeats the same specification,
      // which the bag's fingerprint (depvar|estimator|N|k) cannot tell apart
      // (w2ak_estout_family: `estadd r(mean)` at do:191, then `regress y x1 x2`
      // at :221 left e(mean) live). estadd mirrored the additions into the e()
      // stores (mirrorIntoEStores), so they leave those too. A re-install of the
      // SAME fit (test, lincom) keeps them.
      dropEstAddedMirrors();
      estAddedClear();
      lastEstBlock.reset();
      eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                          [](const auto& p) { return p.first == "b" || p.first == "V"; }),
                           eval.eMatrices.end());
    }
    // W2 esttabnames: a new e() is not a stored one. `estimates restore` sets
    // the name again AFTER calling here (estimates.ado Store/Restore keep the
    // hidden e(_estimates_name) on the results, so a fresh fit has none).
    // W2AK: a re-install of the SAME fit (a post-estimation command such as
    // `test` copying the live model back) keeps it -- Stata's e() is untouched
    // there, so a later `estadd` still reaches the stored copy (w2ak ek_d01).
    if (!lastModel_.has_value() || lastModel_->fitSerial != m.fitSerial) {
      activeEstimatesName.clear();
    }
    lastModel_ = std::move(m);
    // BUG-518 (W2 predictframe, 2026-09-19): the frame a model was FIT on must
    // exist ON DISK for as long as the model is live. Every fit handler records
    // `lm.dataPath = *session.currentHbin()`, and after a resident `generate`
    // that path is a dirty VIRTUAL head -- the fit read it straight from the
    // worker's frame and nothing ever wrote it. The next resident mutation then
    // advances the worker's frame in place, and `test`, `lincom`, `predict`
    // (which replay `--data`/`--fit-data lm.dataPath`) all fail with
    // `[data_io] cannot open .../bysort_gen_*.hbin` (measured on the frozen main
    // binary: `regress y x2 t` / `generate foo = 1` / `test x2` -> rc 1) where
    // Stata is rc 0 (w2_smalls.log section C's shape with the `generate` moved
    // in front of the first predict; BUG-518.do). reclaimSupersededScratch's
    // exclusion (7) already keeps the file ALIVE; this is the other half, the
    // one that makes it EXIST. A no-op unless the head is that dirty virtual
    // frame (a fit on a clean head, a `dataOverride` slice, an `estimates
    // restore` of an older path all fall through), so it costs exactly one
    // snapshot per estimation on a dirty head -- the flush a later `predict` /
    // `sort` / `save` would have paid as "stateless-data" anyway, moved
    // forward to the moment the model starts naming the frame. Tagged for the
    // PIET_ROUTELOG census so the added flushes can be counted by site.
    if (residentDirty_ && !lastModel_->dataPath.empty() &&
        lastModel_->dataPath == residentToken_) {
      ensureMaterializedHead("lastmodel-pin");
    }
    eval.lastModelResult = lastModel_->result;
    // BUG-458: e(cmd)/e(depvar)/e(vce) are PUBLISHED into the e-macro store
    // here, the one point every estimation handler already passes through, so
    // the delimited form `e(cmd)', the bare form e(cmd) (BUG-456) and `ereturn
    // list` (which now reads the store) cannot disagree. Before this, only
    // pca/oaxaca/estadd wrote the store and `e(cmd)' was EMPTY after regress
    // while `ereturn list` printed "feols" (internal notes bug456-20260908 E0/E1).
    // An empty depvar (pca) ERASES the macro rather than publishing "".
    eval.setEString("cmd", lastModel_->stataCmd.empty() ? lastModel_->cobraCmd
                                                        : lastModel_->stataCmd);
    eval.setEString("depvar", lastModel_->depvar);
    // W2 estimopts (BUG-536/537/538), the e() macros a table writer reads
    // after a robust/clustered/weighted fit, published in STATA's spellings.
    // lm.vcov keeps the engine's ("hc1"/"cluster"/"twoway"/"multiway") because the
    // replay machinery keys on it; only the macro store translates.
    // w2_pweight.log PSTR records: R2 `robust|Robust||pweight|= w`,
    // R4/X1 `cluster|Robust|id|...`, R1 `...|aweight|= w`. Anything else
    // (iid and every non-linear spelling) passes through unchanged in e(vce).
    //
    // e(clustvar) is published for EVERY fit: the cluster names under a
    // cluster VCE, erased (setEString("") erases) otherwise. Nothing else in
    // the tree writes it, and no fit clusters under a non-engine spelling
    // (nbreg records `--vce cluster` as "cluster"), so a later robust nbreg,
    // pca, oaxaca or teffects cannot inherit a clustered fit's variable.
    //
    // e(vcetype): the engine's spellings answer here ("Robust" / erased).
    // Of the non-engine spellings, only teffects (stataCmd "teffects",
    // lm.vcov "robust") publishes its own -- `AI robust`,
    // aer_estim2_oracle.log INFO|nn|e(vcetype) -- BEFORE calling here, so it
    // is left untouched. Every other one (nbreg's engine `--vce robust`,
    // oaxaca "robust", pca "none") is ERASED: none of them has a captured
    // e(vcetype) arm, and erased is what this store answered for them before
    // e(vcetype) was published at all, where keeping the previous fit's
    // `Robust` would be a stale macro nobody computed for this fit.
    {
      const std::string& v = lastModel_->vcov;
      const bool robustLike = (v == "hc1");
      const bool clusterLike = (v == "cluster" || v == "twoway" || v == "multiway");
      const bool engineSpelling = v.empty() || v == "iid" || robustLike || clusterLike;
      eval.setEString("vce", lastModel_->eVce.has_value()
                                 ? *lastModel_->eVce
                                 : robustLike ? std::string("robust")
                                              : clusterLike ? std::string("cluster") : v);
      if (lastModel_->eVce.has_value() && *lastModel_->eVce == "linearized") {
        // a `svy:` fit (cacheLastModelM4): StataBE 19.5 `svy: regress` e(vce)
        // linearized, e(vcetype) Linearized (BUG-1513)
        eval.setEString("vcetype", "Linearized");
      } else if (engineSpelling) {
        eval.setEString("vcetype", (robustLike || clusterLike) ? "Robust" : "");
      } else if (lastModel_->stataCmd != "teffects") {
        eval.setEString("vcetype", "");
      }
      std::string clustvar;
      if (clusterLike) {
        for (const auto& c : lastModel_->cluster) {
          if (!clustvar.empty()) clustvar += ' ';
          clustvar += c;
        }
        // cobra's two-way and multiway spellings are one comma-joined "A,B[,C...]"
        // token; Stata's e(clustvar) is "A B [C...]".
        for (auto& ch : clustvar) {
          if (ch == ',') ch = ' ';
        }
      }
      eval.setEString("clustvar", clustvar);
      const std::string& wt = lastModel_->fitWtype;
      const std::string wtype = wt == "aw"   ? "aweight"
                                : wt == "fw" ? "fweight"
                                : wt == "iw" ? "iweight"
                                : wt == "pw" ? "pweight"
                                             : std::string();
      eval.setEString("wtype", wtype);
      // The ivreg2-family ados store "=" + the expression with no blank after
      // the equals sign (StataBE 19.5, BUG-1530: ivreg2/xtivreg2/ivreghdfe
      // `[aw=wf]` and `[aw = wf]` -> "=wf", `[aw=wf ]` -> "=wf "), where regress,
      // reghdfe, ivregress and areg store "= wf". A first stage stored by
      // savefirst (cobraCmd feols) is left as it was.
      const std::string& sc = lastModel_->stataCmd;
      const bool ivreg2Family =
          (sc == "ivreg2" || sc == "xtivreg2" || sc == "ivreghdfe") && lastModel_->cobraCmd != "feols";
      eval.setEString("wexp", wtype.empty() ? std::string()
                                             : (ivreg2Family ? "=" : "= ") +
                                                   (lastModel_->fitWexp.empty()
                                                        ? lastModel_->fitWeight
                                                        : lastModel_->fitWexp));
      // reghdfe's e(resid) names the residual column of ONE fit (BUG-539);
      // runRegress re-posts it after this call when the option was given. A
      // re-install of the SAME fit (setEstHandleM4, reached by lincom/test)
      // keeps it (w2n_followups.log:673/:682/:741 D02/D03/D06: e(resid) is
      // still `_reghdfe_resid` / `myres` after `quietly lincom x1`).
      if (!sameFit) eval.setEString("resid", "");
    }
  }

  // FLOOR metric ② denominator (A1-METRICS-RATIFICATION §2.2 · B2 NAMED-OWED
  // ③ · PHASE-B-DECISION-MEMO §6-3): Σ payload/serialized bytes of the CLOSED,
  // positively-enumerated set of LIVE NAMED artifacts this session is
  // contractually obligated to hold. This is the honest denominator a2_seqbench
  // needs for `ws = RSS / live-named-bytes`, replacing the input-.dta-size
  // STAND-IN it uses today (A2 confession (b)). Enumerated positively as the
  // anti-gaming spine (§2.2): TRANSIENT buffers (the per-row map rebind, the
  // level deep-copy, per-command scratch) are the NUMERATOR's waste and are
  // deliberately EXCLUDED — counting them as entitlement is exactly how you
  // would game `ws` downward.
  //
  // Called TWO ways, both behind the PIET_EMIT_COUNTERS gate (env-off it never
  // runs, so --do stdout stays byte-identical to every captured freeze fixture —
  // the report-only safety property the whole counters seam is built on):
  //   · END SNAPSHOT (peakSampleMode=false) — walked once at the emit site
  //     (main.cpp runDoFileHeadless), reported as `live_named_bytes`.
  //   · PEAK SAMPLE (peakSampleMode=true) — walked once per top-level item by
  //     liveNamedBytesPeakSample() to feed the running max `live_named_bytes_peak`
  //     (the ratified ② denominator, PHASE-B-DECISION-MEMO §6-3 ADDENDUM). In
  //     that mode the resident-frame block is SKIPPED while a dirty virtual head
  //     is pending, so a mid-run sample never forces the expensive head
  //     materialisation (that is exactly the re-materialisation the resident
  //     plane avoids, and it would perturb snapshot_count); a clean-boundary
  //     sample captures the frame and the running max keeps it.
  // Report-only either way: NO floors.json budget (Eval pen — budgeting is a
  // separate Eval-ratified follow-up).
  //
  // COST (FIX-PIET-COUNTERTAX-PEAKSAMPLE): the frame block below walks column
  // METADATA only — never the payload, and never the VarInfo card, whose
  // n_missing scan is what made the per-item peak sample cost a2 +11.6% wall
  // (instruments/alloc_churn_scoping.md §1). Anything added here must keep
  // that property; a per-item diagnostic that reads payload is a tax on every
  // counters-on measurement the campaign quotes. selftest pins it
  // (FIX-PIET-COUNTERTAX-PEAKSAMPLE gate: fullScanCount()==0 across a walk).
  int64_t liveNamedBytes(bool peakSampleMode = false) {
    int64_t total = 0;

    // 1. resident frame — Σ column buffers: f64 8B·rows, factor 4B·rows codes
    //    PLUS its level dictionary once (§2.2). datastore() may materialise a
    //    virtual resident head here; harmless at the emit site (the run is
    //    complete, nothing observable runs after) and unreachable env-off.
    //    Guarded: a diagnostic must never crash a completed run.
    try {
      if (peakSampleMode && residentDirty_) {
        // peak sample over a dirty head — see the method doc: skip, don't
        // materialise mid-run.
      } else if (Datastore* ds = datastore()) {
        const int64_t rows = ds->nRows();
        // METADATA-ONLY column walk (FIX-PIET-COUNTERTAX-PEAKSAMPLE). This used
        // to iterate `ds->varlist()`, i.e. it built the VarInfo CARD for every
        // column — and buildVarInfo() computes `n_missing`, an O(nrows) NaN/
        // code scan that faults the column's ENTIRE mmap payload back in
        // (datastore.cpp Datastore::nMissing). n_missing is not in the §2.2
        // enumeration at all: this walk needs only dtype, nRows and the level
        // dictionary, all of which are metadata already resident in the
        // Dataset's column table. The card was pure tax, and under the peak
        // sample it was paid once PER TOP-LEVEL ITEM: measured on a2_mini
        // (instruments/alloc_churn_scoping.md §1, interleaved on/off A/B ×2)
        // PIET_EMIT_COUNTERS=1 cost +11.6% wall / +45% PIET wall / +98k page
        // faults for only +0.79s CPU — fault-wait, not compute.
        // The VALUE is unchanged: names() is built from the same
        // ds_.columns() varlist() walks, in the same order; the dtype test and
        // the level strings summed are the same ones buildVarInfo() copies a
        // view of.
        for (const std::string& cname : ds->names()) {
          const cecon::Column& c = ds->col(cname);
          if (c.dtype == cecon::DType::Factor) {
            total += 4 * rows;
            // The column's OWN level vector — the same strings a VarInfo's
            // LevelsRef views, read without the owning copy that bumps
            // levelStringCopies() (datastore.h), so this read is still free of
            // the selftest tripwire.
            for (const std::string& lvl : c.levels) {
              total += static_cast<int64_t>(lvl.size());
            }
          } else {
            total += 8 * rows;  // f64
          }
        }
      }
    } catch (...) {
      // resident-frame bytes unavailable (materialise failed) — report the
      // rest rather than aborting the whole run's counters block.
    }

    // 2. eststo / esttab store — Σ serialized bytes of each stored model
    //    snapshot (§2.2, session_state.h estimates vector-of-pairs).
    for (const auto& kv : estimates) {
      total += static_cast<int64_t>(kv.first.size() + kv.second.dump().size());
    }

    // 3. graph gallery — Σ serialized bytes of each named graph spec (§2.2,
    //    namedGraphs map).
    for (const auto& kv : namedGraphs) {
      total += static_cast<int64_t>(kv.first.size() + kv.second.dump().size());
    }
    total += sersets.bytes();  // serset-twoway SS: the graphs' sersets

    // 4. open postfiles — Σ open post-buffer bytes accumulating until postclose
    //    (§2.2): the parallel name/type arrays + every staged (kind,text) cell.
    for (const auto& kv : postfiles) {
      const PostHandle& ph = kv.second;
      total += static_cast<int64_t>(kv.first.size());
      for (const auto& s : ph.names) total += static_cast<int64_t>(s.size());
      for (const auto& s : ph.types) total += static_cast<int64_t>(s.size());
      for (const auto& row : ph.rows) {
        for (const auto& cell : row) {
          total += static_cast<int64_t>(cell.first.size() + cell.second.size());
        }
      }
    }

    // 5. active model — the one live estimation result, distinct from the named
    //    `estimates` store (§2.2 "active model").
    if (lastModel_.has_value() && lastModel_->result.has_value()) {
      total += static_cast<int64_t>(lastModel_->result->dump().size());
    }

    // 6. scalars / matrices / macros — small, but named + retained ⇒ counted
    //    (§2.2, session.eval.*). r()/e() transient stores are excluded (not
    //    user-named persistent entitlement).
    auto dictBytes = [](const auto& d) -> int64_t {
      int64_t b = 0;
      for (const auto& kv : d.items()) {
        b += static_cast<int64_t>(kv.first.size() + kv.second.size());
      }
      return b;
    };
    total += dictBytes(eval.locals);   // _locals macros
    total += dictBytes(eval.globals);  // _globals macros
    total += dictBytes(eval.scalars);  // named scalars
    for (const auto& kv : eval.matrices) {
      total += static_cast<int64_t>(kv.first.size());
      for (const auto& r : kv.second.rownames) total += static_cast<int64_t>(r.size());
      for (const auto& c : kv.second.colnames) total += static_cast<int64_t>(c.size());
      for (const auto& mrow : kv.second.data) {
        total += static_cast<int64_t>(mrow.size()) * 8;  // f64 cells
      }
    }

    return total;
  }

  // Counter- AND cache-neutral walk of liveNamedBytes(), shared by BOTH
  // diagnostic callers (below). liveNamedBytes() may lazily (re)construct the
  // Datastore for the current head — bumping datastore_constructs, a BUDGETED
  // floor_gate counter, and (via a rebuilt mmap) touched_bytes — and, when the
  // head is a DIRTY virtual frame (end-snapshot mode only; the peak sample
  // skips that state), the construct's ensureMaterializedHead performs a REAL
  // frameSnapshot: snapshot_count + snapshot_bytes + a C4 trace line. It also
  // leaves datastore_ warm, which would SUPPRESS a later legitimate
  // reconstruction (the cache-state leak). ALL of it is neutralised here —
  // the save/restore set is EVERY counter the PIET_EMIT_COUNTERS block emits
  // (FIX-C4COUNTER-SAMPLERSPAN/BUG-142: the original set predated the C4-L1A
  // keys, so a walk-induced closure half-recorded — snapshot_bytes kept the
  // walk's bytes while snapshot_count was restored, and the trace line fired
  // attributed to whatever verb happened to be dispatched last). The C4 trace
  // is suppressed for the walk's span so trace lines and snapshot_count stay
  // the same series (the L1B trace==count cross-check invariant); the
  // use-plane keys cannot fire inside a walk today and are included as the
  // "invisible to every reported counter" doctrine, not because a site exists.
  // Runs only under the caller's PIET_EMIT_COUNTERS gate.
  int64_t liveNamedBytesNeutral(bool peakSampleMode) {
    const int64_t savedConstructs = Datastore::constructCount();
    const int64_t savedSnap = counters::g_snapshotCount;
    const int64_t savedTouched = counters::g_touchedBytes;
    const int64_t savedSetcol = counters::g_setcolBytes;
    const int64_t savedSnapBytes = counters::g_snapshotBytes;
    const int64_t savedUseDisk = counters::g_useDiskLoads;
    const int64_t savedUseVirtual = counters::g_useVirtualHits;
    const int64_t savedDtaConverts = counters::g_dtaConverts;
    const bool wasNull = (datastore_ == nullptr);

    c4TraceSuppress_ = true;  // a neutralised closure must not emit a trace line
    const int64_t v = liveNamedBytes(peakSampleMode);
    c4TraceSuppress_ = false;

    if (wasNull) datastore_.reset();  // restore exact cache state (was empty)
    Datastore::setConstructCount(savedConstructs);
    counters::g_snapshotCount = savedSnap;
    counters::g_touchedBytes = savedTouched;
    counters::g_setcolBytes = savedSetcol;
    counters::g_snapshotBytes = savedSnapBytes;
    counters::g_useDiskLoads = savedUseDisk;
    counters::g_useVirtualHits = savedUseVirtual;
    counters::g_dtaConverts = savedDtaConverts;
    return v;
  }

  // PEAK SAMPLE — per-top-level-item high-water accumulator in doexec.cpp
  // (B2 NAMED-OWED ③). Skips a dirty virtual head (never materialises mid-run).
  int64_t liveNamedBytesPeakSample() { return liveNamedBytesNeutral(/*peakSampleMode=*/true); }

  // END SNAPSHOT — the emit site's walk (main.cpp runDoFileHeadless,
  // `live_named_bytes`). MAY materialise a dirty head for an accurate value
  // (the run is complete, nothing observable runs after) — but through the
  // neutral wrapper, so the walk's own physical snapshot is invisible to every
  // reported counter and to the trace (FIX-C4COUNTER-SAMPLERSPAN/BUG-142: the
  // emit block captures snapshot_count/datastore_constructs/touched_bytes
  // BEFORE this walk and snapshot_bytes AFTER it, so an un-neutralised walk
  // half-records its own closure into whichever keys are captured later).
  int64_t liveNamedBytesEndSnapshot() { return liveNamedBytesNeutral(/*peakSampleMode=*/false); }

  std::optional<nlohmann::ordered_json> lastEstBlock;  // session._last_est_block

  // -- last graph block (M4b: session.py's session.last_graph) -------------
  // Set by the dispatcher immediately after any graph command succeeds
  // (scatter/histogram/kdensity/line/twoway/xtline/tsline/marginsplot/
  // coefplot/graph_combine). `graph export`/`graph save` read this. Named
  // graphs (session._named_graphs, `graph save NAME`) are a separate map
  // keyed by name; `graph combine` composites named-graph blocks into a
  // grid panel.
  std::optional<nlohmann::ordered_json> lastGraph;
  std::map<std::string, nlohmann::ordered_json> namedGraphs;
  // graph-family AI-4: per graph in memory, what `graph describe` reports
  // ({family, command, scheme, ysize, xsize, date, time}; serset::makeGraphMeta),
  // and the name of the current graph (the last drawn, displayed or used one).
  std::map<std::string, nlohmann::ordered_json> graphMeta;
  std::string currentGraph;
  // serset-twoway SS: the sersets of the graphs in memory (piet/serset.h). Every
  // graph registration (serset::registerGraph) replaces its graph's sersets;
  // `graph drop`, `serset clear` and `clear all` drop them.
  serset::Store sersets;

  // -- last margins result (M4a, marginsplot) — session._last_margins /
  // _last_margins_dydx: cached after each successful `margins` run so
  // `marginsplot` can plot it without re-running cobra.
  std::optional<nlohmann::ordered_json> lastMargins;
  std::vector<std::string> lastMarginsDydx;

  // -- the last successful `correlate`'s r(C): a correlate that finds no or one
  // usable row posts it again beside r(N) = 0 (StataBE 19.5, 2026-11-03).
  std::optional<eval::EvalMatrix> lastCorrelateC;

  // -- history ---------------------------------------------------------------
  void addHistory(const std::string& cmd, bool success);
  nlohmann::ordered_json historyJson() const;

  // -- variable-editor metadata (session._var_meta / _var_lblname / value labels)
  // BUG-295: names created in-session by an UNDECLARED numeric `gen`/`egen`.
  // Stata's gen defaults to float, so `gen z = _n` must write float into a
  // .dta — but PIET's frame is all-f64 and the writer's narrowest-lossless
  // scan (the right rule for a column whose provenance is a scanned import)
  // stored `byte`. The claim is kept OFF varMeta.stataType deliberately: this
  // is an implicit default, not a declaration, and only the .dta export plane
  // reads it. Pruned by forgetVarPlanes with every other varname-keyed plane.
  std::set<std::string> implicitFloatVars;
  std::map<std::string, VarMeta> varMeta;                    // insertion order not
                                                               // behavior-relevant here
  // BS-005 (B-data): vars carrying a USER-issued `format` command — a subset of
  // varMeta (which ALSO holds .dta-loaded default formats via applyDtaMeta).
  // Since format-display-default-lifecycle no reader gates on it (browse/list
  // take every column's format from displayFormat); kept as the record of
  // which formats a `format` command set.
  std::set<std::string> userFormattedVars;
  std::map<std::string, std::string> varLblname;              // varname -> label-set name
  std::map<std::string, std::map<std::string, std::string>> labelDefs;  // lblname -> {code:text}
  std::map<std::string, std::map<int, std::string>> valueLabels;        // varname -> {code:text}
  // Stata lists value-label sets most-recently defined/added/modified FIRST
  // (`label dir`, bare `label list`; w2ao_label_family.log:1301 lb_dir `zz_lbl
  // keep_lbl negative_lbl grade_drop`, created in the reverse order). The
  // `label define` stamp of each set; a set without one (read from a file) is
  // older than every stamped set. Only the order readers consult it.
  std::map<std::string, unsigned long long> labelSeq;
  unsigned long long labelSeqNext = 0;
  // LBL-LANGUAGE ([D] label language): each language holds its own variable
  // labels and value-label ATTACHMENTS (the label sets themselves are shared).
  // The current language's live in varMeta/varLblname; the others are stashed
  // here. `labelLangs` is the creation order (r(languages)).
  struct LabelLangStore {
    std::string dataLabel;
    std::map<std::string, std::string> varLabels;
    std::map<std::string, std::string> varLblname;
  };
  std::string labelLang = "default";
  std::vector<std::string> labelLangs{"default"};
  std::map<std::string, LabelLangStore> labelLangStash;
  void stampLabelSet(const std::string& name) { labelSeq[name] = ++labelSeqNext; }
  // The defined set names in Stata's listing order (newest first; unstamped
  // sets after the stamped ones, alphabetically).
  std::vector<std::string> labelSetOrder() const {
    std::vector<std::string> names;
    for (const auto& kv : labelDefs) names.push_back(kv.first);
    std::stable_sort(names.begin(), names.end(), [this](const std::string& a, const std::string& b) {
      const auto ia = labelSeq.find(a), ib = labelSeq.find(b);
      const unsigned long long sa = ia == labelSeq.end() ? 0 : ia->second;
      const unsigned long long sb = ib == labelSeq.end() ? 0 : ib->second;
      return sa > sb;
    });
    return names;
  }

  // FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: the two blocks a .dta's own metadata
  // carries that applyDtaMeta() used to leave unread (handlers.cpp) — see that
  // function's doc comment for why a `use A.dta` -> `save B.dta` round trip
  // silently lost both until now. dataLabel is Stata's `label data`;
  // characteristics is the flat `char` table (dataset-level "_dta" entries and
  // per-variable ones together, exactly cobra's --out-meta/--in-meta shape).
  std::string dataLabel;
  std::vector<DtaCharacteristic> characteristics;

  // session.py's get_value_label_map(): reshape the varname->{code:text}
  // value-label store into the {varname -> [(code,text)]} vector-of-pairs
  // shape Datastore::browse() consumes (a pure container-shape adapter —
  // the C++ browse() signature took a vector-of-pairs instead of a nested
  // map). Consumed by every browse-backed dispatch (list/browse, the
  // /browse route, duplicates list) so value-labeled numeric f64 columns
  // display their label text and carry a `value_labels` column-meta
  // sidecar, matching the 1.16.0 oracle's ds.browse(..., value_label_map=)
  // on ALL paths — BUG-025 fixed list/browse but the duplicates-list
  // filtered path kept passing {}.
  std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>>
  valueLabelMapForBrowse() const {
    std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>> out;
    out.reserve(valueLabels.size());
    for (const auto& [varname, codeMap] : valueLabels) {
      std::vector<std::pair<int, std::string>> pairs;
      pairs.reserve(codeMap.size());
      for (const auto& [code, text] : codeMap) pairs.emplace_back(code, text);
      out.emplace_back(varname, std::move(pairs));
    }
    return out;
  }

  // ── format-display-default-lifecycle: the ONE display-format resolver ─────
  //
  // "The display format of variable v", for every reader: `: format` (the
  // eval projection, syncEvalVarLabels), describe, codebook, list/browse, the
  // varlist cards (varlistWithMetaFrom), tsset's current format and the
  // var_detail route. In order:
  //   1. the format this session holds for v (varMeta[v].format: a `format`
  //      command, a .dta's or an .hbin header's own format adopted at load,
  //      or a creation default a verb pinned before it changed v's type);
  //   2. the format the card's file states (VarInfo::formatStated);
  //   3. Stata's default for v's DECLARED storage type (defaultDisplayFormat):
  //      byte/int %8.0g, long %12.0g, float %9.0g, double %10.0g, str# %#s
  //      with a minimum width of 9, strL %9s ([D] format; w2p_estim_ev.log
  //      :1725-1827). No claim: %9.0g numeric, %9s string.
  // Step 3 is DERIVED at read time and never stored: hbinMetaPatch writes only
  // step 1, so a saved header states no format the session did not hold (the
  // BUG-198 absent-vs-empty law) and the reload derives the same default from
  // the type the checkpoint carries (adoptCheckpointTsDecl).
  static std::string defaultDisplayFormat(const std::optional<std::string>& stataType,
                                          bool isString);
  std::string displayFormat(const VarInfo& v) const;
  // {name -> resolved format} for the given card entries (all of them), the
  // shape Datastore::browse() takes: every listed column carries the format
  // its cells are displayed in, not only the ones a `format` command set.
  std::vector<std::pair<std::string, std::string>> displayFormatMap(
      const std::vector<VarInfo>& card) const;
  // Before a verb changes v's storage type: hold the format v displays now as
  // the session's own (step 1 above), because Stata keeps the format a variable
  // was created with (`recast double` of a float stays %9.0g, a widening
  // replace of a byte keeps %8.0g: internal capture
  // exprfns_contract_oracle.log:735/:736). No-op when v already has one.
  void pinDisplayFormat(const VarInfo& v);
  // Before a verb changes v's storage type (recast, compress): a numeric keeps
  // the format it displays now (pinDisplayFormat); a string's DEFAULT format
  // follows its new width instead (`recast str20` of a str4 displays %20s,
  // `compress` of a str20 holding "ab" leaves str2 %9s, and a replace that
  // widens str12 to str14 displays %14s: w2af_format_lifecycle.log
  // af_recast_str4_str20, af_compress, af_replace_str12_str14), so a held
  // format equal to the old width's default is dropped and an explicit one
  // (%-12s) stays.
  void prepareStorageChange(const VarInfo& v);

  void setVarLabel(const std::string& varname, const std::string& label) {
    varMeta[varname].label = label;
  }

  // ── BUG-208: the extended-macro-function label store ──────────────────────
  //
  // `local l : variable label x` is answered in the eval layer, which reads
  // `eval.varLabels` (eval/macros.cpp). That OrderedDict had exactly ONE writer
  // in the whole tree — the evalcase test harness — so in real use it was a
  // permanently empty map and the function returned "" for a label the same
  // session had just set and `describe` was displaying. A legal value, no
  // diagnostic, and `if "\`l'" == ""` is the idiomatic use, so do-files took the
  // wrong branch silently. Present in the shipped B.10.0 build.
  //
  // THE DESIGN QUESTION, and why it is answered this way. Two stores for one
  // fact is what caused this, so the fix must not leave two. `piet::eval` is
  // deliberately layered BELOW SessionState (see dispatcher.cpp's note that
  // tryCaptureDefinition "only sees the narrower EvalSession, not full
  // SessionState"), so the reader cannot simply be pointed at varMeta without
  // inverting that dependency. Mirroring on write in setVarLabel was rejected
  // for the opposite reason: it would have fixed the `label variable` verb ONLY,
  // leaving .dta-loaded labels, .hbin-header labels, rename-carried labels and
  // preserve/restore still answering "" — i.e. it would have converted one
  // honest total failure into a partial one that is harder to notice.
  //
  // So eval.varLabels stops being a store and becomes a PROJECTION of the one
  // live source, refreshed from varlistWithMeta()'s resolved card — the exact
  // fallback chain `describe` renders. That makes the macro function agree with
  // `describe` BY CONSTRUCTION rather than by two code paths staying in step,
  // and it covers every writer of varMeta uniformly instead of one of them.
  //
  // Cost: this is called ONLY from the dispatcher's extended-macro precondition
  // (a cheap textual gate), never on the hot path, and it prefers the resident
  // card so it constructs no Datastore when the card is current — the MAXPERF
  // verb-overhead selftest gates count exactly that and must not move.
  //
  // ── BUG-216/217 widening ──────────────────────────────────────────────────
  //
  // BUG-208's projection carried the LABEL only, and it inserted a variable only
  // when that label was non-empty. So at the eval layer "exists with no label"
  // and "does not exist" were the SAME INPUT — literally indistinguishable —
  // which is why `: variable label nosuchvar` could not answer Stata's r(111)
  // and had to return "" (BUG-217). Nor could `: format` / `: value label` be
  // implemented at all: the facts were not in the projection (BUG-216).
  //
  // So the projection now carries, per variable, an EXISTENCE marker plus the
  // format and value-label-set name, under the reserved key prefixes below.
  // What changed is the FILTER, not the rebuild discipline: it is still cleared
  // and rebuilt wholesale, for exactly the reason BUG-208 gave — a dropped
  // variable, or one whose label was cleared, must DISAPPEAR, and a merge would
  // let a stale fact outlive the thing it describes, which is the same defect
  // class the projection was introduced to fix.
  //
  // Reserved-key carriage rather than new EvalSession fields: `piet::eval` sits
  // below SessionState and this keeps that boundary where BUG-208 put it. The
  // convention is already established in this tree — eval/macros.cpp carries
  // s-class values in the r()-strings store under kSClassKeyPrefix ("\x01s:")
  // for the same reason. The `\x01` lead byte cannot collide with a variable
  // name (isValidStataIdentifier rejects it), and eval.varLabels has no
  // enumerating reader anywhere — its only two readers are the `: variable
  // label` arm and the evalcase harness — so reserved keys are inert to
  // everything else.
  //
  // `: type` IS carried since BUG-250's close. The dispatcher's gen/egen arms
  // now record the DECLARED subtype at creation into VarMeta::stataType (the
  // .dta import path already did, handlers.cpp:254), and the projection below
  // carries it under kEvalVarTypePrefix with describe's own fallback: a
  // numeric column with no declaration projects "float" (also Stata's `gen`
  // default), a string column with no recorded width projects NOTHING — its
  // true `str#` is not known anywhere in this engine, and the reader keeps
  // the honest r(101) for exactly that remainder (macros.cpp reVarType).
  //
  // kEvalVarPlaneKey is the AUTHORITY sentinel, and it is what makes the reject
  // one-sided. Present = a SessionState has projected this plane, so a missing
  // exists-key really does mean the variable is absent and r(111) is decidable.
  // Absent = nobody synced (the evalcase harness and the session-less selftest
  // callers write eval.varLabels by hand), and there the eval layer falls back to
  // the pre-fix lenient answer rather than inventing an error. So a harness
  // fixture can never be turned into an r(111) by this change.
  static constexpr const char* kEvalVarExistsPrefix = "\x01v:x:";   // -> "1"
  static constexpr const char* kEvalVarFormatPrefix = "\x01v:f:";   // -> "%9.3f"
  static constexpr const char* kEvalVarLblsetPrefix = "\x01v:l:";   // -> "mylbl"
  static constexpr const char* kEvalVarTypePrefix = "\x01v:t:";     // -> "double" (BUG-250)
  static constexpr const char* kEvalVarPlaneKey = "\x01v:@";        // -> "1"
  void syncEvalVarLabels();

  // ── BUG-198: the .hbin format/label carriage plane ────────────────────────
  //
  // `format x %12.4f` and `label variable x "…"` write HERE and nowhere else —
  // not into the engine's frame, not into the scratch .hbin. So a `save x.hbin`
  // (either arm: resident snapshot, or clonefile/copy of the scratch head)
  // produced a file that had never heard of them, and the very next
  // `use x.hbin` came back with the pre-override format and label at rc 0.
  // Values were always intact; this is the metadata half.
  //
  // The fix is a pair: state the overrides INTO the saved file's header
  // (hbinMetaPatch, consumed by cecon::patch_hbin_meta at the one place the
  // save plane has produced its bytes) and read them back OUT into this store
  // on load (seedVarMetaFromCard). Both directions are needed — a header
  // nobody reads back is the same silent loss one hop later.

  // What the file being saved should say about each of `card`'s columns.
  // An entry appears only for a column this session has an opinion about; the
  // engaged tri-state is `std::nullopt` for a key with no override, so a column
  // whose format is declared and whose label is not leaves the label key
  // EXACTLY as the writer wrote it instead of erasing it.
  std::vector<cecon::ColMetaPatch> hbinMetaPatch(const std::vector<VarInfo>& card) const;

  // Cheap "is there anything to state at all" — true iff hbinMetaPatch(card)
  // would be non-empty. Used by the save plane to decide whether a minted
  // tempfile may still take the no-disk virtual arm.
  bool hasHbinMetaOverrides(const std::vector<VarInfo>& card) const;

  // The load direction: adopt the header's own format/label keys as this
  // session's overrides. Deliberately mirrors applyDtaMeta()'s shape — it
  // populates varMeta and NOT `userFormattedVars`, exactly as the .dta plane
  // has always done, so both load planes agree on what "the user formatted
  // this" means and no browse/list output moves. Reads the RAW header strings
  // off the Datastore's own columns, never the rendered card ("" = the key was
  // absent, never a value; VarInfo::format has already defaulted it).
  void seedVarMetaFromLoadedFile(Datastore* ds);

  // A minted tempfile saved through the virtual (RAM-only) arm has no file
  // whose header could carry the layer, so the layer rides beside the entry
  // and is re-applied when `use` restores it — the same metadata the disk arm
  // would have round-tripped through the header, by construction (both are
  // built by hbinMetaPatch).
  void noteVirtualSaveMeta(const std::string& path, std::vector<cecon::ColMetaPatch> meta) {
    virtualSaveMeta_[path] = std::move(meta);
  }
  void seedVarMetaFromVirtualSave(const std::string& path);
  // format-display-default-lifecycle: merge and append bring in a using
  // variable with the format the using file states for it (Stata's tempfile is
  // a .dta: w2af_format_lifecycle.log af_merge `double|%7.3f`). A virtual save
  // states it only here, beside the path, so the variables in `names` that hold
  // no format of their own adopt the one recorded for `path`.
  void adoptVirtualSaveFormats(const std::string& path, const std::vector<std::string>& names);
  // The whole format/label layer recorded beside a virtual save (nullptr when
  // `path` has none): merge/append/joinby read a using variable's label too.
  const std::vector<cecon::ColMetaPatch>* virtualSaveMetaOf(const std::string& path) const {
    auto it = virtualSaveMeta_.find(path);
    return it == virtualSaveMeta_.end() ? nullptr : &it->second;
  }
  // BUG-291: every varname-keyed metadata plane must forget a column the moment
  // that column leaves the frame. Before this existed, the only eraser in the
  // tree was rename's (handlers_data_verbs.cpp), so `gen double x` -> `drop x`
  // -> `gen x = 1` left the stale `double` claim behind and `describe x`
  // answered double where Stata (19.5 IC, measured) answers float. labelDefs is
  // NOT pruned here: it is keyed by label-set name, not varname, and Stata keeps
  // a defined label set alive after the variables using it are dropped.
  void forgetVarPlanes(const std::string& name) {
    varMeta.erase(name);
    implicitFloatVars.erase(name);
    userFormattedVars.erase(name);
    varLblname.erase(name);
    valueLabels.erase(name);
  }

  void clearMetadata() {
    varMeta.clear();
    implicitFloatVars.clear();
    userFormattedVars.clear();
    labelDefs.clear();
    labelLang = "default";
    labelLangs = {"default"};
    labelLangStash.clear();
    varLblname.clear();
    valueLabels.clear();
    dataLabel.clear();
    characteristics.clear();
    // xtset-file-persistence: the tsset/xtset declaration IS a set of `_dta`
    // characteristics in Stata (_TSpanel/_TStvar/_TSdelta/iis/tis), so a verb
    // that drops the char table drops the declaration with it — `clear`,
    // `import`, `filelist`, and `use` (which re-adopts it from the file's own
    // chars; see adoptTsFromChars). Before this, panelvar/timevar survived every
    // dataset replacement and `xtreg, fe` after `use` of an un-xtset file ran on
    // the PREVIOUS dataset's panel id (w2_eststo_prefix.log:441, rc 459 there).
    panelvar.reset();
    timevar.reset();
    delta.reset();
  }

  // -- xtset-file-persistence: the declaration <-> `_dta` char table ---------
  // Invariant kept by every writer: session.characteristics' ts entries state
  // exactly panelvar/timevar/delta, so `save x.dta` (verbatim char carry,
  // writeDtaMetaSidecar) writes the declaration into the file and `preserve`
  // snapshots it. Names and value shapes are Stata's own, read off a StataBE-
  // written file (aer-packages/087 Dataset_Bowling_Replication_JPE.dta):
  // _TSpanel/iis = panel var, _TStvar/tis = time var, _TSdelta = %21x of the
  // delta ("+1.0000000000000X+000"). _TSitrvl is erased on re-declaration but
  // never written: its semantics are not pinned by any oracle here.
  static bool isTsCharName(const std::string& n) {
    return n == "_TSpanel" || n == "_TStvar" || n == "_TSdelta" || n == "_TSitrvl" ||
           n == "iis" || n == "tis";
  }
  // Stata's %21x spelling: [+-]1.<13 hex digits>X[+-]<3 hex digits of exponent>.
  static std::string stataHex21(double v) {
    char buf[48];
    if (v == 0.0 || !std::isfinite(v)) {
      std::snprintf(buf, sizeof(buf), "+0.0000000000000X-3ff");
      return buf;
    }
    int e = 0;
    const double m = std::frexp(std::fabs(v), &e) * 2.0;  // [1, 2)
    --e;
    const auto frac = static_cast<unsigned long long>(std::ldexp(m - 1.0, 52));
    std::snprintf(buf, sizeof(buf), "%c1.%013llxX%c%03x", v < 0 ? '-' : '+', frac,
                  e < 0 ? '-' : '+', static_cast<unsigned>(e < 0 ? -e : e));
    return buf;
  }
  static std::optional<double> parseStataHex21(const std::string& s) {
    const size_t x = s.find_first_of("Xx");
    if (s.size() < 6 || x == std::string::npos || x + 2 > s.size()) return std::nullopt;
    size_t i = 0;
    double sign = 1.0;
    if (s[i] == '+' || s[i] == '-') sign = (s[i++] == '-') ? -1.0 : 1.0;
    auto hexv = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (i >= x || hexv(s[i]) < 0) return std::nullopt;
    double mant = hexv(s[i++]);
    if (i < x && s[i] == '.') ++i;
    double scale = 1.0 / 16.0;
    for (; i < x; ++i, scale /= 16.0) {
      const int d = hexv(s[i]);
      if (d < 0) return std::nullopt;
      mant += d * scale;
    }
    size_t j = x + 1;
    int esign = 1;
    if (s[j] == '+' || s[j] == '-') esign = (s[j++] == '-') ? -1 : 1;
    if (j >= s.size()) return std::nullopt;
    long ex = 0;
    for (; j < s.size(); ++j) {
      const int d = hexv(s[j]);
      if (d < 0) return std::nullopt;
      ex = ex * 16 + d;
    }
    return sign * std::ldexp(mant, static_cast<int>(esign * ex));
  }
  const std::string* dtaChar(const std::string& name) const {
    for (const auto& c : characteristics)
      if (c.varname == "_dta" && c.charname == name) return &c.value;
    return nullptr;
  }
  // xtset/tsset (and every carry of the declaration across a load whose file
  // format has no char table): restate the ts entries from panelvar/timevar/delta.
  void writeTsChars() {
    characteristics.erase(
        std::remove_if(characteristics.begin(), characteristics.end(),
                       [](const DtaCharacteristic& c) {
                         return c.varname == "_dta" && isTsCharName(c.charname);
                       }),
        characteristics.end());
    const bool hasP = panelvar.has_value() && !panelvar->empty();
    const bool hasT = timevar.has_value() && !timevar->empty();
    if (hasP) {
      characteristics.push_back(DtaCharacteristic{"_dta", "iis", *panelvar});
      characteristics.push_back(DtaCharacteristic{"_dta", "_TSpanel", *panelvar});
    }
    if (hasT) {
      characteristics.push_back(DtaCharacteristic{"_dta", "tis", *timevar});
      characteristics.push_back(
          DtaCharacteristic{"_dta", "_TSdelta", stataHex21(delta.value_or(1.0))});
      characteristics.push_back(DtaCharacteristic{"_dta", "_TStvar", *timevar});
    }
  }
  // `use X.dta` / `restore`: the declaration is whatever the char table says —
  // cleared when the table states none (w2_eststo_prefix.log:441 B1 rc 459,
  // :478-496 B2 restored). _TSpanel/_TStvar win; iis/tis are the pre-Stata-10
  // spellings a file may carry alone. A delta of exactly 1 stays unset (the
  // engine's own default — same argv as a plain `xtset id t`).
  void adoptTsFromChars() {
    panelvar.reset();
    timevar.reset();
    delta.reset();
    auto pick = [&](const char* primary, const char* legacy) -> std::optional<std::string> {
      const std::string* v = dtaChar(primary);
      if (v == nullptr || v->empty()) v = dtaChar(legacy);
      if (v == nullptr || v->empty()) return std::nullopt;
      return *v;
    };
    panelvar = pick("_TSpanel", "iis");
    timevar = pick("_TStvar", "tis");
    if (timevar.has_value()) {
      if (const std::string* d = dtaChar("_TSdelta")) {
        std::optional<double> dv = parseStataHex21(*d);
        if (dv.has_value() && *dv > 0.0 && *dv != 1.0) delta = dv;
      }
    }
  }

  // BUG-659: a native `.hbin` / tempfile `.tmp` has no char table, so the
  // declaration live at `save` rides beside the checkpoint's path instead (the
  // BUG-198 virtualSaveMeta_ shape, for both the virtual and the disk arm).
  // Stata's tempfile IS a .dta whose _dta chars carry it: a checkpoint saved
  // declared comes back declared, one saved un-declared comes back un-declared
  // (w2_eststo_prefix.log:478-500 B2 applied to the tempfile container).
  //
  // BUG-922 (FU-752/FU-792): the same record carries the rest of what a .dta
  // states and an hbin header cannot: each variable's declared storage type,
  // the value-label attachments and definitions, and the dataset label. Stata's
  // tempfile IS a .dta, so `save `t'` / `use `t', clear` must hand back
  // double/long/byte/int/str#, `: value label b` and the label set exactly
  // (oracle w2_prog_controlflow pc_tf_use_types, pc_tf_use_labels, pc_tf_rehop).
  // The hbin header keeps carrying format and variable label (hbinMetaPatch /
  // seedVarMetaFromVirtualSave); this record is the other half, recorded by
  // the save plane's two arms through the one call below (noteCheckpointTsDecl,
  // virtual and disk alike) and restated by `use` through the one call both of
  // its planes make (adoptCheckpointTsDecl, after clearMetadata). Before, the
  // types fell back to float and the labels were lost.
  struct CheckpointTsDecl {
    std::optional<std::string> panelvar;
    std::optional<std::string> timevar;
    std::optional<double> delta;
    std::map<std::string, std::string> stataTypes;  // variable -> declared storage type
    std::set<std::string> implicitFloat;            // gen's implicit float (implicitFloatVars)
    std::map<std::string, std::string> varLblname;  // variable -> value-label set name
    std::map<std::string, std::map<std::string, std::string>> labelDefs;
    std::map<std::string, std::map<int, std::string>> valueLabels;
    std::string dataLabel;
    // The char table (notes `_dta[note1]`, `x[note0]`, user chars): a native
    // checkpoint has no table of its own, so it rides beside the path like the
    // label layer (w2aw_data_combine_family.log dc_ml_notes: notes saved to a
    // tempfile are there after `use`).
    std::vector<DtaCharacteristic> characteristics;
    std::string labelLang;
    std::vector<std::string> labelLangs;
    std::map<std::string, LabelLangStore> labelLangStash;
  };
  void noteCheckpointTsDecl(const std::string& path) {
    CheckpointTsDecl ck{panelvar, timevar, delta};
    for (const auto& [name, vm] : varMeta)
      if (vm.stataType.has_value() && !vm.stataType->empty()) ck.stataTypes[name] = *vm.stataType;
    ck.implicitFloat = implicitFloatVars;
    ck.varLblname = varLblname;
    ck.labelDefs = labelDefs;
    ck.valueLabels = valueLabels;
    ck.dataLabel = dataLabel;
    ck.characteristics = characteristics;
    ck.labelLang = labelLang;
    ck.labelLangs = labelLangs;
    ck.labelLangStash = labelLangStash;
    checkpointTsDecl_[path] = std::move(ck);
  }
  // `use` of a native checkpoint: false when this session never saved `path`
  // (the caller then keeps its own rule); true after restating the declaration
  // recorded at save time, including "none", and (BUG-922) the type and
  // value-label layer recorded with it.
  bool adoptCheckpointTsDecl(const std::string& path) {
    auto it = checkpointTsDecl_.find(path);
    if (it == checkpointTsDecl_.end()) return false;
    panelvar = it->second.panelvar;
    timevar = it->second.timevar;
    delta = it->second.delta;
    characteristics = it->second.characteristics;
    writeTsChars();
    for (const auto& [name, t] : it->second.stataTypes) varMeta[name].stataType = t;
    implicitFloatVars = it->second.implicitFloat;
    varLblname = it->second.varLblname;
    labelDefs = it->second.labelDefs;
    valueLabels = it->second.valueLabels;
    dataLabel = it->second.dataLabel;
    labelLang = it->second.labelLang;
    labelLangs = it->second.labelLangs;
    labelLangStash = it->second.labelLangStash;
    return true;
  }
  // BUG-922: the storage types a native checkpoint recorded at its save (empty
  // when this session never saved `path`) -- the tempfile counterpart of the
  // .dta sidecar's stata_type, for a verb that reads `using` a checkpoint.
  std::map<std::string, std::string> checkpointStataTypes(const std::string& path) const {
    auto it = checkpointTsDecl_.find(path);
    return it == checkpointTsDecl_.end() ? std::map<std::string, std::string>{}
                                         : it->second.stataTypes;
  }
  // A native checkpoint's recorded declaration, or nullptr (merge's using side).
  const CheckpointTsDecl* checkpointTsDecl(const std::string& path) const {
    auto it = checkpointTsDecl_.find(path);
    return it == checkpointTsDecl_.end() ? nullptr : &it->second;
  }

  // varlist_with_meta: Datastore::varlist() augmented with label/format overrides.
  std::vector<VarInfo> varlistWithMeta(Datastore* ds) const;
  // Same label/value-label remap as varlistWithMeta(), but over an explicit
  // base list — the resident shadow card, so a mutation can render
  // varlist_after WITHOUT re-mmapping the (virtual) head (Stage 1b §4).
  std::vector<VarInfo> varlistWithMetaFrom(const std::vector<VarInfo>& base) const;

  // -- Stage 1b: persistent-worker intercept + resident-frame data plane -----
  // The routing target the free runCobra() calls when this session is
  // dispatch-active. Owns: stateless vs frame routing, the append/merge
  // stderr-count carve-out, virtual-head advance, and the one-shot fallback
  // whenever the worker is unavailable. CobraRunResult stays byte-compatible
  // so every caller (and runCobraJson) is untouched. Definition in
  // session_state.cpp.
  CobraRunResult runViaCobraSession(const std::vector<std::string>& argv);

  // Snapshot a VIRTUAL resident head to its currentHbin_ path (then it is a
  // real file). No-op when the head is clean, when frames are unused, or when
  // the resident frame no longer corresponds to currentHbin_ (a divergent
  // intermediate temp is in flight — currentHbin_ is then already a real file,
  // and snapshotting would clobber it with the wrong frame). Idempotent and
  // cheap on the clean path; call it before ANY use of currentHbin_ as a file.
  // `c4Site` is trace attribution ONLY (C4-L1 window trace, PIET_C4_TRACE):
  // which caller closed the dirty window. Callers not yet tagged keep the
  // default; it never changes behavior.
  void ensureMaterializedHead(const char* c4Site = "untagged");

  // C4-L1 window-trace attribution (report-only, C4-RESIDENT-READ-SCOPE-brief
  // §6): remember the verb token of the command currently being dispatched so
  // the PIET_C4_TRACE stderr line can name WHICH verb closed a dirty window.
  // No-op unless $PIET_C4_TRACE is set (the env-off path does no work at all);
  // read only by ensureMaterializedHead's trace emit. Definition in
  // session_state.cpp (needs the env gate helper).
  void noteC4TraceVerb(const std::string& rawCmd);

  // Resident shadow-card fast path for the 4 mutationResult copies. When a
  // non-structural frame delta from the just-completed mutation is pending,
  // folds it into the shadow card and returns a complete data_mutation card
  // (varlist_after / n_rows_after / new_vars / dropped_vars) WITHOUT touching
  // the datastore/mmap. Returns std::nullopt to fall through to the existing
  // real-datastore rebuild (structural change, no delta, or frames unused).
  std::optional<nlohmann::ordered_json> tryResidentMutationCard(
      const std::string& action, const std::vector<std::string>& newVars,
      const std::vector<std::string>& droppedVars,
      const std::vector<std::string>& rewrittenVars);

  // fix/verb-overhead-sweep S2. The safety check governing tryResidentMutation
  // Card's row-filter branch: does `delta.varsDelta` describe every SURVIVING
  // column of the post-mutation frame (count against delta.cols, exact name-set
  // equality with `card`, and an untouched column set), so that a card can be
  // rebuilt from it with nothing left to infer? False for everything else —
  // notably a PARTIAL delta, which the engine does not contract against (see
  // the definition's comment in session_state.cpp for the mechanism CoBrA Op
  // verified and why it stayed uncontracted). Public and static purely so a
  // gate leg can assert the guard's own truth table directly, without needing
  // an engine that produces each shape on demand.
  static bool residentCardDeltaComplete(const CobraFrameMeta& delta,
                                        const std::vector<VarInfo>& card);

  // Selftest instrumentation, mirroring Datastore::constructCount()'s
  // convention (datastore.h): the number of tryResidentMutationCard() calls
  // that produced their card by an incremental FOLD of the shadow card
  // instead of falling through to mutationResult's full rebuild. Counts the
  // MECHANISM, not the answer — a value-pinned card assertion stays green
  // even when the fold silently stops firing and every verb pays the rebuild
  // again (precisely how the L4 seed carry-forward stayed dead for a release
  // under a green n_missing pin). Process-wide and resettable so a gate leg
  // can bracket exactly one dispatch() call.
  static int64_t cardFoldCount() { return cardFoldCount_; }
  static void resetCardFoldCount() { cardFoldCount_ = 0; }

  // Selftest capability probe, same convention as cardFoldCount(): how many
  // times the §26 worker ACCEPTED a frame in this process — i.e. a frameUse
  // that came back ok with frames still enabled. Sticky and process-wide on
  // purpose. It answers the question residentFrameLive() cannot: that member
  // reads residentToken_, which is a TRANSIENT per-operation token cleared
  // during healthy operation (materializeVirtualSave clears it after the
  // frameSnapshot; runViaCobraSession clears it on a stateless mutation), so
  // at end-of-run it reports what the LAST command happened to do, not whether
  // this machine has a frames-capable `cobra serve` at all. A gate reading
  // cardFoldCount() == 0 needs the latter to tell an ENVIRONMENT failure from
  // a regressed fold.
  //
  // Incremented at exactly the three frameUse-gated sites (residentSyncWorker-
  // ToHead, loadFromVirtual, runViaCobraSession's resync). The three OTHER writers
  // of residentToken_ — the two residentRestore repoints and the post-frameRun
  // repoint to *outPath — are deliberately EXCLUDED: they merely re-point an
  // already-live plane and prove no capability, so counting them would inflate
  // this into a repointing tally and make a per-command netted reading
  // meaningless. Do not "helpfully" add them.
  static int64_t residentLiveCount() { return residentLiveCount_; }

  // ---- BUG-189: log-file visibility of a quiet scope -----------------------
  // The block filters that implement `quietly`/`noisily` run AFTER the block
  // array is built, but `log using` is teed from dispatch() (logTee) BEFORE
  // those filters ever see it — so a quiet body's echo and output reached the
  // log file no matter what the filter later decided. The scope has to be
  // visible at tee time, which is what this stack is.
  //
  // Semantics are the MEASURED ones (StataBE 19.5, /tmp/qprobe.do 2026-08-06),
  // not the intuitive ones — and the two differ:
  //   quietly { display "B" }            -> log shows `. quietly {` and NOTHING
  //                                         else: no body echo, no output, not
  //                                         even the closing brace.
  //   quietly { noisily display "C" }    -> log shows the OUTPUT `C`, but still
  //                                         NO echo of the noisily line.
  //   noisily { display "D" }            -> full transparency: echo AND output.
  // So `noisily` nested in `quietly` restores OUTPUT ONLY; command echo stays
  // suppressed for the whole enclosing quiet scope. Hence two questions, not
  // one: `logEchoSuppressed()` asks "is any quietly enclosing me" (a depth
  // count), while `logOutputSuppressed()` asks "what did the INNERMOST scope
  // decide" (the stack top) — which is also what makes
  // `noisily { quietly { … } }` suppress again.
  //
  // Scope-only: the returned block array is untouched, so the JSON transcript
  // (and every freeze fixture pinned on it) is byte-identical.
  bool logEchoSuppressed() const { return logQuietDepth_ > 0; }
  bool logOutputSuppressed() const {
    return !logVerbosity_.empty() && !logVerbosity_.back();
  }
  // A PREFIXED line (`quietly cmd` / `noisily cmd`) resolves its own visibility
  // inside dispatchImpl, but the log tee runs one frame out, in dispatch(),
  // after that scope has already closed — so the decision has to be handed
  // forward. Set by the innermost prefix layer (the BS-040 pass-through leaves
  // it alone, so `quietly noisily cmd` keeps the inner `noisily`, matching the
  // innermost-wins rule the block filters implement), consumed exactly once.
  enum class LineVis { None, Quiet, Noisy };
  void setLineVisibility(LineVis v) { lineVis_ = v; }
  LineVis consumeLineVisibility() {
    LineVis v = lineVis_;
    lineVis_ = LineVis::None;
    return v;
  }

  struct LogVerbosityScope {
    SessionState& s;
    bool quiet;
    LogVerbosityScope(SessionState& sess, bool isQuiet) : s(sess), quiet(isQuiet) {
      s.logVerbosity_.push_back(!isQuiet);
      if (isQuiet) ++s.logQuietDepth_;
    }
    ~LogVerbosityScope() {
      if (!s.logVerbosity_.empty()) s.logVerbosity_.pop_back();
      if (quiet && s.logQuietDepth_ > 0) --s.logQuietDepth_;
    }
    LogVerbosityScope(const LogVerbosityScope&) = delete;
    LogVerbosityScope& operator=(const LogVerbosityScope&) = delete;
  };
  // W0 (P2PERF §W0): the live worker, or nullptr when this session never needed
  // one. Read-only on purpose — unlike ensureCobraSession() it never spawns, so
  // reading the engine's counters can never fabricate the very mechanism being
  // counted (a spawn) as a side effect of measuring it.
  CobraSession* cobraSessionIfLive() { return cobraSession_.get(); }
  static void resetResidentLiveCount() { residentLiveCount_ = 0; }

  // Selftest precondition probe: is the §26 worker currently holding this
  // session's head as a resident frame? A fold gate reading cardFoldCount()
  // == 0 must be able to tell "this environment has no frames-capable
  // `cobra serve`, so there was never a delta to fold" (false here — an
  // ENVIRONMENT failure) from "the fold regressed" (true here).
  bool residentFrameLive() const { return !residentToken_.empty(); }
  // Selftest instrumentation (PP-L2B): TRUE iff the head is a dirty VIRTUAL
  // frame (a resident mutation not yet materialized). The residentLiveCount
  // delta stopped being a reliable "did the mutation go resident" probe once
  // the use-stash cache could adopt (PUT sync) or restore (hit) a frame at
  // `use` time — a live plane now services open+mutate with no new adoption.
  // This reads the state itself instead of inferring it from adoption counts.
  bool residentHeadDirty() const { return residentDirty_; }

  // Did the most recent runViaCobraSession() take the RESIDENT leg? A caller
  // that opted in with ResidentEligible needs this to tell "no --out file was
  // written because it was staged" (correct) from "no --out file was written
  // because the write failed" (BUG-350's silent short-write). Inferring it from
  // the file's absence cannot distinguish the two, which is why the state is
  // read rather than guessed. Set on every runViaCobraSession() exit, so it
  // describes that call and nothing earlier.
  bool lastRunWasResident() const { return lastRunResident_; }

  // GA sweep 2026-08-22: the merge counts the last resident run reported, if it
  // reported any. Returns nullptr when the last run was stateless (the counts came
  // on stderr) or was not a merge — so a caller can never mistake "not sent" for
  // "zero matched", which is the whole reason the engine sends a validity flag
  // rather than three bare zeroes.
  const CobraFrameMeta* lastResidentMergeCounts() const {
    return pendingDelta_.mergeCountsValid ? &pendingDelta_ : nullptr;
  }

  // MUTRES-WIRE (2026-08-22): the delta of the run that just finished, for the
  // ONE caller that needs it after a card fold has already declined —
  // mutationResult, which would otherwise open the datastore (and therefore
  // materialize the virtual head) purely to rebuild metadata the engine just
  // sent.
  //
  // Gated on CONTENT, not on pendingDeltaValid_, and that is deliberate:
  // tryResidentMutationCard consumes the flag on entry (it clears it before
  // deciding), so by the time a refusal reaches mutationResult the flag is
  // always false while the payload is intact. Staleness is impossible by
  // construction — pendingDelta_ is overwritten wholesale at every mutation
  // (runViaCobraSession's `r.ok && isMutation` branch), so it either describes
  // the run that just ran or is empty.
  const CobraFrameMeta* pendingWireDelta() const {
    return pendingDelta_.vars.empty() ? nullptr : &pendingDelta_;
  }

  // MUTRES-WIRE: ask the worker for the resident varlist without touching disk.
  // Returns nullptr when the engine predates the `vars` op or the session has no
  // frame — both of which mean "fall back to the datastore path", never an error.
  const CobraFrameMeta* fetchWireVarsFromWorker();

  // MUTRES-WIRE: the fold trace is how the census counts which rebuild path a
  // refusal took; mutation_result.cpp lives outside this class, so it needs a
  // public door to the same (env-gated, no-op when PIET_FOLDTRACE is unset) sink.
  static void foldTracePublic(const char* why, long long a, long long b) { foldTrace(why, a, b); }

  // MUTRES-WIRE: the wire→card seeder, promoted from private. Its callers were
  // all inside SessionState until mutation_result.cpp needed the SAME seeder for
  // the declined-fold rebuild — reusing it is the point (a second seeder would
  // be a second place for the format/label carry rules to drift).
  void seedShadowFromFrameVarsPublic(const CobraFrameMeta& meta) { seedShadowFromFrameVars(meta); }

  // Reseed the shadow card from a real datastore rebuild, keeping the resident
  // and datastore paths coherent (called by mutationResult's fallthrough).
  void reseedShadowFrom(Datastore* ds);

  // -- MAXPERF Stage 2 (§27 session snapshots) client wiring -----------------
  // preserve → `stash`. Ensures the worker holds the current head, then stashes
  // it (O(columns) RAM capture). A DIRTY VIRTUAL head is first materialized to
  // disk (ensureMaterializedHead) — NOT for the stash's correctness (it copies
  // worker RAM directly) but to keep a crash-recovery FLOOR: without an on-disk
  // head a post-preserve worker death would lose BOTH the resident and the
  // stash with nothing to roll back to. Returns the stash key, or std::nullopt
  // when the resident plane is unavailable (§26-only engine / serve disabled /
  // worker down) — the caller then falls back to the clonefile plane.
  std::optional<std::string> residentPreserve();
  // restore ← `unstash`. Re-establishes the named stash as the resident and
  // rebuilds the shadow card. `preserveHbinPath` is the on-disk head recorded at
  // preserve time (byte-identical to the stash by construction — the stash was
  // taken immediately after residentPreserve materialized that exact file): when
  // it still exists it is re-adopted as a CLEAN real head, so the card build
  // re-mmaps it with zero writes. Only if it is gone does restore fall back to a
  // dirty virtual head token (a later PIET-side datastore() materializes on
  // demand). Returns false if the stash could not be restored (worker restarted
  // / plane down) — the snapshot was RAM-only.
  //
  // `keepStash` (PP-L2B / K1b): default false consumes the stash (Stata plain
  // `restore` / the preserve-stack's LIFO pop). true retains it for reuse (the
  // K1b `use` cache re-`use`s the same source many times) — the engine's
  // unstash keep=true hands back a CoW copy of the frame (columns shared with
  // the retained stash until a later mutation), so a restored-then-mutated
  // dataset never corrupts the cached snapshot.
  bool residentRestore(const std::string& stashName, const std::string& preserveHbinPath,
                       bool keepStash = false);
  // tempfile `save` → `save_virtual`. Registers `path` in the worker's virtual-
  // file table (O(columns) RAM snapshot, no disk write). Returns false when the
  // resident plane is unavailable — the caller then writes a real byte-copy so a
  // stateless `use` still finds a file on disk.
  bool residentSaveVirtual(const std::string& path);
  // `use` of a virtual tempfile ← the engine's virtual-file table (frameUse hits
  // it virtual-first, O(columns)). Sets up a virtual head token so a later
  // datastore() materializes on demand. Returns false if `path` is not a live
  // virtual entry (never save_virtual'd here, or lost to a worker restart).
  bool loadFromVirtual(const std::string& path);
  // True iff `path` was registered by residentSaveVirtual this session (drives
  // runUse's virtual-first fast path). Cheap set membership.
  bool isVirtualSave(const std::string& path) const { return virtualSaves_.count(path) > 0; }
  // BUG-125: write a registered virtual save to its real path on demand, for
  // the DISK readers of a tempfile (merge/joinby/append `using`) that never
  // consult the worker's RAM table. Returns true when the file is now real
  // (or already was); false when the entry is unknown/lost — the caller's
  // existing file-not-found handling then reports honestly.
  bool materializeVirtualSave(const std::string& path);

  // C4-L2C (D3, C4-RESIDENT-READ-SCOPE-brief §3 close / §4 "save 이중쓰기 절반"):
  // the outcome of trySaveHeadDirect — how runSaveData should fill a real-file
  // save's destination sibling-temp.
  enum class SaveHeadPath {
    // The current head is already a real file at *currentHbin_ (a clean head,
    // or a divergent intermediate temp is in flight so currentHbin_ is real):
    // the caller copies that file into the temp (clonefile/buffered), exactly
    // the pre-C4 path. Also the outcome when frames are unavailable.
    CopyFromCurrent,
    // The head was a dirty VIRTUAL frame still corresponding to currentHbin_ and
    // has been serialized STRAIGHT into the caller's destination temp — the
    // materialize-scratch-then-copy double write is now a single write.
    WroteDirect,
  };

  // C4-L2C (D3): for a dirty virtual head, write it DIRECTLY into `destTmp` (a
  // sibling temp of the user's `save` target) with one frameSnapshot, instead of
  // first materializing the scratch head (ensureMaterializedHead → currentHbin_)
  // and then copying that file into the temp — the save's own double write. The
  // guard mirrors ensureMaterializedHead's exactly: only a dirty head whose
  // resident frame still corresponds to currentHbin_ takes the direct path (a
  // clean or divergent head returns CopyFromCurrent, because currentHbin_ is
  // then a real file the caller copies verbatim).
  //
  // The head is deliberately left VIRTUAL — residentDirty_ is NOT cleared. `save`
  // does only its own work (serialize a copy to the user's file) and no longer
  // has the SIDE EFFECT of anchoring the head on disk for a hypothetical next
  // consumer; a genuine later disk consumer materializes lazily on demand (the
  // resident-plane philosophy — and post-D1/D2 many former consumers are
  // card/value-plane reads that never materialize at all). Consequently this
  // deliverable write is NOT counted as a snapshot cycle (floor metric ① counts
  // SCRATCH-materialization waste; the user's save target is not waste — the old
  // save-entry EMH counted it only as an accident of routing through scratch).
  //
  // Byte-parity with the escape/old path is by construction: both serialize the
  // SAME resident frame via frameSnapshot, and a clonefile copy is byte-exact —
  // so the file that lands at `destTmp` is identical whichever path runs (the
  // A/B save-parity arm, C4-L2C gate). On a snapshot FAILURE (ENOSPC, worker
  // death) the possibly-torn temp is removed and CopyFromCurrent is returned so
  // the caller's ensureMaterializedHead+copy path retries and reports honestly.
  SaveHeadPath trySaveHeadDirect(const std::string& destTmp);

  // Opt a MUTATION (--out) verb in scope INTO the resident frame plane. The
  // routing default is stateless serve (byte-equal one-shot, real --out files)
  // — universally safe because a real scratch file is exactly what today's
  // chain-head handlers expect (they read intermediate --out temps back AS
  // FILES via local Datastore/ifstream). Only verbs whose --out becomes the
  // session head and is NEVER read back as a file (it is load()ed and rendered
  // from the shadow card) wrap their mutation call in this guard to advance the
  // resident frame in place: the hot pure-add/replace verbs (gen/replace/egen),
  // the column-set verbs (drop/keep), and — S2 — the if-only ROW FILTER, whose
  // engine call carries --keepif and which is therefore also the one opt-in
  // that changes how runViaCobraSession routes (see its goResident comment).
  // Read-only calls always use the frame plane regardless (no --out => no temp
  // file => nothing to break), so estimation/summarize/list read worker RAM
  // without a per-verb file rewrite. The invariant a caller asserts by entering
  // this guard is unchanged and load-bearing: "this --out becomes the head and
  // no one reads it as a file". A shared helper that cannot honour it for a
  // sub-scope says so with ResidentSuspend below.
  void enterResidentEligible() { ++residentEligible_; }
  void exitResidentEligible() {
    if (residentEligible_ > 0) --residentEligible_;
  }
  struct ResidentEligible {
    explicit ResidentEligible(SessionState& s) : s_(s) { s_.enterResidentEligible(); }
    ~ResidentEligible() { s_.exitResidentEligible(); }
    ResidentEligible(const ResidentEligible&) = delete;
    ResidentEligible& operator=(const ResidentEligible&) = delete;
    SessionState& s_;
  };
  // fix/verb-overhead-sweep S2: the inverse — force STATELESS routing for a
  // scope nested inside an opted-in caller. Needed because eligibility is a
  // session counter, not a call-stack property: a caller that opts its own
  // single terminal mutation in also, transitively, opts in every cobra call a
  // shared helper makes on its behalf. A helper whose intermediate --out temps
  // are read back AS FILES (applyIfAndInFilter's `in`-range chain) must be able
  // to say so locally rather than depend on no caller ever opting in. Saves and
  // restores the full count, so it composes with nesting in either direction.
  int suspendResidentEligible() {
    int prev = residentEligible_;
    residentEligible_ = 0;
    return prev;
  }
  void restoreResidentEligible(int prev) { residentEligible_ = prev; }
  struct ResidentSuspend {
    explicit ResidentSuspend(SessionState& s) : s_(s), saved_(s.suspendResidentEligible()) {}
    ~ResidentSuspend() { s_.restoreResidentEligible(saved_); }
    ResidentSuspend(const ResidentSuspend&) = delete;
    ResidentSuspend& operator=(const ResidentSuspend&) = delete;
    SessionState& s_;
    int saved_;
  };
  // P2-PERF ARC-C C1-④: the OTHER inverse — force stateless routing for
  // READ-ONLY calls in a scope. ResidentSuspend cannot express this: read-only
  // residency is not gated on the eligibility counter at all (a read-only call
  // with no --out writes no file, so it is normally always safe and always
  // cheaper on the resident plane).
  //
  // It stops being cheaper when the --data is not the session's own head. The
  // by-group dispatch loop runs its sub-command once per group against a
  // per-group SLICE file: routing those reads to the resident plane frame-uses
  // each slice in turn, which evicts the session's real head, snapshots the
  // outgoing virtual head to keep it alive, and then resyncs on the next
  // iteration — G evictions plus G resyncs to read G small files, and one more
  // resync for whatever runs after the loop. Stateless leaves the resident
  // frame holding the real head untouched for the whole loop, and the slices
  // are by construction 1/G of the data.
  //
  // Scope-shaped rather than a parameter for the same reason ResidentSuspend
  // is: the sub-command reaches the router through the ordinary handler stack
  // (runRegress -> ... -> runViaCobraSession), which no caller should have to
  // thread a routing flag through.
  int suspendResidentRead() {
    int prev = statelessRead_;
    statelessRead_ = prev + 1;
    return prev;
  }
  void restoreResidentRead(int prev) { statelessRead_ = prev; }
  struct StatelessRead {
    explicit StatelessRead(SessionState& s) : s_(s), saved_(s.suspendResidentRead()) {}
    ~StatelessRead() { s_.restoreResidentRead(saved_); }
    StatelessRead(const StatelessRead&) = delete;
    StatelessRead& operator=(const StatelessRead&) = delete;
    SessionState& s_;
    int saved_;
  };
  bool statelessReadScope() const { return statelessRead_ > 0; }

  // -- macro-layer subset (locals/globals/scalars/r()/e()/matrices/cwd) ----
  // Embedded, not duplicated: macros.cpp / exprfns.cpp keep operating on
  // `eval::EvalSession` unmodified. `eval.cwd` is kept in sync with `cwd`
  // by the handlers that mutate cwd (see use handler) so macro-layer r()/
  // scalar expansion inside a do-file sees the same session cwd.
  eval::EvalSession eval;

  // -- run progress / liveness (B1, 1.16.0; M3a: real do-executor publisher)
  // Thread-safe: guarded by progressMutex_ so a concurrent reader (future
  // GET /api/run_progress at M4) never observes a torn snapshot while
  // doexec.cpp's executor updates it from a worker thread. See ROADMAP's
  // M0 shell-review carry-forward — thread-lifetime discipline binds here.
  void progressStart(int totalLines, std::optional<std::string> doPath = std::nullopt);
  void progressUpdate(int currentLine, const std::string& currentCmd, int blocksReady);
  void progressFinish(const std::string& reason, std::optional<int> blocksReady = std::nullopt);
  bool progressRequestCancel();       // true iff a run was in flight
  bool progressCancelRequested() const;
  nlohmann::ordered_json progressSnapshot() const;

  // -- _request() interactive dialog request/response (BUG-087, D9 BETA
  // Phase F: contract_B additive MINOR — one new route + one new push-event
  // shape). Native side of the `display ... _request(macroName)` extended
  // macro function (display_hardening.cpp is the real call site — see that
  // file for why): blocks the do-executor WORKER thread, pushes a
  // `{"kind":"request_dialog","requestId":...,"prompt":...,"macroName":...}`
  // event over the SAME pietEvents channel RunProgress ticks already use,
  // and resumes once the frontend posts an answer through the new
  // "request.answer" bridge route (main.cpp wires that to resolveUserInput()
  // below) or the run is cancelled (reuses progressRequestCancel()'s
  // EXISTING cancelRequested flag/notify — see that method's doc comment).

  // Registers the one callback that knows how to push a JSON string onto
  // the real pietEvents channel (Shell::postEvent, bound by main.cpp
  // immediately after constructing the Shell in the windowed / --ui-smoke
  // entry points). Left unset (nullptr) in headless contexts — runDoFile
  // Headless() never constructs a Shell at all — so hasRequestPostEvent()
  // IS the "is there a real Shell/webview" signal display_hardening.cpp
  // consults to choose the blocking-modal path vs the headless
  // PIET_REQUEST_<NAME> env-var fallback.
  using PostEventFn = std::function<void(const std::string&)>;
  void setRequestPostEvent(PostEventFn fn) { requestPostEvent_ = std::move(fn); }
  bool hasRequestPostEvent() const { return static_cast<bool>(requestPostEvent_); }

  // Blocks the CALLING thread until the frontend posts an answer for a
  // freshly-minted requestId, or the run is cancelled. Callers MUST check
  // isCurrentThreadADoWorker() first (see that function's doc comment) —
  // this method does not re-check itself, it only requires
  // hasRequestPostEvent() to be true (defensive: returns nullopt
  // immediately if not, though real callers already gate on that too).
  // Returns the answer string, or nullopt if the run was cancelled before
  // an answer arrived (caller must surface an honest error — never hang,
  // never silently continue, per BUG-087's own spec).
  // SPEC II-4 (2026-09-02, contract_B additive): `choices` non-empty turns the
  // push into a fixed-choice question — the event carries
  // `choices:[{value,label}]` and any `extra` fields (the pre-flight adds
  // `suggest_budget_mb`); the answer wire is unchanged (the frontend answers
  // with the chosen `value`). Empty = the free-text dialog `_request()` uses.
  std::optional<std::string> requestUserInput(
      const std::string& macroName, const std::string& prompt,
      const std::vector<std::pair<std::string, std::string>>& choices = {},
      const nlohmann::ordered_json& extra = nlohmann::ordered_json::object());

  // Called by the request.answer bridge route handler (runs on the UI
  // thread; see main.cpp's bindAllRoutes) to deliver a submitted answer and
  // wake the matching requestUserInput() wait. A stale/unknown requestId
  // (e.g. the modal was answered after its run was already cancelled and
  // the pending slot reaped) is a silent no-op — the wire contract's
  // response is always {"ok":true} regardless (WIRE SHAPE, locked).
  void resolveUserInput(const std::string& requestId, const std::string& value);

  // -- per-run tempdir (tempfile/tempname, S5/U4) — cleaned up by execute_do's
  // caller (mirrors session.py's `_run_tempdir` + doexec.execute_do's finally).
  std::optional<std::string> runTempdir;

  // BUG-106: recursion-depth counter for doexec::executeDo. executeDo is the
  // TOP-LEVEL entry point (CLI --do, GUI Run) but is ALSO invoked recursively
  // for every nested `do FILE`/`run FILE` command a script dispatches
  // (dispatcher.cpp's `do` arm calls doexec::executeDo again). runTempdir
  // above is session-wide — it backs every `tempfile` local for the entire
  // top-level run, not just whichever do-file happens to be executing — so
  // its cleanup must fire only once, when the OUTERMOST executeDo call
  // returns. This counter (mirrors progCallDepth just above) lets executeDo
  // tell the two cases apart. See doexec.cpp's DoExecDepthGuard.
  int doExecDepth = 0;

  // BUG-922: nested do-files that own a preserve level -- `do`/`run` FILE
  // dispatched from a running script (dispatcher.cpp's do arm), not `include`
  // (it runs as if typed in its caller) and not the top-level run. Stata keeps
  // one preserved copy per program OR do-file level and restores it when that
  // program or do-file concludes ([P] preserve: "When the program or do-file
  // concludes, the user's data are automatically restored").
  int doFileDepth = 0;

  // BUG-1015/BUG-1459: the version the caller runs under (`version #`), 0 when
  // none was set. A bare `version #` statement sets it until the do-file or
  // program that set it concludes ([P] version: "the version set in a program
  // or do-file is local to it"; StataBE: `version 12` inside a program reads
  // 12 there and the caller's 16 after it); the `version #:` prefix sets it
  // for its one command. table's pre-17 contents() reads it, and c(version).
  double callerVersion = 0.0;

  // BUG-111: monotonic per-session counter so each `tempfile NAME` / `tempname NAME`
  // CALL mints a UNIQUE path/basename (Stata's `__000001`, `__000002`, … scheme).
  // The old scheme keyed the file purely on NAME (`<dir>/NAME.tmp`) / on epoch
  // SECONDS (`__NAME_<time>`), so two calls with the same name (a loop re-declaring
  // `tempfile t`, or a nested program's own `tempfile t`) silently shared one file /
  // one scalar name → cross-scope clobber. The macro NAME still binds to the minted
  // value, so scripts using `\`NAME'` are unaffected.
  long tempSeq = 0;

  // BUG-360 follow-up (contract_A section 38 projections). `fetchColumns` writes a
  // getcols projection into the session workdir, hands back a Datastore over it,
  // and — on the SUCCESS path — nothing ever removes the file. It is deleted only
  // when the fetch FAILS ("never leave a torn file"). The unique_ptr frees the
  // mmap; the bytes stay until the process exits.
  //
  // `reclaimSupersededScratch` structurally cannot help: it registers a head that
  // a NEW head superseded, and a projection is not a head — it is a side file
  // read once and abandoned, superseded by nothing. Exactly the reason it could
  // not see a tempfile either.
  //
  // Measured 2026-08-19, 5 serial controls=yes tasks: **692 projection files**,
  // 460 MB, still climbing at the end of the run — the reason the workdir's FILE
  // COUNT rises while its byte total hovers at the 4 GiB budget.
  //
  // A projection's life is one dispatched command (its consumers read it and
  // build their own output from it), so the ledger is drained at the OUTERMOST
  // dispatch boundary rather than per call site — one of the eight sites returns
  // the path to its caller, so per-site RAII would be wrong there.
  void noteProjectionScratch(const std::string& path) {
    if (!path.empty()) projectionScratch_.push_back(path);
  }
  // Best-effort, never throws. Refuses the current head as a matter of principle:
  // a projection is never adopted as one, so if that ever changes this says no
  // rather than deleting the session's data. Defined in session_state.cpp — the
  // path conversion lives behind io/win_compat.h and this header is included
  // almost everywhere, so the include stays out of it.
  void reapProjectionScratch();

  // BUG-381 DIAGNOSTIC (2026-08-31): attribute every byte in the LIVE workdir to
  // the ledger that owns it, or to ORPHAN. Env-gated (PIET_SCRATCH_CENSUS) —
  // it is a full recursive walk with a stat and a canonicalization per file.
  //
  // Why it exists: ADDENDUM 6 of internal note GA-SWEEP-CENSUS-20260822.md measured a
  // scratch floor that grows LINEARLY in wall time (3.37 GiB/hour, R^2 0.9981)
  // and that PIET_SCRATCH_BUDGET_MB cannot touch — so by construction it is not
  // superseded scratch. The two classes THIS header names above (projection
  // scratch, minted tempfiles) were measured 2026-08-19, which is BEFORE their
  // reaps were wired (dispatcher.cpp at exitDispatch()==0; program.cpp on
  // program return). Those notes are therefore a hypothesis about that build,
  // not this one, and attributing today's floor to them would be exactly the
  // stale-report mistake. This measures instead of arguing.
  //
  // ORPHAN is the finding class: a file no ledger in this session knows about
  // is one that no reap can ever reach, however the reaps are scheduled.
  std::string scratchCensusJson() const;

  // BUG-381 DIAGNOSTIC. One stderr line per traced event when PIET_SCRATCH_TRACE
  // is set; a no-op (one getenv, memoized) otherwise.
  static void scratchTrace(const char* what, const std::string& a, const std::string& b);
  // Memoized once per process; inline so an off build is a load and a branch.
  static bool scratchTraceOn() {
    static const bool on = [] {
      const char* e = std::getenv("PIET_SCRATCH_TRACE");
      return e != nullptr && *e != '\0';
    }();
    return on;
  }

  // PHASE-0 DIAGNOSTIC (2026-08-20): env-gated (PIET_FOLDTRACE) card-fold
  // rejection trace. Report-only; removed once the fold gap is closed.
  static void foldTrace(const char* why, long long a, long long b);

  // BUG-360: every path `tempfile` mints, tagged with the progCallDepth that
  // minted it, so a returning program can erase its own.
  //
  // The tempdir reap in doexec.cpp fires once, at `doExecDepth == 0` — i.e. at
  // the end of the whole top-level run. That is CORRECT for a do-file-level
  // `tempfile` and WRONG for a program-level one, and Stata says so plainly:
  // "When the program or do-file concludes, any datasets created with these
  // assigned names are erased" ([P] macro, and again under tempfile: it happens
  // "regardless of whether your program ends with an error").
  //
  // Measured 2026-08-19 on the acceptance corpus's controls=yes arm, which
  // calls one plotting program 47 times: the session workdir's `.tmp` files
  // grew monotonically, 13 -> 80 files over five tasks, never once reclaimed,
  // while the `.hbin` bytes beside them plateaued at the 4 GiB scratch budget.
  // The budget cannot bound these — it only ever sees a head that a NEW head
  // superseded, and a tempfile is superseded by nothing.
  std::vector<std::pair<int, std::string>> mintedTempfiles;

  // BUG-922: the names `tempvar` and `tempname` mint, tagged with the
  // preserveLevel() that minted them, the same way as mintedTempfiles. Stata
  // drops a program's or nested do/run file's temporary variables, scalars and
  // matrices when it concludes (oracle w2_prog_controlflow pc_tv_gone and
  // w2ad_prog_preserve_nest pn_tv_do_gone: `confirm variable/scalar` on the
  // names afterwards are r(111)); program::concludeLevel drops the entries
  // deeper than the caller's level.
  std::vector<std::pair<int, std::string>> mintedTempvars;
  std::vector<std::pair<int, std::string>> mintedTempnames;

  // -- preserve/restore LIFO snapshot stack (preserve_restore.py, M3a) -----
  std::vector<PreserveFrame> preserveStack;
  // BUG-922: `restore, not` -- forget the top preserved copy without putting
  // it back (its worker stash is released). The caller checks that the copy
  // is its own (ownPreserveFrame).
  void discardTopPreserve();
  // BUG-922: the one reach predicate. The top frame iff the level now running
  // preserved it, else nullptr. `preserve` (r(621)), `restore`, `restore, not`
  // and `restore, preserve` (r(622)) all ask this, and nothing else.
  const PreserveFrame* ownPreserveFrame() const;

  // -- user-defined programs (program.py, M3a S6/S7) -----------------------
  // session._programs: canonical-case name -> ProgramDef. std::map (not
  // unordered_map) purely for deterministic `program dir` listing ordering
  // via sorted-key iteration at the call site — Python's `sorted()` call in
  // handle_program_manage does the same regardless of underlying dict type,
  // so map vs unordered_map is not itself behavior-relevant, but map keeps
  // debugging/iteration order stable.
  std::map<std::string, ProgramDef> programs;
  // The ado file whose program is running (ProgramDef::ado), empty outside
  // one. An ado file's other programs are private to it ([U] 17.11): they are
  // stored under `<ado>.<name>` and only a program of that same file finds
  // them by their short name.
  std::string adoContext;

  // BUG-342: capture-time copies of every `program define` block seen in the
  // do-file currently being preprocessed, keyed the same way. `programs` is the
  // LIVE table and is cleared by `clear all` / `program drop _all`; this one is
  // not, because it is not a program table — it is the source text's own record
  // of what each definition site installs when execution reaches it. See
  // program.h's kProgInstallSentinelPrefix note for why the definition point,
  // not the preprocess pass, must be what defines a program.
  std::map<std::string, ProgramDef> programArchive;

  // P9 automode: bumped whenever the program TABLE changes shape (a definition
  // installed, a drop, a clear). The effect-summary cache keys on it, so a
  // redefined program can never be served a stale summary — the alternative
  // (invalidate by name) misses `clear all`, which changes every entry at once.
  unsigned long long programsEpoch = 0;

  // -- timer stopwatch bank (BS-124, BETA campaign C4) ----------------------
  // Slots 1..100 (index 0 unused, matching Stata's own 1-based numbering);
  // see TimerSlot's doc comment above and handlers_m3.cpp::runTimer for the
  // on/off/clear/list state machine this backs.
  std::array<TimerSlot, 101> timers;

  // session._scope_stack: push_scope()/pop_scope() save/restore frames here
  // by swapping `eval.locals` in place (mirrors Python's `session._locals`
  // swap). Each program call pushes the caller's locals dict and installs a
  // fresh OrderedDict seeded from the callee's positional args.
  std::vector<eval::OrderedDict> scopeStack;

  // session._return_staging: name -> (kind, value-as-string) where kind is
  // "scalar" or "local". Python stores scalar values as float and local
  // values as str in the SAME dict (a `tuple[str, float|str]`); here both
  // are normalised to string and the scalar side is parsed back to double
  // at the flush site (run_program) with std::stod — the staged string is
  // always produced by std::to_string/formatting of a float upstream (see
  // program.cpp's handleReturnStmt), so the round-trip is exact for the
  // values this module ever stages.
  std::map<std::string, std::pair<std::string, std::string>> returnStaging;

  // session._prog_call_depth: recursion guard counter for run_program.
  int progCallDepth = 0;

  // BUG-922: the preserve level of the code now running: one step per program
  // call and per nested do/run file. A PreserveFrame records it; a second
  // `preserve` at the same level is r(621); the program or do-file that ends
  // restores every frame deeper than its caller's level.
  int preserveLevel() const { return progCallDepth + doFileDepth; }

  // BUG-920: loops::loopBodyDepth() when the program or nested do/run file now
  // running started. A `continue` is in context only inside a loop body of its
  // own level: a program or do-file run from a loop body starts outside any
  // loop (oracle w2ad_prog_preserve_nest pn_lm_cb_prog, pn_lm_cb_do: r(119)
  // at every iteration). program::LoopFloorGuard sets and restores it.
  int loopBodyFloor = 0;

  // session._postfiles: handle name -> open PostHandle (S8/M3b, postfile.py).
  std::map<std::string, PostHandle> postfiles;

  // BUG-343: `file open|write|read|close` — an open text-mode file handle. Same
  // shape as `postfiles` above and for the same reason: Stata's file I/O is a
  // NAMED-handle protocol whose state has to outlive the statement that opened
  // it. Text mode only; the dispatcher rejects `binary` by name rather than
  // pretending, so a do-file that needs it stops instead of writing nonsense.
  struct FileHandle {
    std::string path;      // resolved absolute path
    bool write = false;    // opened for write/append (vs read)
    bool append = false;
    std::FILE* fp = nullptr;
    bool eof = false;      // last `file read` hit end-of-file -> r(eof)
    // BUG-1526: `binary` mode, `read write` (both verbs on one handle) and the
    // byte order of its multibyte elements (hilo = most significant first).
    bool binary = false;
    bool readWrite = false;
    bool hilo = false;
  };
  std::map<std::string, FileHandle> fileHandles;

  // RED-fix (piet_2.1.0_gate VERDICT.md Leg 3, test_postfile_using_replace_
  // roundtrips#5): postclose's `cobra convert` writes this session's OWN
  // hbin bytes to whatever target name `postfile ... using` was given
  // (commonly a `tempfile`-generated path with a `.tmp`/no-hbin extension —
  // that is completely routine, universal Stata usage, not a mislabeled or
  // stale file). handlers.cpp's runUse BUG-A hbin-sniff (commit ad23151)
  // correctly rescues a genuinely mislabeled/stale-save-era file with an
  // explanatory note, but it fired on this routine postfile round-trip too,
  // inserting an unrequested extra block. Track the resolved absolute path
  // of every file THIS session's postclose finalizes here; runUse checks
  // membership to suppress the note ONLY (the sniff/format-redirect stays:
  // content still decides how to load) when `use` is reading back a file
  // the session itself just wrote via postfile — there is nothing to
  // explain to the user about a file they didn't misname. A genuine
  // external/misnamed .hbin-content file (not in this set) still gets the
  // note, unchanged.
  std::set<std::string> postfileWrittenPaths;

  void recordPostfileWrite(const std::string& resolvedPath) {
    postfileWrittenPaths.insert(resolvedPath);
  }

  bool isPostfileWrittenPath(const std::string& resolvedPath) const {
    return postfileWrittenPaths.count(resolvedPath) != 0;
  }

  // Resolved absolute paths THIS session wrote with `save`. Read only by the
  // `datastore_reloads` counter in runUse, which needs to tell a re-read of
  // the session's OWN output (work done twice by construction) apart from an
  // ordinary first load of a foreign file (irreducible). Never gates behaviour
  // — the memo's stat key does that; this set only classifies what the counter
  // is allowed to count, so a stale entry can misreport a number and can never
  // serve wrong data.
  std::set<std::string> savedPaths;
  void recordSaveWrite(const std::string& resolvedPath) { savedPaths.insert(resolvedPath); }
  bool isSavedPath(const std::string& resolvedPath) const {
    return savedPaths.count(resolvedPath) != 0;
  }

  // -- eststo/esttab named estimates store (M4a, session.py's _estimates /
  // eststo()/estimates_names()/estimates_get()/estimates_drop()/
  // estimates_clear()) — insertion-ordered (Python dict order is observable
  // via `esttab`/`eststo dir`'s listing order), so a vector-of-pairs +
  // linear lookup mirrors dict semantics exactly (small N in practice).
  std::vector<std::pair<std::string, nlohmann::ordered_json>> estimates;
  int eststoSeq = 0;

  // eststo(name, snapshot[, autoPrefix, viaEststo]): overwrites in place if the
  // name already exists (preserving its original position, matching Python
  // dict[key]=value semantics); auto-names when name is empty.
  //
  // W2 eststoprefix (BUG-521). The auto-name was "m#", counting bare stores
  // only. estout's is `est#`, and # counts EVERY set eststo has stored, named
  // or not -- tests/eval/golden/aer_w2/w2_eststo_prefix.log:
  //   A2   `eststo clear`, `eststo lit1: ...`, then bare  -> "(est2 stored)";
  //        `estimates table est1` AND `... m1` are both r(111)
  //   A7   eight sets stored, `eststo, prefix(zz): ...`   -> "(zz9 stored)" --
  //        prefix() swaps the stem and SHARES the counter (zz1/zz2 are r(111))
  //   A8   two failing prefix lines, then bare            -> "(est10 stored)":
  //        a failure consumes no number (it never reaches this function)
  //   A11  `eststo clear`, then bare                      -> "(est1 stored)"
  // Corpus do-files read these names back (`esttab est1 est3`), so "m#" was a
  // table built from the wrong models, or r(111), two commands later.
  //
  // Two rules the oracle does not reach come from the package itself, which is
  // the contract for a community command (eststo.ado 1.1.0, _eststo_store and
  // _eststo_cleanglobal): re-storing under a name already in the list is a
  // REFRESH and does not advance the counter, and an EMPTY list restarts it.
  // `viaEststo=false` is `estimates store`: official Stata, which never writes
  // $eststo_counter, so it must not move the next `est#` either.
  std::string eststo(const std::optional<std::string>& name, nlohmann::ordered_json snapshot,
                     const std::string& autoPrefix = "est", bool viaEststo = true) {
    const bool named = name.has_value() && !name->empty();
    if (estimates.empty()) eststoSeq = 0;
    if (named) {
      for (auto& [k, v] : estimates) {
        if (k == *name) {
          v = std::move(snapshot);
          return k;
        }
      }
    }
    // An unnamed `estimates store` is not Stata at all; it keeps minting a name
    // here, as it always has, so it has to take a number to mint it from.
    if (viaEststo || !named) ++eststoSeq;
    const std::string key = named ? *name : autoPrefix + std::to_string(eststoSeq);
    for (auto& [k, v] : estimates) {
      if (k == key) {
        v = std::move(snapshot);
        return key;
      }
    }
    estimates.emplace_back(key, std::move(snapshot));
    return key;
  }

  std::vector<std::string> estimatesNames() const {
    std::vector<std::string> out;
    out.reserve(estimates.size());
    for (const auto& [k, v] : estimates) out.push_back(k);
    return out;
  }

  const nlohmann::ordered_json* estimatesGet(const std::string& name) const {
    for (const auto& [k, v] : estimates) {
      if (k == name) return &v;
    }
    return nullptr;
  }

  // AER-SCOPE1 bundle E: `estadd ... : m1 m2` attaches to ALREADY-STORED
  // estimates rather than the active one, so it needs a mutable handle. The
  // const overload above stays the default; this is the only writer.
  nlohmann::ordered_json* estimatesGetMutable(const std::string& name) {
    for (auto& [k, v] : estimates) {
      if (k == name) return &v;
    }
    return nullptr;
  }

  // -- AER-SCOPE1 bundle E: estadd's bag on the ACTIVE estimate ---------------
  // Stata's `estadd` runs BEFORE `eststo` and attaches to whatever e() is
  // active; the corpus writes it that way 5,725 times. So the additions cannot
  // live in the `estimates` store (nothing is stored yet) -- they sit here and
  // `runEststo` copies them into the snapshot.
  //
  // `estAddedFp` is a fingerprint of the estimate the bag belongs to
  // (depvar|estimator|n_obs|coeftable size). It exists because `lastEstBlock`
  // is assigned directly by five different TUs, so there is no single place to
  // clear the bag from; instead the bag is checked for staleness at READ time
  // and discarded when a different estimation has replaced e() underneath it.
  // Known limit, deliberately documented rather than papered over: re-running
  // the IDENTICAL specification produces the same fingerprint, so an earlier
  // estadd survives into it. That is the same model, so the additions are still
  // the right ones.
  nlohmann::ordered_json estAdded = nlohmann::ordered_json{
      {"scalars", nlohmann::ordered_json::object()},
      {"locals", nlohmann::ordered_json::object()},
      {"matrices", nlohmann::ordered_json::object()}};
  std::string estAddedFp;

  // -- AER-SCOPE1 bundle G: putexcel's open workbook ---------------------------
  // `putexcel` is a STATEFUL command: `set` names the file, each `A1 = ...`
  // updates one cell, and the workbook is rewritten from the accumulated map.
  // The map lives here rather than in the handler because it must survive
  // between statements, which is exactly what session state is for.
  //
  // Shape: {path, sheet, mode, noteShown, cells:{"A1":{"n":1.5}|{"s":"t"}},
  // merges:["A1:B2"]}. One json field rather than five typed ones because the
  // whole thing is written and read in one place, and a struct here would have
  // to be declared in a header every TU already includes.
  nlohmann::ordered_json putexcel = nlohmann::ordered_json::object();

  // The fingerprint itself. It lives HERE, as one static function on the state
  // it describes, because TWO translation units must agree on it exactly:
  // handlers_aer_tables.cpp WRITES it when `estadd` fills the bag, and
  // eststo_esttab.cpp READS it when `eststo` decides whether the bag still
  // belongs to the active estimate. Two private copies that drift by one field
  // would make the bag silently never match, and the visible symptom would be
  // an `estadd`ed row quietly missing from the table -- rc 0, plausible output.
  static std::string estAddFingerprint(const nlohmann::ordered_json& estBlock) {
    std::string dep = estBlock.value("depvar", std::string());
    std::string est = estBlock.value("estimator", std::string());
    std::string n = "-";
    if (estBlock.contains("n_obs") && estBlock["n_obs"].is_number()) {
      n = std::to_string(estBlock["n_obs"].get<long long>());
    }
    std::size_t k = 0;
    if (estBlock.contains("coeftable") && estBlock["coeftable"].is_array()) {
      k = estBlock["coeftable"].size();
    }
    return dep + "|" + est + "|" + n + "|" + std::to_string(k);
  }

  void dropEstAddedMirrors() {
    for (const char* kind : {"scalars", "locals"}) {
      if (!estAdded.contains(kind) || !estAdded[kind].is_object()) continue;
      for (const auto& kv : estAdded[kind].items()) {
        eval.eScalars.erase(kv.key());
        eval.eStrings.erase(kv.key());
      }
    }
    if (estAdded.contains("matrices") && estAdded["matrices"].is_object()) {
      for (const auto& kv : estAdded["matrices"].items()) {
        const std::string n = kv.key();
        eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                            [&](const auto& p) { return p.first == n; }),
                             eval.eMatrices.end());
      }
    }
  }
  void estAddedClear() {
    estAdded = nlohmann::ordered_json{{"scalars", nlohmann::ordered_json::object()},
                                      {"locals", nlohmann::ordered_json::object()},
                                      {"matrices", nlohmann::ordered_json::object()}};
    estAddedFp.clear();
  }

  bool estimatesDrop(const std::string& name) {
    for (size_t i = 0; i < estimates.size(); ++i) {
      if (estimates[i].first == name) {
        estimates.erase(estimates.begin() + static_cast<long>(i));
        estimatesModelErase(name);
        // estimates.ado Drop: dropping the name the active e() carries clears
        // e(_estimates_name) (ClearFields); the results themselves stay active.
        if (activeEstimatesName == name) activeEstimatesName.clear();
        return true;
      }
    }
    return false;
  }

  void estimatesClear() {
    estimates.clear();
    for (const auto& kv : estimatesModels) {
      if (kv.second) pinStoredModel(*kv.second, -1);
    }
    estimatesModels.clear();
    activeEstimatesName.clear();
    eststoSeq = 0;
  }

  // -- W2 esttabnames (BUG-553): the stored MODEL behind each stored table ----
  // `estimates store`/`eststo` kept only the table snapshot above, so
  // `estimates restore NAME` had nothing to make active. This is what
  // `estimates use` restores from a file (runEstimatesUse) -- the LastModel
  // that predict/test/lincom/e()/_b[] read and the e() display block -- plus
  // the macro layer's e() as it stood at store time: eval.lastModelResult
  // (estadd merges its scalars there, not into LastModel::result) and the
  // e-scalar/e-macro/e-matrix stores (estadd, pca, reghdfe's e(resid) ...).
  // Held by shared_ptr so a restore shares the one copy. No FRAME is copied:
  // LastModel names its fit data by path, and that path is PINNED against the
  // scratch GC for as long as the model is stored (pinStoredModel below).
  struct StoredModel {
    std::optional<LastModel> lastModel;
    std::optional<nlohmann::ordered_json> estBlock;
    std::optional<nlohmann::ordered_json> lastModelResult;
    eval::OrderedFloatDict eScalars;
    eval::OrderedDict eStrings;
    std::vector<std::pair<std::string, eval::EvalMatrix>> eMatrices;
    // W2D-T01 round 3: SessionState::eOnFitNames as it stood at the store, so
    // the next fit after `estimates restore` drops them again.
    std::vector<std::string> eOnFitNames;
    // e(sample) T3: the store wrote the variable `_est_<name>` (byte, 1 = in
    // e(sample)) on the data, as estimates.ado Store does (`_est hold ...,
    // varname(_est_<name>)`; w2_esample.log:1361 G0b `0|28|`). `estimates
    // restore` reads e(sample) back from that variable, and `estimates drop`/
    // `clear` drop it. False when the store could not write it (no data, or a
    // model whose e(sample) PIET does not have): the restore then stays a named
    // r(198) and a same-named variable the user made is never read as a mask.
    bool esampleVar = false;
    // A stored POSTED e() (no lastModel): its `ereturn post ..., esample()`
    // mask serial (SessionState::ePostedEsampleSerial), restored with it. The
    // reading still checks the cells, so a column another install has since
    // replaced is refused, never read as this posting's rows.
    unsigned long long postedEsampleSerial = 0;
    // FU-1176: an `ereturn post` e() with an e(cmd) (an eclass program's own
    // results): no fitted model and no estimation block, only the e() stores
    // and the coeftable mirror in lastModelResult. `estimates restore`
    // reinstalls it as the posted e() it was.
    bool postedE = false;
    // BUG-1414: the fit's published e() name lists (eNameContract) as they
    // stood at the store. The restored LastModel keeps its fit serial, so after
    // `estimates restore` the lists answer again, as they do right after the
    // fit (StataBE 19.5: regress, store, `ereturn clear`, restore, then
    // `matrix S = e(nosuch)` is the 1x1 missing, rc 0).
    std::optional<ENameContract> eNameContract;
  };
  std::map<std::string, std::shared_ptr<const StoredModel>> estimatesModels;

  // The data files a STORED model replays (`--data`/`--fit-data`), pinned in
  // `scratchPins` -- reclaimSupersededScratch's exclusion (6) -- by refcount:
  // two stored models can name one file, and a scoped ScratchPin on the same
  // path must not release it when its command ends (~ScratchPin checks here).
  // Without it, `reg y x; est sto m1; <mutations>; reg y z; est restore m1;
  // predict p` replayed a file the GC had already unlinked ("cannot open").
  std::map<std::string, int> storedModelPinRefs;

  void pinStoredModel(const StoredModel& sm, int delta) {
    if (!sm.lastModel.has_value()) return;
    for (const std::string* p : {&sm.lastModel->dataPath, &sm.lastModel->fitDataPath}) {
      if (p->empty()) continue;
      int& refs = storedModelPinRefs[*p];
      refs += delta;
      if (refs > 0) {
        scratchPins.insert(*p);
      } else {
        storedModelPinRefs.erase(*p);
        scratchPins.erase(*p);
      }
    }
  }

  // The ONE writer of estimatesModels: a re-store under the same name releases
  // the previous model's pins before the new model takes its own.
  void estimatesModelSet(const std::string& name, std::shared_ptr<const StoredModel> sm) {
    // W2D-T01 round 3: the snapshot of the ACTIVE e() (eststo_esttab.cpp, the
    // only caller) also carries the names that sit on top of the fit.
    if (sm && sm->lastModel.has_value() && !eOnFitNames.empty()) {
      auto withNames = std::make_shared<StoredModel>(*sm);
      withNames->eOnFitNames = eOnFitNames;
      sm = std::move(withNames);
    }
    estimatesModelErase(name);
    if (sm) pinStoredModel(*sm, +1);
    estimatesModels[name] = std::move(sm);
  }
  void estimatesModelErase(const std::string& name) {
    const auto it = estimatesModels.find(name);
    if (it == estimatesModels.end()) return;
    if (it->second) pinStoredModel(*it->second, -1);
    estimatesModels.erase(it);
  }

  // Stata's hidden e(_estimates_name): the store name the ACTIVE results carry
  // (set by `estimates store`/`eststo` and `estimates restore`, cleared by a
  // new fit in setLastModel/lastModel_reset, by estpost's posting and by
  // dropping that name). Read through activeStoredName(), which also demands
  // that the live e() block IS the stored one, by full value: `lastEstBlock`
  // is assigned directly by TUs that never pass through setLastModel (csdid),
  // and a fingerprint (depvar|estimator|N|k) cannot tell two such postings
  // apart -- `esttab .` then rendered an older stored table.
  std::string activeEstimatesName;

  std::string activeStoredName() const {
    if (activeEstimatesName.empty() || estimatesGet(activeEstimatesName) == nullptr) return {};
    const auto it = estimatesModels.find(activeEstimatesName);
    if (it == estimatesModels.end() || !it->second) return {};
    return it->second->estBlock == lastEstBlock ? activeEstimatesName : std::string();
  }

  // W2 esttabnames (BUG-553): `estimates restore NAME` -- make the stored
  // model the ACTIVE one, as `estimates use` does from a file
  // (runEstimatesUse): setLastModel() first (it re-points the _b[]/e()
  // resolver), then the e() display block, then the macro layer's e() exactly
  // as it stood when NAME was stored (so an estadd posted to a LATER model
  // cannot answer `e(fe)` for this one), then whatever `estadd ...: NAME`
  // added to the stored bag since. Oracle: w2_cmdtriage.log section C --
  // after `estimates restore m1`, e(df_m) and _b[a] are m1's
  // (1, .4848484848484848), not the later fit's (2, .4875776397515528).
  // Returns false when NAME has no model PIET can reactivate (stored from
  // results with no replayable model, e.g. estpost) and is not already the
  // active results; the caller refuses by name. The §31 engine handle is
  // dropped: the next lincom/test re-specifies the model rather than trusting
  // a handle minted in an earlier state of the worker. e(sample) is left a named
  // r(198) here (kUnavailable); the one caller, handlers' runEstimatesRestore
  // (eststo_esttab.cpp), then installs the mask from `_est_NAME` (T3).
  bool estimatesRestore(const std::string& name) {
    const auto it = estimatesModels.find(name);
    if (it == estimatesModels.end() || !it->second) return false;
    const std::shared_ptr<const StoredModel> sm = it->second;
    if (!sm->lastModel.has_value()) {
      if (activeStoredName() == name) return true;
      // eb-order T-P3 (SPEC C, `estimates restore` of an estpost store): a
      // store with an estimation block but no fitted model behind it (an
      // estpost posting; runEststo stashes its e() stores) is installed as the
      // posted e() it was -- no LastModel, its block, its stores -- exactly as
      // postPseudoEstimate installed it, so the next fit supersedes it.
      if (!sm->estBlock.has_value() && !sm->postedE) return false;
      lastModel_reset();
      ePostedLive = true;
      ePostedEsampleSerial = sm->postedEsampleSerial;
    } else {
      LastModel lm = *sm->lastModel;
      lm.estHandle.clear();
      // e(sample) T2: the stored LastModel may say kColumn, but the column on the
      // data now holds whichever fit ran LAST, not this one's sample. The default
      // is the named refusal, never another model's rows; runEstimatesRestore
      // replaces it with the mask it installs from `_est_NAME` (T3).
      lm.esample = LastModel::EsampleState::kUnavailable;
      setLastModel(std::move(lm));
    }
    lastEstBlock = sm->estBlock;
    eval.lastModelResult = sm->lastModelResult;
    eval.eScalars = sm->eScalars;
    eval.eStrings = sm->eStrings;
    eval.eMatrices = sm->eMatrices;
    eOnFitNames = sm->eOnFitNames;  // round 3: setLastModel above emptied the live record
    eNameContract = sm->eNameContract;
    estAddedClear();
    const nlohmann::ordered_json* snap = estimatesGet(name);
    if (snap != nullptr && snap->contains("added") && (*snap)["added"].is_object()) {
      const nlohmann::ordered_json& bag = (*snap)["added"];
      if (lastEstBlock.has_value()) {
        estAdded = bag;
        estAddedFp = estAddFingerprint(*lastEstBlock);
      }
      republishAddedBag(bag);
    }
    activeEstimatesName = name;
    return true;
  }

  // The stored bag back into e(), the way runEstadd posts a live addition
  // (mirrorIntoEStores + the lastModelResult merge that never shadows a
  // fitted key). Re-applying what the store-time stash already holds is a
  // no-op; what it adds is an `estadd ... : NAME` made AFTER the store.
  void republishAddedBag(const nlohmann::ordered_json& bag) {
    if (bag.contains("scalars") && bag["scalars"].is_object()) {
      for (const auto& [n, v] : bag["scalars"].items()) {
        if (!v.is_number()) continue;
        eval.setEScalar(n, v.get<double>());
        if (eval.lastModelResult.has_value() && eval.lastModelResult->is_object() &&
            !eval.lastModelResult->contains(n)) {
          (*eval.lastModelResult)[n] = v;
        }
      }
    }
    if (bag.contains("locals") && bag["locals"].is_object()) {
      for (const auto& [n, v] : bag["locals"].items()) {
        if (v.is_string()) eval.setEString(n, v.get<std::string>());
      }
    }
    if (bag.contains("matrices") && bag["matrices"].is_object()) {
      for (const auto& [n, m] : bag["matrices"].items()) {
        eval::EvalMatrix em;
        if (m.contains("rownames") && m["rownames"].is_array())
          em.rownames = m["rownames"].get<std::vector<std::string>>();
        if (m.contains("colnames") && m["colnames"].is_array())
          em.colnames = m["colnames"].get<std::vector<std::string>>();
        if (m.contains("data") && m["data"].is_array()) {
          for (const auto& row : m["data"]) {
            std::vector<double> r;
            if (row.is_array()) {
              for (const auto& cell : row) {
                r.push_back(cell.is_number() ? cell.get<double>()
                                             : std::numeric_limits<double>::quiet_NaN());
              }
            }
            em.data.push_back(std::move(r));
          }
        }
        bool replaced = false;
        for (auto& kv : eval.eMatrices) {
          if (kv.first == n) { kv.second = em; replaced = true; break; }
        }
        if (!replaced) eval.eMatrices.emplace_back(n, std::move(em));
      }
    }
  }

  // Stored-estimates namelist expansion (BUG-552), shared by every consumer of
  // a namelist. rc==0 on success; otherwise rc/message carry the Stata error.
  struct EstNamelist {
    std::vector<std::string> names;
    int rc = 0;
    std::string message;
  };
  //
  // Namelist grammar per [R] estimates store/table: each token is `_all`
  // (every stored name), `.` (the active results), or a Stata match()
  // pattern over the stored names. Groups are concatenated in written order;
  // within a group the store order is kept; duplicates are kept (consumers
  // that want a set de-duplicate themselves). The first failing token decides
  // the error and the name list is then empty. Never throws.
  EstNamelist expandEstimatesNamelist(const std::vector<std::string>& tokens) const {
    EstNamelist out;
    auto fail = [&out](int rc, std::string msg) {
      out.names.clear();
      out.rc = rc;
      out.message = std::move(msg);
    };
    try {
      for (const std::string& tok : tokens) {
        if (tok == "_all") {
          // Empty store: nothing appended, not an error.
          for (const auto& kv : estimates) out.names.push_back(kv.first);
          continue;
        }
        if (tok == ".") {
          const bool haveActive = lastModel_.has_value() || lastEstBlock.has_value();
          if (!haveActive) {
            fail(301, "last estimates (.) not found");
            return out;
          }
          std::string stored = activeStoredName();
          out.names.push_back(stored.empty() ? std::string(".") : std::move(stored));
          continue;
        }
        bool hit = false;
        for (const auto& kv : estimates) {
          if (piet::strutil::globMatch(tok, kv.first)) {
            out.names.push_back(kv.first);
            hit = true;
          }
        }
        if (!hit) {
          fail(111, "estimation result " + tok + " not found");
          return out;
        }
      }
    } catch (...) {
      fail(198, "invalid namelist");
    }
    return out;
  }

  // -- PP-L2A / K1a: same-file `use` conversion cache ------------------------
  // Lookup returns the memo for `src` ONLY if its stored statKey equals the
  // freshly-computed `statKey` the caller just measured; nullptr otherwise
  // (absent, moved, or an unkeyable source — the caller then converts). The
  // caller still has to check that the memoed files exist: eviction is
  // suppressed for cached paths (reclaimSupersededScratch's exclusion 3) but
  // an external `rm` of workdir_ is not this cache's problem to trust away.
  const UseConvCacheEntry* useConvCacheGet(const std::string& src,
                                           const std::string& statKey) const {
    if (statKey.empty()) return nullptr;
    auto it = useConvCache_.find(src);
    if (it == useConvCache_.end() || it->second.statKey != statKey) return nullptr;
    useCacheTouch_[src] = ++useCacheClock_;  // BUG-390: recency for the byte-LRU
    return &it->second;
  }
  // Memoize a conversion just completed for `src`. A superseded memo's hbin is
  // released to the ordinary scratch GC first (the entry is erased BEFORE the
  // reclaim call, so exclusion 3 no longer pins it) — otherwise a source that
  // legitimately changes mid-session would strand one hbin per re-conversion.
  // Definition in session_state.cpp (reclaimSupersededScratch is private there).
  void useConvCachePut(const std::string& src, const UseConvCacheEntry& entry);

  // -- PP-L2B / K1b: same-file `use` resident-stash cache --------------------
  // Same guard contract as useConvCacheGet: the memo for `src` ONLY when its
  // stored statKey matches the caller's freshly-measured one; nullptr otherwise.
  // A hit still requires the caller to check the on-disk twin (hbinPath) exists
  // AND that residentRestore actually succeeds — a stash lost to a worker
  // restart returns a live memo whose frame is gone, and the restore's own
  // false result is the honest signal to fall through to disk.
  const UseStashCacheEntry* useStashCacheGet(const std::string& src,
                                             const std::string& statKey) const {
    if (statKey.empty()) return nullptr;
    auto it = useStashCache_.find(src);
    if (it == useStashCache_.end() || it->second.statKey != statKey) return nullptr;
    useCacheTouch_[src] = ++useCacheClock_;  // BUG-390
    return &it->second;
  }
  // The stable per-source stash name (client-owned "__usecache:" namespace).
  static std::string useStashName(const std::string& src) { return "__usecache:" + src; }
  // Stash the CURRENT resident head under `name` for later K1b restore. Syncs
  // the worker to the head first (residentSyncWorkerToHead), then frameStash.
  // Returns false when the resident plane is unavailable (§26-only engine /
  // serve disabled / worker down) — the caller simply does not memoize, and the
  // next `use` pays the ordinary disk path (honest tri-state fallback).
  bool useStashSnapshot(const std::string& name);
  // Memoize a stash just taken for `src`. A superseded entry is overwritten in
  // place (the stash name is stable per source, so useStashSnapshot already
  // replaced the worker-RAM frame); the old hbinPath's reclaim is owned by K1a
  // (the shared conversion output) or is a user file outside workdir_, so no
  // reclaim happens here. Definition in session_state.cpp (header-inlinable but
  // kept beside useConvCachePut for symmetry).
  void useStashCachePut(const std::string& src, const UseStashCacheEntry& entry);

 private:
  // 2.2.0 item E: register `oldPath` as reclaimable scratch iff it is a
  // session-owned scratch hbin — contained in workdir_ (never a user file) AND
  // not referenced by any live preserve snapshot (preserveStack) — then evict
  // the oldest registered files while the total exceeds the scratch budget.
  // Best-effort, never throws. Definition in session_state.cpp (needs
  // <filesystem>, kept out of this header).
  // `abandoningHead=true` is for the two callers that reclaim the head they are
  // about to leave (clearData, residentRestore): exclusion (0) — never the live
  // head — is skipped for them, because for them the head IS the superseded path.
  void reclaimSupersededScratch(const std::string& oldPath, bool abandoningHead = false);

  // Registered superseded scratch files (oldest first) + their running byte
  // total, for the budget-bounded eviction above.
  std::vector<std::pair<std::string, std::uintmax_t>> supersededScratch_;
  std::uintmax_t supersededBytes_ = 0;

  // PP-L2A / K1a: resolved-source-path -> completed conversion memo. Session-
  // scoped (dies with workdir_), so nothing survives into another process and
  // there is no on-disk cache index to keep coherent. Read by runUse only.
  std::map<std::string, UseConvCacheEntry> useConvCache_;
  // C4-L4: the single-entry resident computed-column cache (public API above).
  // Session-scoped, bounded by construction (exactly one column), invalidated
  // by real-file load() / any fold touching its name / row-count drift.
  std::optional<std::string> computedColName_;
  ResidentComputedCol computedCol_;
  // PP-L2B / K1b: resolved-source-path -> worker-RAM stash memo. Same session
  // scope; a live entry pins its hbinPath against the scratch GC (exclusion 4).
  std::map<std::string, UseStashCacheEntry> useStashCache_;
  // BUG-390: recency clock for the byte-LRU over both memos (keyed by source).
  mutable std::map<std::string, std::uint64_t> useCacheTouch_;
  mutable std::uint64_t useCacheClock_ = 0;
  void enforceUseCacheCap();
  void dropUseCacheEntry(const std::string& src);
  // BUG-389: every path some ledger owns, as raw AND canonical keys → owner name.
  std::map<std::string, std::string> scratchOwners() const;
  int lastOrphanSweepRemoved_ = 0;
  unsigned long long mintSeqAtLastSweep_ = 0;

  // BUG-189 (see logEchoSuppressed above). `logVerbosity_` is the innermost-wins
  // stack; `logQuietDepth_` is the "am I anywhere inside a quietly" count, which
  // a stack top cannot answer once a noisily is pushed on top of it.
  std::vector<bool> logVerbosity_;
  int logQuietDepth_ = 0;
  LineVis lineVis_ = LineVis::None;

  // -- Stage 1b resident-frame state (all inert until the first frame op) ----
  CobraSession* ensureCobraSession();  // lazy spawn-on-first-use
  void flushDeferredReclaim();         // reclaim files a virtual chain pinned
  // C4-L2A: takes the whole CobraFrameMeta (not just .vars) so every card
  // seed also captures meta.rows into residentRows_ — the "seedShadowFrom
  // FrameVars pair" rule: rows ride wherever the card rides, gated on
  // meta.valid (an unparsed frame block seeds the card exactly as before but
  // leaves rows unknown rather than trusting a default-constructed 0).
  void seedShadowFromFrameVars(const CobraFrameMeta& meta);
  // §27 shared precondition: resident plane usable AND worker synced to the
  // current head (materializes a dirty virtual head for the crash floor first).
  bool residentSyncWorkerToHead();
  // Post-op death/respawn handling (best-effort; not gate-exercised): one-shot
  // fallback on a real head, rollback-to-floor on a lost virtual head.
  CobraRunResult finishSessionOp(CobraRunResult r, const std::vector<std::string>& argv);
  CobraRunResult rollbackToFloor(const std::vector<std::string>& argv);

  std::unique_ptr<CobraSession> cobraSession_;
  // C4-L1 trace attribution: first token of the command being dispatched
  // (dispatcher.cpp's dispatchImpl entry). Written only when $PIET_C4_TRACE is
  // set; "" renders as "-" in the trace line.
  std::string c4TraceVerb_;
  // FIX-C4COUNTER-SAMPLERSPAN/BUG-142: true only for the span of a
  // counter-neutral diagnostic walk (liveNamedBytesNeutral). A closure fired
  // inside that span has its snapshot_count restored, so its trace line must
  // not print either — trace and snapshot_count are the same series by
  // construction (ensureMaterializedHead), and a half-recorded line gets
  // attributed to whatever verb was dispatched LAST (c4TraceVerb_ is stale by
  // then), which is how L1B read a diagnostic-induced closure as a
  // "foreach-body closure". Checked only at the trace emit; never affects the
  // physical snapshot itself.
  bool c4TraceSuppress_ = false;
  std::string residentToken_;         // path the worker's resident frame corresponds to ("" = out of sync)
  bool residentDirty_ = false;        // resident frame not yet on disk at residentToken_ (VIRTUAL head)
  std::vector<std::string> preInstallPins_;  // e(sample) T2: see pinPreInstallHead
  bool preInstallLoadPending_ = false;        // the next load() is the install's own
  long long tokenOrphanWarns_ = 0;    // BUG-384 audit hits (see auditHeadBacked)
  std::string lastHeadAudited_;       // BUG-384: one stat per distinct head
  bool lastRunResident_ = false;      // last runViaCobraSession() took the resident leg (see lastRunWasResident)
  std::string lastMaterializedHead_;  // last on-disk real head — crash-recovery floor
  CobraFrameMeta wireVars_;           // MUTRES-WIRE: buffer for fetchWireVarsFromWorker()
  CobraFrameMeta pendingDelta_;       // last mutation's frame delta (shadow-card source)
  bool pendingDeltaValid_ = false;
  // fix/verb-overhead-sweep S1b: the column-SET/ORDER change the just-issued
  // resident mutation is about to make, read off the argv the intercept is
  // sending. That argv is the only place PIET knows it: `order`'s reorder spec
  // and `eval --drop`'s removal list never reach mutationResult's 3-list
  // signature, and the engine's run response reports only COUNTS for them
  // (changed/added/dropped) plus a vars_delta of the changed/added columns —
  // never the surviving order, never the dropped names (cobra_session.h
  // CobraFrameMeta). Written at the same site as pendingDelta_ and consumed +
  // cleared at the same site, so a plan can never be applied to a DIFFERENT
  // mutation's delta. Kind::None (the default, and every argv shape the parser
  // does not recognize with certainty) means "no fold is possible here" and
  // the caller rebuilds the card exactly as before.
  struct CardFoldPlan {
    enum class Kind { None, Order, Drop, Rename };
    Kind kind = Kind::None;
    std::vector<std::string> vars;  // Order: the reorder list; Drop/Rename: the OLD names
    std::vector<std::string> renameTo;  // Rename only: the new name for vars[i]
    std::string mode;               // Order only: "first" | "last" | "before" | "after"
    std::string anchor;             // Order only: the before()/after() anchor
  };
  CardFoldPlan pendingCardPlan_;
  // Parse `argv` into pendingCardPlan_ (Kind::None on anything unrecognized).
  void capturePendingCardPlan(const std::vector<std::string>& argv);
  // Apply `plan` to shadowCard_ in place. Returns false — leaving the card
  // untouched — for every case the plan cannot be proven to describe exactly,
  // so the caller falls back to the full rebuild.
  bool applyPendingCardPlan(const CardFoldPlan& plan, const CobraFrameMeta& delta);
  static inline int64_t cardFoldCount_ = 0;  // selftest instrumentation — see cardFoldCount()
  static inline int64_t residentLiveCount_ = 0;  // selftest instrumentation — see residentLiveCount()
  std::vector<VarInfo> shadowCard_;   // resident base varlist (label remap applied at render)
  bool shadowValid_ = false;
  // C4-L2A (D1 card plane): the row count the card's own writer captured —
  // CobraFrameMeta.rows at every seedShadowFromFrameVars pair, ds->nRows() at
  // reseedShadowFrom, delta.rows at the mutation-card fold (the same number
  // that fold already trusts as n_rows_after). -1 = not captured (pre-first-
  // seed, or a meta the writer could not vouch for) → residentRowsIfCurrent()
  // answers nullopt and the consumer falls back. Shares shadowValid_/
  // shadowCardHbin_'s currency governance — no separate validity bit.
  int64_t residentRows_ = -1;
  // fix/sort-validate-lazy: the hbin path shadowCard_ currently describes —
  // set alongside shadowValid_=true at every writer (seedShadowFromFrameVars,
  // reseedShadowFrom, tryResidentMutationCard's fold). NOT always currentHbin_
  // (a resident resync onto a post-estimation model's dataPath tags this with
  // THAT path, not currentHbin_, when they diverge) — see
  // shadowVarlistIfCurrent()'s doc comment for why the comparison against
  // currentHbin_ is exactly the safety check a new consumer needs.
  std::string shadowCardHbin_;
  std::vector<std::string> deferredReclaim_;  // reals pinned while the head is virtual
  int residentEligible_ = 0;          // >0 => a mutation in scope may use the resident frame plane
  int statelessRead_ = 0;             // >0 => read-only calls in scope stay stateless (C1-④)
  // §27 session snapshots (MAXPERF Stage 2): paths registered via save_virtual
  // (so runUse routes them virtual-first) + a monotonic key/token minter shared
  // by stash names and restored-head tokens.
  std::unordered_set<std::string> virtualSaves_;
  // BUG-360 follow-up: section-38 getcols projections awaiting the reap at the
  // outermost dispatch boundary. See noteProjectionScratch() above.
  std::vector<std::string> projectionScratch_;
  // BUG-198: the format/label layer a virtual (RAM-only) tempfile save would
  // otherwise have no header to carry. Keyed by the same path virtualSaves_ is.
  std::unordered_map<std::string, std::vector<cecon::ColMetaPatch>> virtualSaveMeta_;
  // BUG-659: the ts declaration live when a native checkpoint was saved, keyed
  // by the resolved path runSaveData wrote (see noteCheckpointTsDecl()).
  std::unordered_map<std::string, CheckpointTsDecl> checkpointTsDecl_;
  long stashSeq_ = 0;

  std::string workdir_;
  std::optional<std::string> currentHbin_;
  std::string currentFile_;  // P8 U1: last successfully-loaded dataset path; "" initially
  std::unordered_map<std::string, int64_t> nMissCache_;  // C2-F13a: per-column nMissing carried across mutations
  std::unique_ptr<Datastore> datastore_;
  // ALLOC-C3A-ARC1-CARRY: the descriptor in flight between a mutation's load()
  // and the datastore() that follows it. Only ever non-empty inside a CarryArm
  // scope (see CarryArm above) — these are not a cache.
  std::vector<cecon::Column> carried_;
  std::string carryTarget_;
  cecon::CarryDelta carryDelta_;
  bool carryArmed_ = false;
  // ALLOC-C3A-ARC2A-PREVMAP: the PREVIOUS frame's Datastore, spent of its columns
  // but kept alive for its mmap alone, plus the span that names it. Held only
  // between CarryArm's constructor and releaseCarryPrev() — the next open, or the
  // arm's destructor, whichever comes first — so this is a deferral, not a cache
  // (a process-wide "last mapping" would be exactly the silent-SKIP surface
  // CarryArm's own doc refuses, [[cache-invalidates-test-proxy]]). Declared AFTER
  // datastore_ so a SessionState teardown destroys this one first; neither points
  // at the other, so the order is hygiene rather than a requirement.
  std::unique_ptr<Datastore> carryPrev_;
  cecon::CarrySource carrySrc_;
  cecon::CarryOutcome lastCarryOutcome_ = cecon::CarryOutcome::FallbackNoCarried;
  std::optional<LastModel> lastModel_;
  std::vector<HistoryEntry> history_;
  RunProgress progress_;
  mutable std::mutex progressMutex_;

  // -- _request() pending-answer state (BUG-087/D9) -------------------------
  // Deliberately a SEPARATE mutex/CV from progressMutex_ above (no lock-
  // order coupling between them anywhere in this class — requestUserInput()
  // calls progressCancelRequested(), which takes progressMutex_ internally,
  // while ALREADY holding requestMutex_, but never the reverse, so this
  // can't deadlock). pendingRequests_ maps requestId -> answer-once-set;
  // requestSeq_ is a simple incrementing counter (stringified) under the
  // same mutex — no UUID machinery needed for a same-process request/answer
  // round trip.
  std::mutex requestMutex_;
  std::condition_variable requestCv_;
  std::map<std::string, std::optional<std::string>> pendingRequests_;
  long long requestSeq_ = 0;
  PostEventFn requestPostEvent_;

  // BUG-028 / BS-006: the open log (nullptr when none is open — the default,
  // common case) and the dispatch()-recursion depth counter that gates
  // logTee() to the outermost call only (see the public methods' doc
  // comments above and dispatcher.cpp's dispatch()/dispatchImpl() split).
  // W:P8: was a single slot; now every open log in open order (the unnamed one
  // plus any `name()`d ones). Empty for the overwhelming majority of sessions,
  // which therefore still pay nothing and stay byte-identical.
  std::vector<std::unique_ptr<LogState>> logs_;
  // SMCL logs that `log close` has already removed from logs_ but whose last
  // frame and closed-on footer logTee() has not written yet (LogOp::Close).
  std::vector<std::unique_ptr<LogState>> endingLogs_;
  int dispatchDepth_ = 0;
};

}  // namespace piet

#endif  // PIET_NATIVE_SESSION_STATE_H
