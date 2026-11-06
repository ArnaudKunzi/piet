# MAXPERF golden set — MSG-MI-237 items 1-3

Owner: Eval Harness Gen (CONSTITUTION Art 10 — Eval owns `tests/eval/**`).
Status: **ADDITIVE, standalone leg — NOT wired into `run_eval.py`'s default
gating set.** Same recipe as the `piet_d1`/`piet_d2d3`/`piet_d45` siblings:
The maintainer ratifies wiring it into the standing gate at merge time, once B.2.2
lands and this driver reports GREEN end-to-end.

## What this gates

Three gate-① blind spots found by the 2026-07-16/17 MAXPERF audit
(`internal note GATE23-analysis-do-audit-20260716.md`, findings F1/F2/F3),
requested in `<private-coordination-tree>/messenger/eval-harness/inbox.md` MSG-MI-237 items
1-3, ahead of the piet B.2.2 fix wave (MI-236). Item 4 (the standing "budget"
gate class, ②③) is a **separate** future stint — not in this leg.

| Case id | What it gates | Bug class |
|---|---|---|
| `mp1_merge_highprecision_key` | `merge m:1` false "not unique" on f64 keys that agree only up to 6 significant digits | silent **error** where none should fire |
| `mp2_collapse_factorby_precision` | `collapse (mean) ..., by(FACTOR)` silently truncates the computed statistic to 6 significant digits | silent **wrong number** |
| `mp3_foreach_continue_capture` | `foreach` silently stops after iteration 1's `capture`d error instead of continuing to iteration N | silent **skipped work** |

All three are **verified RED** against the pre-B.2.2 baseline (`piet
B.2.1` @ op-piet `6904f8e`) — see `RED-baseline.txt` for the full transcript
— and must turn GREEN once the corresponding B.2.2 fix lands.

## Files

- **`maxperf_expected.json`** — the golden key. `_meta.cases` holds one
  entry per case: `do_body` (the exact do-file lines, isolated per case),
  the bug's exact `file:line` citation, the oracle derivation
  (`first_principles` / `first_principles_dyadic` / `structural`), the
  tolerance policy where applicable, and a `red_baseline_observed` block
  recording exactly what the pre-fix engine returns (so a future reader
  never has to re-derive "what was broken" from scratch).
- **`run_maxperf_eval.py`** — the driver. `python3 run_maxperf_eval.py
  [--binary PATH | --piet-bin PATH] [--only case_id,...] [--json OUT]`.
  `--binary` is the primary flag (MSG-MI-237's own wording — this is the
  exact invocation the maintainer uses to gate a fixed build); `--piet-bin` is
  accepted as an alias matching the piet_d1/d2d3/d45 sibling convention.
  Resolves the binary from `--binary`/`--piet-bin` > `$PIET_BIN` >
  `<repo_root>/piet/native/build/piet_native`. Prints a per-case
  PASS/FAIL table + a `N/3 MAXPERF (MI-237) cases GREEN` summary line;
  exits 0 iff every case is GREEN, 1 if any is RED, 2 on a harness error
  (bad binary path, bad JSON).
- **`maxperf_cases.do`** — human-readable reference mirroring the same
  commands the driver runs, concatenated in item order. **Not** what the
  driver executes (see "isolation" below) — manual spot-checking only.
- **`RED-baseline.txt`** — captured run against the pre-B.2.2 baseline
  (`piet_native_baseline_B21`, copied out of `op-piet` on 2026-07-17
  before that worktree started being edited under the fix wave): `0/3
  GREEN`, exit 1. Also records a grader self-test (hand-constructed
  correct-shape JSON fed straight to each `grade_*()` function) confirming
  the graders themselves flip GREEN on genuinely-fixed data and are not
  just permanently-RED by construction.

## Why each case is an ISOLATED `piet_native --do` invocation

Same rationale as `tests/eval/piet_d1/README.md`: `piet_native --do FILE`
halts the whole file at the first block whose `type` is `"error"` and is
NOT `captured`. A monolithic 3-case script would make an already-fixed case
falsely report FAIL merely because a still-unfixed case's OWN do-file
halted first — most concretely, `mp1`'s current false merge error would
prevent `mp2`/`mp3` from ever running at all if they shared one script.
Each case therefore gets its own `--do` invocation with its own fully
self-contained synthetic fixture (no `sysuse`/corpus dependency).

## Case notes

### mp1 — merge m:1 high-precision-key

Bug: `findDupKey` (`piet/native/src/handlers_data_verbs.cpp:2010`) builds
its duplicate-key check via `keyOss << "n:" << v` — a bare `ostringstream`
defaults to 6 significant digits. Two distinct f64 keys that agree on their
first 6 significant digits (e.g. `1005938` / `1005942`, or `3141.59261` /
`3141.59264`) collide in the dup-check's `seen` map even though pandas/
`duplicates report` on the same data confirm the key is genuinely unique.
`joinbyRowKeys` in the SAME file already does this correctly via
`eval::pythonFloatRepr` — this is a missed-class fix, not a hard problem.

