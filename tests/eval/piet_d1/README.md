# Phase-D1 golden set (BETA campaign, Phase D)

Owner: Eval Harness Gen (CONSTITUTION Art 10 — Eval owns `tests/eval/**`).
Status: **ADDITIVE, standalone leg — NOT wired into `run_eval.py`'s default
gating set.** See "Wiring into the standing gate" below for what the maintainer must do
to promote it once the D1 verbs land.

## What this gates

Six D1 verbs, golden-FIRST (authored before the wiring exists, against real
Stata truth, expected RED today, must turn GREEN on landing):

| Case id | Verb | Oracle section |
|---|---|---|
| `d1a_ivregress_2sls_justid` | `ivregress 2sls`, just-identified | D1a |
| `d1b_ivregress_2sls_overid` | `ivregress 2sls`, over-identified + control | D1b |
| `d1c_ivregress_2sls_robust` | `ivregress 2sls, vce(robust)` | D1c |
| `d1d_ivregress_2sls_cluster` | `ivregress 2sls, vce(cluster state)` | D1d |
| `d1e_ivregress_gmm` | `ivregress gmm` | D1e |
| `d1f_ivregress_liml` | `ivregress liml` | D1f |
| `d1g_reg3_3sls` | `reg3` (3SLS default, 2-equation system) | D1g |
| `d1h_reg3_sure` | `reg3, sure` | D1h |
| `d1i_reghdfe_cluster2way` | `reghdfe ..., absorb(state) vce(cluster state year)` | D1i |
| `d1x_ivreghdfe_absorb` | `ivreghdfe ..., absorb(snum) cluster(snum)` | professor pangram S08 (numeric golden) |
| `d1_boottest_engine_parity` | `boottest` | **no oracle** — engine-parity only (fixed-seed reproducibility + `reps` forwarded) |

Oracle: `internal capture d1-oracle-stata.log`
(real StataBE 19.5, `import delimited piet/packaging/sample/fatalities.csv,
clear`, 336 obs). piet's own `sysuse fatalities` loads the byte-identical
bundled CSV (verified: sha256 matches across the main/piet/eval worktrees).
The `d1x` numeric upgrade uses the user-run StataNow log at
`pangram_stata_results.log` (internal capture).

## Files

