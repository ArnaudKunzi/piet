# serve_session -- the RESIDENT FRAME session-mode parity leg (gate (1)b for MAXPERF Stage 1b/2)

Owner: Eval Harness (CONSTITUTION Art 10 -- Eval owns `tests/eval/**`).

Sibling of `../serve_parity` (contract_A section 25, `cobra serve` dual-run
parity, shipped 0.28.0). This leg targets contract_A **section 26**
(RESIDENT FRAME session ops: `use`/`run`/`snapshot`/`drop`, RATIFIED
2026-07-18 in cobra 0.29.0) and, as of MAXPERF Stage 2, **section 27**
(session snapshots: `stash`/`unstash`/`save_virtual`, an additive fast-path
on `use` -- PROPOSED, pending ratification at merge). Golden-first law, same
as serve_parity: the section-27 scenarios (s13-s16) landed BEFORE section 27
exists on the reachable release binary.

## What this gates

Every release stamp touching the serve session path should show
`serve-session: N/N identical (0 skipped)` for both this leg and
`serve_parity`, once every scenario's capability precondition is met on the
binary under test. A RED here means the resident-frame result (byte-level
stdout/stderr per verb, plus full value-level equality of the final data --
or, for s13-s16, of an intermediate stash/virtual-restore point) diverges
from the equivalent one-shot `--data in --out out` chain -- a correctness
bug in the engine, not a perf tradeoff.

## Day-1 SKIP (confirmed live)

**Section 26** (s01-s16, gates the whole leg): probes with one real
`frame-use` request against a tiny throwaway fixture before running any
scenario. Confirmed live (at the time section 26 itself was authored)
against both `<private-coordination-tree>/releases/engine/current/cobra` (0.28.0) and the
in-progress `op-cobra` worktree build (also 0.28.0 at probe time): neither
recognized a `"frame"` request key -- the `serve` loop's generic parser
answered `{"id":null,"exit":2,"stdout":"","stderr":"serve: malformed
request line (need {id:<int>, argv:[<string>,...]})"}`, and the session
stayed alive afterward. `probe_frame_support()` treats anything other than
`exit==0` with a `"frame"` object containing `"rows"` as section-26-absent:
every scenario prints `SKIP`, overall exit 0 at the time (`--require-frame`
correctly flipped this to exit 1 back then -- FIX-SERVE-REQUIRE has since
inverted the default, so section-26 absence is now a hard RED unless the
opt-in `--allow-no-frame` is passed).

