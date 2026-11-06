# serve_parity -- the dual-run parity leg (gate (1) for MAXPERF Stage 1)

Owner: Eval Harness (CONSTITUTION Art 10 -- Eval owns `tests/eval/**`).

Request: MSG-MI-237 item 5 (heads-up) / MI-242 (`<private-coordination-tree>/messenger/
eval-harness/inbox.md`). Design source: `internal notes
GATE23-MAXPERF-design-20260717.md` SS4 "Stage 1 -- persistent engine
session (`cobra serve`)" + SS5.2 "Dual-run parity leg". Authored against
piet **B.3.0** (main @4b5ac08, op-eval `a25b7cf` merged forward), 2026-07-18.
`cobra serve` does not exist yet on either engine build available at
authoring time (`<private-coordination-tree>/releases/engine/current/cobra` 0.27.0, `op-cobra`
worktree `fix/serve-stage1`@4b5ac08) -- this is intentional, golden-first:
the harness lands BEFORE the feature.

## What this gates

Once `cobra serve` exists, this is the **(1) gate for Stage 1**: every
release stamp that touches the serve path must show `serve-parity: N/N
identical (0 skipped)` -- any RED is a byte-level behavioral difference
between session-mode and one-shot-mode for identical argv, which the
Stage-1 architecture decision (design doc SS4) treats as a correctness bug,
not a perf tradeoff.

Until then, it runs gracefully SKIP-everything against any engine lacking
`serve` (exit 0), which is itself a useful smoke: it proves the harness's
plumbing (case sourcing, argv construction, fixture prep, comparison,
reporting) all work correctly on the *known-good* one-shot half, ready to
light up the moment the engine side lands.

## Case sourcing -- imports, not a parallel corpus

This tool imports `../run_eval.py` as a module (`sys.path.insert` +
`import run_eval as RE`) and calls **its own functions directly**:

- `RE.load_manifest(path)` -- the exact 101-case corpus (`tests/eval/
  manifest.json`) the standing gate runs. Same cases, same order, same
  filtering semantics (`--only`/`--group` mirror run_eval.py's flags).
- `RE.run_prep(prep, data_dir)` -- executed **for real**, one-shot, to
  materialize a case's fixture (11/101 cases have a `prep` block: 4 `gen`,
  2 `recode`, 1 `collapse`, 4 `convert`). This is the actual function the
  gate calls -- not a reimplementation of its convert/recode/gen/collapse
  branching. The one addition is a transparent tap on the `subprocess.run`
  call it makes internally (`_one_shot_prep`), which *observes* the exact
  argv `run_prep` executed (needed to replay the identical argv through
  `cobra serve` later) without changing what `run_prep` does or duplicating
  its logic.
- `RE.run_cobra(command, args, data_dir, overrides=...)` -- executed for
  real, one-shot, for every case's main invocation. This is byte-identical
  to what `run_eval.py`'s own gate loop does (`--format tidy` appended,
  same `resolve_args` placeholder substitution, same fixture-override
  wiring for prep cases).

Nothing about *which* cobra invocations run, in what order, or with what
arguments is reinvented here -- only the **transport** differs (fresh
subprocess-per-call vs. one persistent `cobra serve` session), which is
exactly the axis this leg is designed to test.

## Design: two phases, one persistent session

