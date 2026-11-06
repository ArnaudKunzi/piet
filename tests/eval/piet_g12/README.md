# G12 exotica goldens — factor rows under value labels

Owner: Eval Harness (CONSTITUTION Art 10 — Eval owns `tests/eval/**`).
Status: **ADDITIVE, standalone leg — NOT wired into `run_eval.py`'s default
gating set.** Authored 2026-07-31 by `opus-routine-202607311304-97936`
for `FIX:BUG102-EVAL-G12-ADDCASE` (spawned by the `P1-W2-0-LABELBOOK-BUG102`
stint; `internal note P1-W2-SCOPING-20260731.md` §2.7 — "whichever W2 arc lands first
files its own add-case request, G12 is the accumulation of those").

This is the **first** cell of that accumulation, so it also creates the
catalog. It is Art-10 **oracle-first**: the Stata truth below was captured
before any judgement was formed about the implementation.

## What it gates

`piet_native`'s coefficient-table **row naming** when a numeric factor
variable carries a value label (BUG-102).

| case | fixture shape | Stata stub column |
|---|---|---|
| `g12a_labeled_full` | 7-level factor, every level labeled | `Y83 Y84 Y85 Y86 Y87 Y88` |
| `g12b_unlabeled_control` | same model, no value label | `1983 1984 1985 1986 1987 1988` |
| `g12c_labeled_partial` | only 1983/1985/1987 labeled | `Odd83 1984 Odd85 1986 Odd87 1988` |

`g12c` is the exotica case and the reason this leg is worth its bytes: Stata
falls back **per level**, so an all-or-nothing implementation looks correct on
`g12a`/`g12b` and is only caught here.

## The oracle

Real StataBE 19.5, committed at
`tests/eval/golden/g12_factor_valuelabel.{do,log}`. Fixture is
`piet/packaging/sample/fatalities.csv` (336 obs) — the same bundled sample
`piet_native`'s `sysuse fatalities` loads, so oracle and engine see
byte-identical data.

Two facts the oracle settles, both measured across all three arms:

1. **`e(b)` column names are `<level>.<varname>` and are INVARIANT under value
   labelling.** Arms A/B/C produce identical colname vectors modulo the
   variable's own name (`1983.year` / `1983.yearnl` / `1983.yearpl`).
2. **The display stub is the label text ALONE** (`Y83`), printed under a single
   `varname` header row — never `<label>.<varname>`. Unlabeled levels print the
   bare level number. Stata emits `Y83.year` **nowhere, in any layer**.

## Why there is no `manifest.json` cell

The dimension is unreachable from the cobra-facing `run_eval` manifest: value
labels do not survive into hbin (no label field in `src/io/columnar.h`), so a
cobra cell keyed on a labeled factor would compare two identical label-free
runs and read GREEN **vacuously** — exactly the silent-skip class
`tests/eval/AUTHORING.md` exists to prevent. The dimension is PIET-side only,
hence a standalone `piet_*` leg (`piet_d1` precedent).

## Asserted vs reported

- **ASSERTED** — row `name` equals Stata's `e(b)` colname, and b/se match the
  oracle within `rtol=1e-9`.
- **REPORTED ONLY** — *where* the display stub text is carried in the row
  object. PIET's coeftable JSON is a flat array with no header-row concept and
  currently has no field for a stub distinct from `name` (row keys are exactly
  `b/ci_hi/ci_lo/name/p/se/t`). Choosing that carriage is a **lane:piet design
  decision this golden deliberately does not prescribe**; the runner prints
  what it finds and never fails on it.

## Running it

```
/opt/homebrew/bin/python3 tests/eval/piet_g12/run_g12_eval.py --piet-bin <piet_native>
```

Exit 0 iff every asserted check passes. `--only ID,ID` filters cases,
`--json OUT` writes a machine-readable result.

## Measured status 2026-07-31 (authoring stint, two arms)

| arm | binary | result |
|---|---|---|
| pre-fix | `main`@`be6535f` `piet/native/build/piet_native` | **3/3 asserted-GREEN, exit 0** |
| post-fix | `fix/w2-labelbook-bug102`@`9fbf361` | **1/3 asserted-GREEN, exit 1** |

The post-fix arm emits `Y83.year` / `Odd83.yearpl` — a third shape present in
**neither** Stata layer — and in doing so **destroys the level code**, which no
other field in the row carries. `g12b` (unlabeled control) stays GREEN in both
arms, so the comparator fires on the labeled dimension only and is not
vacuously failing everything; in `g12c` the unlabeled levels (`1984.yearpl`)
keep the correct name while the labeled ones do not, confirming the substitution
is per-level.

**This RED indicts a landed change, not a missing one** — the inverse of the
`piet_d1` golden-first posture, where RED meant "wiring not written yet". The
pre-fix binary was already colname-correct; what it lacked was any carriage for
the label at all (the original BUG-102 complaint), which the reported layer
shows unchanged in both arms. Routed to lane:piet as
`FIX:BUG102-COEFTABLE-NAME-PARITY` (this leg is the acceptance gate for it).
BUGLOG-102's `CLOSED 2026-07-31` addendum needs the same correction; that file
is a lane:piet write surface, so it is named in the FIX row rather than edited
here (manifest-escape law, worker-v7 §3).