**Section 27** (s13-s16 only, does not gate s01-s12): a SEPARATE probe,
`probe_stash_support()`, runs only after section 26 itself is confirmed
available -- it sends one real `stash` request (against the section-26
probe's own resident) and requires `exit==0` with a `"frame"` object
containing `"name"`. Confirmed live against the real 0.29.0 release binary
(which HAS section 26 but not section 27): `stash`/`unstash`/`save_virtual`
each answer via the exact SAME generic-unknown-op path section 26 itself
uses for a bad op string -- `{"exit":2,"stderr":"serve: unknown frame op
'stash'"}` (substituting the actual op name) -- and the session stays fully
usable afterward. `probe_stash_support()` treats anything else as
section-27-absent: only s13-s16 print `SKIP` (s01-s12 run and compare
normally); `--require-stash` flipped overall exit to 1 in that case at the
time (FIX-SERVE-REQUIRE has since inverted the default: section-27 absence
is now a hard RED unless the opt-in `--allow-no-stash` is passed). Confirmed
live end-to-end: `12/12 identical (4 skipped)` against the 0.29.0 release
binary (`--only s01,...,s12` alone gives the literal `12/12 identical (0
skipped)`), and `16/16 identical (0 skipped)` against the op-cobra
fix/cow-stage2 build that implements section 27.

## Design: one persistent session, 16 independent scenarios

Unlike serve_parity (one shared manifest corpus replayed through one
session), this leg's unit is a short **verb chain** run twice: once as a
`use` + N `run` + `snapshot` sequence against ONE persistent `cobra serve`
process (shared across all 16 scenarios -- each scenario's own `use` cleanly
resets resident state, so this is still a genuine multi-reset stress test
of the session, not 16 independent processes), once as N one-shot
`cobra <verb> --data <head> [--out <next>]` invocations chaining `head`
forward by hand (advancing only when the one-shot call's own exit code is
0). Fixtures are tiny (n<=60 rows, one string factor column, built at
runtime via `write_hbin()`, which reuses `tools/to_hbin.py`'s own
`encode_column`/`align_up`/`MAGIC` rather than reimplementing the columnar
layout -- never committed).

Per verb: `compare_verb()` byte-compares stdout/stderr after masking (see
below). Per mutator verb: `check_run_frame_coherence()` asserts the
response's `frame.rows`/`frame.cols` match the reference chain-head's actual
shape (read back via `tools/hbin_read.py`, reused verbatim). End of every
scenario: `snapshot` + `compare_hbin_values()` -- column set, per-column
dtype, factor level dictionaries, and every value, row-for-row in STORED
order (deliberately not re-sorted -- s04 exists specifically to test that
physical row order propagates correctly through the resident frame).

**s13-s16 (section 27) apply a variant of the same axis mid-scenario, not
just at the end.** `snapshot_and_compare()` (a no-drop sibling of the
end-of-scenario `snapshot_compare_and_drop()`) snapshots whatever is
CURRENTLY resident at each stash/unstash/save_virtual restore point and
value-compares it against the one-shot reference state AS OF THAT EARLIER
POINT in the chain -- the whole point of a snapshot op is that later
mutation (of the resident, or of a DIFFERENT stash) must not leak into an
earlier capture, so a single end-of-scenario compare would not exercise
that. `check_stash_like_frame()` covers the two response shapes that are new
to section 27: `stash`/`save_virtual` return `{"rows","cols","name"|"path"}`
(distinct from `use`'s body), while `unstash` and a virtual-hit `use`
re-establish the full section-26 `use` body (`check_use_frame_meta()`,
reused verbatim -- confirmed live the two are byte-identical in shape).

## Masking -- read this before treating a RED as real

Two things are masked out of every stdout/stderr byte-compare:

1. `time_ms` / `timing_ms` -- serve_parity's own finding (real wall-clock,
   non-deterministic by construction). Reused verbatim via
   `SP.mask_timing` (imported from `../serve_parity`), not reimplemented.
2. **`"provenance":{...}` (the whole object, however deep) -- new to this
   leg, confirmed load-bearing.** Live evidence (`ols`/`logit`/estimator
   JSON): `provenance` carries `argv` (echoing whatever argv the process
   actually received) and `data.{path,fnv1a64}`. Session-mode `run` argv
   deliberately never carries `--data`/`--out` (that omission is the entire
   point of section 26), while the one-shot reference always does -- so
   `provenance.argv` (and usually `data.path`) WILL legitimately differ
   between arms on every single estimator verb, even when section 26 is
   100% correct. This is an Art-10 tolerance call, same status as
   serve_parity's timing mask. Because `provenance` nests one level deep
   (a `"data":{...}` sub-object), a non-nested regex like serve_parity's
   `timing_ms` one is unsafe here (it would stop at the FIRST inner `}`);
   `mask_provenance()` does a real quote/escape-aware brace-depth scan
   instead. Confirmed live: mutators (`eval`/`gsort`/`recode`/`merge`/
   `collapse`) never emit a provenance block at all -- only estimators do;
   the mask is a no-op where absent.

Also, any reference-arm stderr line containing the word "wrote" is dropped
before comparing (mutators print a write confirmation since one-shot mode
takes `--out`; the session arm never touches disk for a `run`, so it has no
such line -- an expected asymmetry, not a bug). The suffix varies by verb --
confirmed live: bare `wrote <path>` (`eval`), `(N rows)` (`gsort`),
`(new var X)` / `(replaced X)` (`recode`), `(N groups)` (`collapse`),
`(N rows; master_only=.. using_only=.. matched=..)` (`merge`) -- so the
strip is a generic per-line substring match, not one hardcoded pattern.

## Scenarios (16)

`s01` gen chain (3x `--gen`) * `s02` replace (`--gen` overwrite) + `recode`
* `s03` egen mean + rank by(g) * `s04` gsort then `egen seq()` by(g)
(order-dependence: `seq()` numbers in CURRENT PHYSICAL order, no internal
sort clause of its own, unlike `--bygen`) * `s05` drop var + keepif row
filter * `s06` merge 1:1 with a `--using` file * `s07` collapse mean by a
**numeric** group key * `s08` gen mutation -> `ols` -> `eval --summarize`
* `s09` keepif -> `logit` (pure-random binary y, avoids perfect separation)
* `s10` error mid-chain, state must not advance * `s11` `--data`/`--out` in
a frame-run argv must reject (exit 2) without disturbing state, then a
normal verb must still work * `s12` a stateless section-25 request against
a DIFFERENT file interleaved mid-session must not affect resident state
(also opportunistically covers `run`/`snapshot` after `drop` -> exit 3, the
only scenario with a live resident frame it can safely tear down for this).

**Section 27 (s13-s16, need contract_A section 27 -- SKIP individually if
absent, see "Day-1 SKIP" above):**

* `s13` stash -> mutate -> unstash (default, `keep` omitted) round-trip: the
  restored resident must value-match the one-shot chain AT THE STASH POINT,
  unaffected by the intervening mutation; also confirms the documented
  default DROPS the stash (a second `unstash` of the same name then fails
  exit 3).
* `s14` save_virtual -> further mutate -> use(virtual path) round-trip: the
  restored resident must value-match the one-shot chain's on-disk state AT
  THE SAVE POINT; confirms the virtual path is never actually written to
  disk (checked with `Path.exists()`); confirms `use` of a virtual path does
  NOT consume the entry (unlike `unstash`'s default) by using it TWICE with
  a different intervening mutation each time and getting the identical
  save-point state back both times.
* `s15` stash with `keep:true` -> unstash twice: the first `unstash(keep=
  true)` retains the stash (Stata `restore, preserve`) so a second `unstash`
  of the same name still succeeds and restores the SAME stash-point state
  even though the resident was mutated differently in between; a third
  `unstash` (default, no `keep`) then fails exit 3 -- the second call
  consumed it.
* `s16` section-27 error paths, each followed by a recovery verb proving
  state was left untouched: unknown stash name -> exit 3; `unstash` with no
  resident AND an empty stash table at all (right after a `drop`, before any
  subsequent `use`) -> exit 3, plus `stash`/`save_virtual` with no resident
  -> exit 3 each; a `frame` object missing the required `"name"`/`"path"`
  key on stash/unstash/save_virtual -> exit 2 (sent via the raw
  `_send_frame()` transport directly, since the public `frame_stash`/
  `frame_unstash`/`frame_save_virtual` wrappers always supply the key).

## Spec/engine inconsistencies found while implementing (empirical, against 0.28.0's shared machinery -- section 26 itself does not exist yet to check against directly)

- **`collapse --by` rejects a factor column** (`"column 'g' is a factor, not
  f64"`, confirmed live) despite this leg's own `s07` name ("collapse by
  factor") -- `--by` must be a plain numeric group code. Used a separate
  numeric `grp` column instead of the string factor `g`.
- **`eval`'s own hand-rolled mini-language silently ignores unrecognized
  flags** -- a documented, deliberate exception (ctest gate BUG-060/E6:
  "cmd_eval is a documented exception ... deliberately NOT exercised
  here"), confirmed live (`--totally-unknown-flag` on `eval` returns exit
  0). The commission's `s10` wording ("unknown flag on verb 2") would not
  reproduce a real error on an `eval` verb; substituted an undefined-
  variable reference (`--gen "bad = zzz_not_a_real_var * 2"`, a clean
  exit-1 "unknown variable" error) to preserve the scenario's actual intent
  (state must not advance past a verb-level failure) with a construct that
  is a genuine error on this engine.
- `--gen` on an EXISTING variable name overwrites it in place (confirmed
  live, `n_cols` unchanged) -- this is how `s02`'s "replace" half is
  implemented; there is no separate `--replace` flag.
- The one-shot mutator write-confirmation ("wrote ...") lands on
  **stderr**, with a per-verb-family suffix (see "Masking" above) -- not a
  single uniform message.

## Usage

```
python3 run_serve_session.py --cobra ENGINE_BIN
python3 run_serve_session.py --cobra ENGINE_BIN --only s04,s10   # subset
python3 run_serve_session.py --cobra ENGINE_BIN --only s13,s14,s15,s16  # section-27 only
python3 run_serve_session.py --cobra ENGINE_BIN --json /tmp/out.json
python3 run_serve_session.py --cobra ENGINE_BIN  # CI-style "section 26/27 must exist" is now the default
python3 run_serve_session.py --cobra ENGINE_BIN --allow-no-frame  # opt-in: tolerate section-26 absence (golden-first landing window)
python3 run_serve_session.py --cobra ENGINE_BIN --allow-no-stash  # opt-in: tolerate section-27 absence (with section 26 present)
```

`--scratch DIR` keeps fixtures/snapshots for inspection instead of an
auto-cleaned temp dir (mirrors `piet_budget`'s convention).

## Output

Per-scenario `PASS`/`RED`/`SKIP` line (`RED` includes every failing
assertion, byte-diffs as a bounded hexdump excerpt via serve_parity's own
`first_diff_excerpt`, reused not reimplemented). Summary:
`serve-session: X/Y identical (Z skipped)`. Exit 0 unless any scenario is
RED, or section 26 was globally unavailable and `--allow-no-frame` was NOT
passed, or section 27 was unavailable (with section 26 present) and
`--allow-no-stash` was NOT passed (these are now the hard-RED defaults --
`--allow-no-frame`/`--allow-no-stash` opt back into the old graceful-SKIP
behavior). The `--json` dump also carries
`stash_available`/`stash_reason`, parallel to `frame_available`/
`frame_reason`.

## Validation this stint (no section-26 binary exists yet to test against directly)

- **Day-1 SKIP**: confirmed live against real 0.28.0 (see above),
  `12/12 (0 skipped -> all 12 skipped)`, exit 0 at the time; `--require-frame`
  correctly flipped to exit 1 back then (FIX-SERVE-REQUIRE has since inverted
  the default -- this exit-0 result is now what `--allow-no-frame` opts into).
- **Happy path + RED-detection**: validated with a throwaway local double
  (delegates every actual computation to the real one-shot 0.28.0 binary;
  only frame bookkeeping -- resident-path tracking via temp files, the
  `--data`/`--out`-in-run rejection, the post-drop exit-3 behavior -- is
  invented). **Not part of this deliverable** (lived in a scratch dir, not
  committed) -- exists only to prove this harness's own comparison logic
  before section 26 exists to test against. Full run: `12/12 identical (0
  skipped)`. A deliberate 1-byte stdout corruption was caught with the
  expected hexdump excerpt (exit 1). Three further targeted-bug runs each
  caught cleanly: the double reporting a wrong rejection code (5 instead of
  2) on an illegal `--data`-in-run request (`s11`), the double reporting a
  wrongly successful verb2 (`s10`'s "verb2 unexpectedly SUCCEEDED" check),
  and (initially, before a double-fidelity fix -- see next point) the
  double's own "wrote" leakage.
- **A double-fidelity finding, not a harness bug**: the first double
  iteration mutated via real `--out` files under the hood (to fake RAM
  residency) and its delegated calls therefore printed "wrote" to stderr --
  which a genuine in-RAM implementation never would (only `snapshot` should
  touch disk). Fixed in the double (strip "wrote" lines from its own `run`
  responses) rather than loosening the harness's asymmetric strip-only-the-
  reference design, which is correct for what section 26 is actually
  specified to do.

(Section 26 has since RATIFIED and shipped, cobra 0.29.0 / B.4.0; the above
predates that and is kept as the historical record of how this leg was
validated before a real section-26 binary existed.)

## Validation this stint (section 27 extension, 2026-07-18, MI-2xx)

Unlike the section-26 stint above, a real section-27 binary already existed
(the op-cobra `fix/cow-stage2` build, @8f27319) -- so s13-s16 were verified
against the REAL engine directly, no double needed:

- **Day-1 SKIP**: confirmed live against the real 0.29.0 release binary
  (`<private-coordination-tree>/releases/engine/current/cobra`, section 26 present, section
  27 absent) -- `12/12 identical (4 skipped)` for the full 16-scenario run
  (`--only s01,...,s12` alone gives the literal `12/12 identical (0
  skipped)`); `--require-stash` correctly flipped to exit 1 back then
  (FIX-SERVE-REQUIRE has since inverted the default -- this exit-0/SKIP
  result is now what `--allow-no-stash` opts into).
- **Happy path**: `16/16 identical (0 skipped)` against the op-cobra
  fix/cow-stage2 build, repeated across multiple runs with no flakiness
  (the four new scenarios share the ONE persistent session with s01-s12, so
  this also re-confirms no state leakage from the new ops into the older
  ones or vice versa).
- **RED-detection, fault-injected against the real engine** (temp copies in
  this directory, never committed): (1) flipping s16's expected
  unknown-stash exit code from 3 to 0 -- caught immediately (`expected
  exit=0, got exit=3 stderr="serve: no stash named
  'totally_unknown_key_999'"`); (2) pointing s13's post-unstash value-compare
  at `out2` (the post-mutation state) instead of `out1` (the true stash
  point) -- caught immediately via `compare_hbin_values`'s column-set check
  (`session=[...,'p1',...] reference=[...,'p1','p2',...]`), proving the
  round-trip check is a genuine value-level test and not vacuous (a
  restore that silently no-op'd, or a stash that captured the wrong point,
  would both be caught the same way).
- **A live engine finding, not a bug**: the initial `s16` draft referenced
  a variable (`e0`) that only exists on the scenario's OWN fixture chain;
  after `s16`'s own mid-scenario `drop` + reload of a completely fresh
  fixture (`fx2`, part of exercising "no resident"/"unstash before use"),
  the recovery verb referencing `e0` legitimately failed (`unknown variable
  e0`) against BOTH the session and a correctly-behaving reference -- a
  harness bug (recovery verb referencing a variable not guaranteed to exist
  on every possible head), not an engine defect. Fixed by having the
  recovery verb reference `x1` (present on every `make_fixture` frame,
  including a freshly reloaded one) instead.

## Wiring into the standing gate (the maintainer's job)

Same recipe as `serve_parity/README.md`'s own "Wiring" section. Section 26
is already wired (shipped 0.29.0/B.4.0, RATIFIED at merge); this covers
section 27 once it ratifies too:

1. Confirm the real `frame` response shapes (`stash`/`save_virtual`'s
   `{rows,cols,name|path}` body, `unstash`/virtual-`use`'s reuse of the
   section-26 `use` body) match what `probe_stash_support()` /
   `check_stash_like_frame()` / `check_use_frame_meta()` expect -- if the
   landed protocol differs, that's an Eval update (Art 10), not a request
   back to the engine side.
2. Run once against the ratified binary; must be `16/16 identical (0
   skipped)` before wiring as a release-checklist gate alongside
   `serve_parity`.
3. Add as a separate subprocess step in `run_eval.py` (the `--with-ctest`
   pattern), not folded into `manifest.json` -- same reasoning as
   `serve_parity`.
4. If section 26/27 lands with a new non-deterministic JSON field (a name
   other than `time_ms`/`timing_ms`/`provenance`), extend the masking here.

## Files

- `run_serve_session.py` -- the tool. Imports `../serve_parity/
  run_serve_parity.py` (for `ServeSession`/`ServeUnavailable`/
  `mask_timing`/`first_diff_excerpt` -- reused, never duplicated) and
  `tools/hbin_read.py` + `tools/to_hbin.py` (fixture read/write, reused
  verbatim).