1. **One-shot phase** (`run_one_shot_phase`): every case's prep step (if
   any) + main command run as **fresh subprocesses**, in natural manifest
   order -- this is what `run_eval.py` already does and is unaffected by
   `--shuffle` (a one-shot process's output cannot depend on invocation
   order; there's no state to leak).
2. **Serve phase** (`run_serve_phase`): the SAME argv sequence (prep, main,
   prep, main, ... across the whole corpus) is sent as requests to **ONE**
   `cobra serve` subprocess for the entire run -- persistence across the
   whole corpus, in corpus order, is the point: it is what would surface
   request-to-request state leakage that comparing isolated one-shot pairs
   could never see.

A case's own prep-then-main pair is always sent adjacently within the
serve request stream (main's args reference the prep's materialized `--out`
path, so prep must complete first) -- see "Order-sensitivity guard" below
for what *can* be reordered.

Fixture files land at the same deterministic path `run_eval.run_prep`
already uses (`/tmp/eval_prep_<as>.hbin`) in *both* phases. This is safe
without cross-phase interference because the phases are strictly
sequential (the whole one-shot phase completes -- prep written, prep's own
main already consumed it -- before the serve phase starts re-deriving the
same paths).

## Order-sensitivity guard (`--shuffle SEED`)

Default off (natural manifest order sent to serve, matching the 101-case
gate's own iteration order). `--shuffle SEED` reorders **which case's
prep+main pair goes when** in the serve request stream (via
`random.Random(SEED).shuffle`) without ever splitting a pair -- this is
what would catch a bug where, say, request N's server-side state leaks
into request N+1 only when N+1 happens to be a *different* case than in
the recorded corpus order. Comparison is keyed by case id, not by send
position, so a shuffled serve run still grades correctly against the
(always-natural-order) one-shot recordings.

**Recommended standing-gate cadence** (per MI-237 item 5's own framing,
once wired): run this tool **twice** per gate -- once plain (natural
order) and once `--shuffle <any fixed seed>` -- both must be `N/N
identical (0 skipped)`. A single plain pass alone cannot distinguish "no
bug" from "the corpus's own natural order happens not to trigger the
leak".

## Timing-field masking -- a load-bearing finding, read this

cobra's own JSON schema embeds **real wall-clock timing**, which is
non-deterministic *by construction* -- confirmed at the source (grep
across `op-cobra`, current as of this authoring):

- `apps/cmds/cmds_linear.cpp:255`, `apps/cmds/cmds_panel.cpp:161/263/378/
  467/553`, `apps/cmds/common.cpp:1122` -- a top-level scalar
  `"time_ms": <float>`.
- `apps/cmds/common.cpp:811/914/1048/1213/1298` -- a nested object
  `"timing_ms": {"load": ..., "build": ..., "fit": ...}`.

These are the **only two field names** used anywhere in the engine for
this (verified by grep; nothing named `elapsed_ms`/`wall_ms`/etc. exists).
Two back-to-back **one-shot** invocations of the identical binary on the
identical argv already differ in these fields -- confirmed live (see
"Live validation" below) -- so **literal byte-equality would RED on
essentially every estimator case** (most of the 101-case corpus) purely
from timing jitter, regardless of whether `serve` is behaviorally correct.
A gate that is permanently RED for a reason unrelated to correctness is
worse than no gate (it trains people to ignore RED).

**Default behavior:** both fields are masked (regex-replaced with a
`"<masked>"` placeholder, preserving everything else byte-for-byte
including original key order and every other field's exact formatting)
before the stdout/stderr comparison. `--strict-bytes` disables masking for
a literal, no-exceptions byte comparison -- useful for manual inspection
of what the *raw* difference looks like, expected to show mass RED against
a real engine on any case whose JSON carries either field (confirmed live,
see below) and **not recommended as the standing-gate invocation**.

This is an Art-10 tolerance call (masking, not a numeric tolerance, but
the same authority), made because the alternative (matching the literal
"byte-equality" wording in the commission) would make the leg
non-actionable. Flagging prominently per the design doc's own "Honesty
ledger" discipline (SS7) -- the maintainer should be aware `serve-parity: N/N
identical` means "identical modulo declared-volatile timing fields", not
"byte-for-byte including timing". If cobra's JSON schema ever adds a new
non-deterministic field under a different name, extend `_TIME_MS_RE`/
`_TIMING_MS_RE` (or the masking approach generally) here -- Eval's job
(Art 10), not the requester's.

## Live validation (this stint, against current-engine binaries)

- **Graceful SKIP path** (the required day-1 behavior, since `serve`
  doesn't exist): full 101-case corpus against both `<private-coordination-tree>/releases/
  engine/current/cobra` (0.27.0) and the main worktree's `build/cobra`
  -- both `serve-parity: 0/0 identical (101 skipped)`, exit 0, one-shot
  phase genuinely executed (~6s wall for the real one-shot half, confirming
  it isn't a stub). `--require-serve` correctly flipped this to exit 1 at
  the time. (FIX-SERVE-REQUIRE has since inverted the default: serve-
  unavailable is now a hard RED by default, and the opt-in
  `--allow-no-serve` flag is what reproduces this graceful-SKIP/exit-0
  result today.)
- **Happy-path + RED-detection**, since no real `serve` build exists to
  test against: validated with a throwaway local double (a tiny wrapper
  that implements the pinned protocol -- handshake, request/response,
  shutdown -- by delegating every request to the real one-shot binary, so
  a correct implementation is byte-identical to itself by construction).
  Full 101-case corpus: `101/101 identical (0 skipped)`, ~14s, exit 0 --
  covers every prep kind (gen/recode/collapse/convert) and every
  `oracle_kind` (golden/smoke/reject) actually present in the manifest. A
  deliberately corrupted single response (`FLIP=1` in the double) was
  correctly caught as RED with the expected hexdump excerpt, and forced
  exit 1. `--shuffle 42` on a 15-case slice (including both prep cases in
  that slice) stayed `15/15 identical`, confirming prep/main adjacency
  survives reordering. This double is **not part of the deliverable** (it
  lived in a scratch dir, not committed) -- it exists only to prove the
  harness's own logic before the real engine feature exists to test
  against.
- **The masking finding itself**: with the local double and
  `--strict-bytes`, the SAME correct delegate showed `5/10 identical`
  purely from `time_ms`/`timing_ms` jitter (5 of the 10-case slice were
  estimator commands) -- concrete proof the masking is load-bearing, not
  precautionary.

## Usage

```
# Standing-gate invocation (once cobra serve exists):
python3 run_serve_parity.py --cobra ENGINE_BIN
python3 run_serve_parity.py --cobra ENGINE_BIN --shuffle 1337   # second pass, per "Order-sensitivity guard"

# Smoke (fast, a handful of cases):
python3 run_serve_parity.py --cobra ENGINE_BIN --limit 20

# Filter like run_eval.py:
python3 run_serve_parity.py --cobra ENGINE_BIN --only feols,nbreg
python3 run_serve_parity.py --cobra ENGINE_BIN --group csdid

# Full JSON verdict dump:
python3 run_serve_parity.py --cobra ENGINE_BIN --json /tmp/out.json

# Golden-first landing window opt-in (accepts the old graceful-SKIP/exit-0
# result instead of the default hard RED when serve is unavailable):
python3 run_serve_parity.py --cobra ENGINE_BIN --allow-no-serve
```

`--data-dir`/`--manifest` default to the same resolution `run_eval.py`
uses (`RE.DATA_DIR`/`RE.EVAL/manifest.json`); override the same way (e.g.
`COBRA_EVAL_DATA` env var, consumed by `run_eval._resolve_data_dir()`,
still applies since this tool imports that module rather than
reimplementing path resolution).

## Output

Per-case `PASS`/`RED`/`SKIP` line (`RED` includes a bounded hexdump-style
excerpt of the first divergent byte in whichever stream(s) diverged;
`SKIP` states the one-line reason -- either "serve mode unavailable
(...)" globally, or a per-case "prep failed in one-shot mode: ..." if a
fixture couldn't be materialized). Summary line:

```
serve-parity: X/Y identical (Z skipped)
```

`Y` = cases actually compared (both halves ran); `X` = how many were
byte-identical (post-masking, unless `--strict-bytes`); `Z` = cases that
could not be compared at all (globally, when serve is unavailable; or
individually, on a prep failure). Exit 0 unless (a) any case is `RED`, or
(b) serve was globally unavailable and `--allow-no-serve` was NOT passed
(the default -- serve-absence is a hard RED unless explicitly waived).

## Files

- `run_serve_parity.py` -- the tool. Self-contained; only dependency is
  `../run_eval.py` (imported, never copied).

## Wiring into the standing gate (the maintainer's job, once `cobra serve` lands)

Same spirit as `piet_maxperf/README.md`'s and `piet_budget/README.md`'s
"Flipping to enforced" sections:

1. Confirm the real handshake shape matches what `ServeSession.__init__`
   expects (`{"serve":1,...}` on the first stdout line) -- if the landed
   protocol differs, this is an Eval update (Art 10), not a request back
   to the engine side.
2. Run plain + one `--shuffle` pass (see "Order-sensitivity guard"); both
   must be `N/N identical (0 skipped)` before this leg is wired as a
   release-checklist gate.
3. Add it as a separate subprocess step in `run_eval.py` (the
   `--with-ctest` pattern, mirroring how `piet_budget`/`piet_maxperf`
   recommend wiring) -- keep it a distinct step rather than folding into
   `manifest.json`, since its unit of comparison (two argv executions) is
   a different shape than a golden-coeftable case.
4. If a future engine change adds a new non-deterministic JSON field
   (under a name other than `time_ms`/`timing_ms`), extend the masking
   regexes here rather than letting the gate go permanently RED.