- **`d1_expected.json`** — the golden key. `_meta` documents the tolerance
  policy (derived from Stata's OWN display-rounding precision, not a
  cross-implementation float-precision assumption — see its `rationale`),
  the coeftable name-alias handling (`_cons` vs `(intercept)`), and the
  documented block-schema expectation for single- vs multi-equation
  (`reg3`) results. `cases` holds one entry per case: `do_body` (the exact
  command(s), oracle-derived b/se per coefficient, n_obs, a few scalars,
  and `oracle_kind` (`golden` / `engine_parity`).
- **`run_d1_eval.py`** — the driver. `python3 run_d1_eval.py [--piet-bin
  PATH] [--only case_id,...] [--json OUT]`. Resolves the binary from
  `--piet-bin` > `$PIET_BIN` > `<repo_root>/piet/native/build/piet_native`.
  Prints a per-case PASS/FAIL table; exits 0 iff every case is GREEN.
- **`d1_cases.do`** — human-readable reference script mirroring the same
  commands in oracle order, `cap noi`-wrapped. **Not** what the driver
  executes (see next section) — for manual spot-checking only, kept in
  sync with `d1_expected.json` by hand.
- **`RED-baseline.txt`** — a captured run against the current (pre-D1)
  op-piet build: `0/11 GREEN`, exit 1. The desired starting state.

## Why each case runs as an ISOLATED `piet_native --do` invocation

`piet_native --do FILE` halts the **entire** file at the first block whose
`type` is `"error"` (`piet/native/src/doexec.cpp`'s per-line loop). This is
**not** prevented by `cap noi` — verified empirically (2026-07-15) against
the current op-piet build: `cap noi ivregress ...` still sets
`stopped_at_reason` and halts, because dispatcher.cpp's capture-prefix arm
forwards the inner error block verbatim (so `noisily` can show it) and
doexec.cpp's halt-check doesn't distinguish a capture-wrapped error from a
top-level one.

Consequence: a single monolithic do-file containing all 11 D1 cases would
stop dead at whichever case is least-supported and never reach the rest —
and once the maintainer lands the D1 verbs in some partial order (e.g. `reg3` before
`ivregress`), a monolithic script would make an already-landed, GREEN-worthy
case falsely report FAIL merely because an earlier, still-unwired case
halted the script first.

`run_d1_eval.py` instead runs **`sysuse fatalities, clear` + exactly one
case's command(s)** per `piet_native` invocation, so every case's PASS/FAIL
is independent of every other case's wiring status. `sysuse fatalities`
resolves via the binary's own compiled-in dev-tree path
(`dispatcher.cpp`'s `devTreeSampleFatalitiesCsv()` bakes in `__FILE__` at
compile time) — not dependent on the invoking process's cwd — so no
cwd/data-dir plumbing was needed to make this portable across worktrees.

## Tolerances

See `d1_expected.json`'s `_meta.tolerance_policy` for the full derivation.
Short version: the oracle numbers are Stata's *displayed* coeftable/scalar
values (default format), not `e(b)`/`e(V)` full float64 precision, so each
pinned value's true precision is bounded by its own last displayed digit.
Surveyed worst case across all D1 cases: coefficient/SE relative precision
~6.3e-4 (reg3 SURE `emppop:income` se = `.0000799`, 3 sig figs); scalar
(chi2/F) relative precision ~1.6e-3 (D1a chi2=3.05, 2 decimals). The policy
adds a >1.5x margin: **coef/se rtol=1e-3, scalar rtol=3e-3** (both with a
1e-6 atol floor for near-zero values, none of which occur in this set).
`n_obs`, `n_clusters`, `df1`/`df2` are EXACT (integer, no tolerance).

## Known open schema questions (documented, not guessed)

1. **Constant-term spelling.** PIET's existing `feols` coeftable rows use
   `(intercept)` (verified live in the frozen piet2 corpus) while Stata
   displays `_cons`. The driver tries both before failing a lookup.
2. **reg3 multi-equation shape.** `piet/native/src/estimation_m4.cpp`
   (~lines 140-165, comment tagged BS-065) already documents that cobra's
   `reg3 --tidy` payload carries per-equation results under `equations`
   (`{depvar, coefs, coeftable}`) and that the M4 converter already reuses
   the `outcomes` array shape for it — the missing piece is purely the
   dispatch arm that recognizes Stata's `reg3 (...) (...)` syntax and calls
   that converter. The driver looks for `block['outcomes']` first (matching
   this documented plan), then a flat `equations` key, then a
   colon-qualified flat coeftable (`depvar:varname`) as a last-resort
   fallback, matched by each equation's `depvar` field.
3. **reg3 per-equation chi2/rmse.** The same estimation_m4.cpp comment notes
   the current `outcomes` remap does NOT carry per-equation chi2 at all
   (an "honest engine-data gap" — cobra's reg3 --tidy never computed it
   per-equation). `d1g`/`d1h`'s per-equation `chi2`/`rmse` checks are
   therefore SECONDARY (logged, but a missing field does not fail the case
   by itself) — only a missing/mismatched `coefs` or `r2` fails the case.
4. **reghdfe 2-way `n_clusters` representation.** Stata's own display shows
   two counts (48 state-clusters, 7 year-clusters) but the SE-adjustment
   line uses 7 (`min(G_state, G_year)`, the 2-way dof-correction
   convention). `d1i` pins `n_clusters==7`; if the eventual JSON instead
   exposes a per-dimension list/dict, the check also accepts 7 appearing
   as a member of that list/dict — genuinely resolving the exact shape is
   for the maintainer or the wiring engineer to decide, not something this golden set
   should guess further about.
5. **`ivreghdfe` numeric oracle resolved (2026-08-01).** The original D1
   probe only checked package availability, so this case began as a
   structural guard. The professor-pangram StataNow run now supplies the
   missing coefficient and clustered SE: beertax b=-22647.04913100959,
   se=10139.1830291201. The case is therefore a normal numeric golden and
   rejects the pre-fix PIET SE=10003.01652769641.

## Historical post-D1 gate status (2026-07-15): 7/11 GREEN

Re-run after the D1 wiring landed on op/piet (operator gate feedback loop):

- **GREEN (7):** `d1e` (gmm), `d1f` (liml), `d1g`/`d1h` (reg3 3sls/sure),
  `d1i` (reghdfe 2-way cluster), the then-structural `d1x`, and
  `d1_boottest_engine_parity` — the boottest case after the FIXTURE FIX
  below.
- **RED (4, tracked — do not relax):** `d1a`–`d1d`. Coefficient cells PASS;
  only the SE cells miss, and the miss is a **characterized engine gap**,
  not a mystery: cobra's iv estimator uses an N-K small-sample denominator
  where Stata's `ivregress` defaults to asymptotic N. Verified arithmetically
  from the driver's own got/want output: `d1c` beertax
  4274.766/4255.64 = 1.0044945 = sqrt(336/333) exactly (K=3), `d1a`
  13684.467/13643.68 = 1.002990 = sqrt(336/334) (K=2). Engine fix queued
  for **Phase F (cobra MINOR)**. Per Art 10 the tolerances and pinned values
  stay exactly as authored — they are the Stata truth, and these cases MUST
  stay RED until the engine matches. Each affected case carries
  `known_engine_gap: "iv-se-denominator (N-K vs N), fix queued Phase F"` in
  `d1_expected.json`, and the driver prints a `[KNOWN ENGINE GAP — tracked,
  not mysterious]` banner on their FAIL entries.
- Secondary note: the wired ivregress block does not (yet) carry `chi2`/
  `rmse` scalars — logged as schema notes by the driver, non-fatal per the
  documented secondary-check policy.

### Boottest fixture fix (was a fixture bug, not a wiring bug)

As originally authored, the boottest case's prerequisite fit was a plain-iid
`regress income beertax spirits` — but boottest is a **wild CLUSTER
bootstrap**, and the landed wiring honest-rejects after an unclustered fit
("boottest requires a preceding regression with vce(cluster ...)"), keeping
the case RED on the post-D1 build for a fixture reason. Fixed (operator gate
feedback, 2026-07-15): the prerequisite is now `regress income beertax
spirits, vce(cluster state)`; both parity runs execute it before
`boottest beertax, reps(499) seed(20260715)`, and the grader now also
requires `p_symmetric`/`p_equaltail` to be PRESENT in both runs
(`required_fields` — guards against a vacuous parity pass) before comparing
them bit-for-bit. Verified GREEN against the post-D1 build (block estimator
`wild_cluster_bootstrap`, reps forwarded = 499).

## RED-baseline summary (captured 2026-07-15, op-piet @812201c, piet 2.5.0 — PRE-D1 build)

**0/11 GREEN**, exit 1 — full transcript in `RED-baseline.txt`. Failure
signatures, one per case:

- `d1a`..`d1f` (all `ivregress` variants): `unsupported_command`, token
  `ivregress` — the Stata-syntax word isn't recognized at all yet.
- `d1g`, `d1h` (`reg3`): `unsupported_command`, token `reg3` — same;
  notably the underlying JSON-conversion plumbing for a future `reg3`
  result already exists (see schema question #2 above) — only the dispatch
  arm is missing.
- `d1i` (`reghdfe` 2-way cluster): a genuinely DIFFERENT failure signature —
  `[missing_column] column not found: state year` — confirming `reghdfe`
  and `absorb()` already dispatch and run today (unlike the others above),
  and the gap is specifically that `vce(cluster state year)`'s two tokens
  get passed to cobra as one literal (invalid) column name "state year"
  instead of being parsed as two separate cluster variables. This matches
  `piet/native/src/handlers.cpp`'s own comment: *"twoway clustering is OUT
  of the M2 slice."*
- `d1x` (`ivreghdfe`): `unsupported_command`, token `ivreghdfe`. This
  historical transcript predates both the wiring and the 2026-08-01
  numeric-oracle upgrade; the case is now a `golden`, not structural.
- `d1_boottest_engine_parity`: both runs fail identically
  (`unsupported_command`, token `boottest`) — the driver correctly does
  NOT count two identical failures as "parity" (there is no p-value yet to
  compare); recorded RED with an explicit note. (Baseline transcript
  predates the fixture fix above — at baseline time the command was
  unsupported outright, so the unclustered prerequisite was unreachable
  and the transcript is accurate for what it gated.)

## Wiring into the standing gate (the maintainer's job, at merge time)

This leg is intentionally **not** referenced anywhere in `run_eval.py` or
any manifest it discovers today — per Eval's mission brief, ratifying it
into the default gating set is the maintainer's call, to be made once the D1 wiring
has actually landed on `op/piet` and this driver reports GREEN end-to-end.
To wire it in:

1. Re-run `python3 tests/eval/piet_d1/run_d1_eval.py --piet-bin
   <new op-piet build>` and confirm `11/11 GREEN` (or work through any
   remaining RED with the owning operator — the per-case table + notes in
   `d1_expected.json` point at exactly which field/case is off). As of the
   post-D1 build this means: the four `d1a`–`d1d` REDs flip GREEN when the
   Phase-F cobra iv-se-denominator fix lands — nothing in this leg needs to
   change for that; the leg is already correct and simply waiting on the
   engine.
2. Add a step to `run_eval.py`'s `main()` (or a small wrapper it shells out
   to) that runs this driver and folds its exit code into the overall gate
   verdict — mirroring how `--with-ctest` is already folded in
   (`ctest_ok = cp.returncode == 0` pattern, `run_eval.py` ~L567-575).
   Keep it a separate subprocess call (different binary contract, JSON
   shape, and tolerance policy than the `cobra`-CLI-direct manifest cases)
   rather than trying to fold D1 cases into `manifest.json` — the block
   schema and grading logic here are piet_native-`--do`-specific, not
   cobra-CLI-`--format tidy`-specific.
3. If schema question #2/#3/#4 above resolved differently than documented
   here once the real wiring lands, Eval (not the requester) updates the
   lookup logic in `run_d1_eval.py` accordingly (Art 10 — only Eval adjusts
   its own judgment surface); the pinned oracle numbers in
   `d1_expected.json` themselves never need to change (they came from real
   Stata and are unaffected by piet's JSON shape).