Fixture: a 4-row `using` side with two independent colliding-pairs (an
integer-id class and a fractional class), each genuinely unique; an 8-row
`master` side referencing each of the 4 keys exactly twice (`m:1` shape).
Grading requires not just "no error" but that every one of the 8 master
rows carries the CORRECT matched `payout` (verified via a trailing `list
id payout, clean noobs` — a `"type":"browse"` block whose `rows` carry full
double-precision values, unlike `data_mutation` cards which only carry
varlist metadata). `merge_counts.matched==8` (from the mutation block's
own `master_only=/using_only=/matched=` regex capture off cobra's stderr)
is checked as a fast top-level cross-check alongside the row-level one.

The `save`/`using` path is a `__USING_HBIN__` placeholder substituted by
the driver with a fresh tempdir path at run time — the JSON stays
environment-independent. (`.dta` writing is not supported by this PIET
build — `save X.hbin` / `use X.hbin` is the native round-trip; discovered
live while authoring this case, not a surprise worth re-deriving.)

### mp2 — collapse factor-by precision

Bug: whenever ANY `by()` variable is a factor (string) column, `collapse`
rewrites the WHOLE dataset through a CSV round-trip twice —
pre-convert (`handlers_data_verbs.cpp:771`) and post-restore (`:1001`) —
both via bare `csv << v` (6 significant digits). Line 1001 is what actually
corrupts the answer: it's writing the COMPUTED STATISTIC (the collapsed
mean itself), not just the by-var's proxy code. A numeric-only `by()` never
touches this path at all (cobra's native fast path handles it), which is
exactly why the pre-existing collapse goldens missed this class.

Fixture: 2 groups (string `by(grp)`, forcing the CSV-shim path), 2 obs each,
every value a **dyadic rational** `k + m/1024` — exactly representable in
float64 with no rounding error, so the true group mean is derivable by
exact arithmetic (verified: `mean*2048` is an exact integer for both
groups) and needs no Stata run at all. Tolerance **rtol=1e-9, atol=1e-9**
(ratified per MSG-MI-237's own suggestion): the bug's signature is a
~1.6-2.0e-6 relative miss (empirically confirmed on the baseline — see
`red_baseline_observed` in the JSON), so 1e-9 sits >1000x below the smallest
observed miss while staying >1000x tighter than the bug, comfortable margin
either direction.

### mp3 — foreach-continue after a captured error

Bug: `loops.cpp`'s `runBody()` (used internally by `runForeach`/`runWhile`
for each iteration) checks `b.value("type","")=="error"` with **no**
exemption for `captured:true` blocks — unlike `doexec.cpp`'s own top-level
halt-check (~line 508/537, tagged `BS-127`), which correctly does NOT halt
on a captured error ("Stata shows the message, sets `_rc`, and CONTINUES").
Net effect: the first `capture`d, `noisily`-shown error inside ANY
foreach/while iteration silently breaks the LOOP's iteration advancement
after that iteration — even though the identical error at top level would
not have halted anything. Because `doexec.cpp`'s OWN check is correctly
captured-aware, the surrounding do-file does NOT halt either — everything
after the loop keeps running, which is precisely what makes the skip easy
to miss (no visible halt, just quietly-fewer iterations).

Fixture: the maintainer's own pre-verified repro shape (MSG-MI-237), re-verified here
byte-for-byte. **Provenance note, so this is never mysterious later:** the
do-file never loads a dataset, so the literal `error 111` line never
actually dispatches through a real `error N` command — that command isn't
implemented in this PIET build at all (confirmed: WITH a dataset loaded,
the same line instead falls through to `unsupported_command`, rc=199).
Without a dataset, PIET's own `session.currentHbin()` pre-dispatch guard
fires first and returns `no_data_loaded`, whose rc is a **stable,
documented** 111 per `piet/native/include/piet/eval/errors.h`'s `coarseRc`
map (`no_data_loaded -> "111"`) — not a coincidence tied to the digits
"111" in the do-file text. The bug under test (loop-breaks-on-captured-
error) is completely independent of which error class fires; this is
simply the minimal, already-maintainer-verified, dataset-free vehicle for it.
Per MSG-MI-237's own caution, the inner program's display text
`"inside-failprog"` does **not** begin with the word `in ` (it begins with
the single token `inside-failprog`) — re-confirmed live not to trip the
unrelated `in`-range parser bug.

Grading is an exact, order-sensitive match on the full `type=="display"`
text sequence (10 entries once fixed: 3× `ITER-*-START` / `inside-failprog`
/ `ITER-*-RC=111` + the post-loop marker) plus a count of `captured:true`
error blocks (1 today, must become 3). The baseline today produces only the
first 4 entries — iterations b/c never run — while the post-loop marker
still prints, matching the maintainer's own description exactly: "baseline runs ONLY
iter a (rc=111 IS correctly set), b/c skipped, marker prints."

## Wiring into the standing gate (the maintainer's job, at merge time)

Identical recipe to the sibling legs:

1. Re-run `python3 tests/eval/piet_maxperf/run_maxperf_eval.py --binary
   <new op-piet build>` and confirm `3/3 GREEN`. The per-case `checks`
   list (or `--json` dump) points at exactly which field is still off if
   not.
2. Fold this driver's exit code into `run_eval.py`'s verdict as a separate
   subprocess step (the `--with-ctest` pattern, `run_eval.py` ~L567-575) —
   different binary contract (`piet_native --do`, not `cobra --format
   tidy`) than `manifest.json`'s cases, so keep it a separate step rather
   than folding into the manifest.
3. If B.2.2 lands a JSON shape different from what's assumed here (e.g.
   `merge_counts` moves, or the `browse` block's `rows` shape changes),
   Eval (not the requester) updates the lookup logic in
   `run_maxperf_eval.py` accordingly (Art 10) — the pinned oracle values
   themselves (the dyadic means, the id/payout pairs, the display sequence)
   never need to change; they came from first-principles arithmetic and a
   live-verified repro, not from the engine's own (buggy) output.
