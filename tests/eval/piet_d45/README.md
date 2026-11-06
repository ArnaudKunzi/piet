# Phase-D4+D5 golden set (BETA campaign, Phase D)

Owner: Eval Harness Gen (CONSTITUTION Art 10 — Eval owns `tests/eval/**`).
Status: **ADDITIVE, standalone leg — NOT wired into `run_eval.py`'s default
gating set** (the maintainer ratifies at merge; same recipe as
`tests/eval/piet_d1/README.md`, which documents the shared architecture:
isolated per-case `--do` invocations, halt-at-first-error rationale,
display-precision tolerances).

## Current-status leg, not RED-first — why

Unlike piet_d1 (D1 RED-first) and piet_d2d3 (D3 RED-first), the D4/D5
wirings were **already landed** (op/piet @ff9d06a, `[BETA D4+D5]`
BS-077..081 + BS-021/082..087) when this leg was authored. The golden-first
RED proof for D4/D5 therefore lives in the wirings' own oracle-pinned
selftest legs on op/piet; **this leg is the independent Eval-owned
regression net** — same oracle, independently derived pins, independently
written graders, run against the shipped binary rather than the builder's
own test rig. `CURRENT-STATUS.txt` records the original set's first full
run: **21/21 GREEN** (exit 0). The three 2026-08-01 additions are RED-first
regression pins and raise the current target to 24/24.

## What this gates

| Case id | Verb | Kind | Result @ff9d06a |
|---|---|---|---|
| `d4a_newey_lag2` | `newey ..., lag(2)` HAC | golden | GREEN |
| `d4b_prais_twostep` | `prais ..., twostep` (n=336) | golden | GREEN |
| `d4a2_newey_lag2_after_xtset_sort` | `xtset`; `_n`; `newey ..., lag(2)` | professor-pangram golden | RED before xtset-sort fix |
| `d4b2_prais_twostep_after_xtset_sort` | `xtset`; `_n`; `prais ..., twostep` | professor-pangram golden | RED before xtset-sort fix |
| `d4b_prais_corc` | `prais ..., corc` (n=335 — CO drops obs 1) | golden | GREEN |
| `d4c_dfuller_bare` | `dfuller` (lags 0, no-drift cv row) | golden | GREEN |
| `d4c_dfuller_lags2_trend` | `dfuller, lags(2) trend` (trend cv row) | golden | GREEN |
| `d4d_pperron` | `pperron` (Zrho + Zt, NW lags 5) | golden | GREEN |
| `d4e_wntestq_bare` | `wntestq` (default 40 lags) | golden | GREEN |
| `d4e_wntestq_lags10` | `wntestq, lags(10)` | golden | GREEN |
| `d4f_tsfill_gapfill` | `tsfill` (329 → 336) | mechanical | GREEN |
| `d5a_sdtest_by` | `sdtest ..., by(high)` | golden | GREEN |
| `d5b_durbinalt_chi2` | `estat durbinalt` (chi2 form) | golden | GREEN |
| `d5b_durbinalt_small_lags2` | `estat durbinalt, small lags(2)` (F form) | golden | GREEN |
| `d5c_estat_firststage` | `estat firststage` (F per endog) | golden | GREEN |
| `d5c_estat_endogenous` | `estat endogenous` | golden | GREEN (flipped 2026-07-15, was `_reject`) |
| `d5c_estat_overid` | `estat overid` | golden | GREEN (flipped 2026-07-15, was `_reject`) |
| `d5d_margins_dydx_logit` | `margins, dydx()` after logit (real AME) | golden | GREEN |
| `d5d_margins_at_logit_reject` | `margins, at()` after logit | **reject** | GREEN (guard IS the pass) |
| `d5e_hausman_fe_re` | full estimates-store hausman idiom | golden | GREEN |
| `d5f_chow_by_high` | `chow ..., by()` vs manual-formula oracle | golden | GREEN |
| `d5g_mkspline_knots_mech` | `mkspline ..., cubic knots(...)` | mechanical | GREEN |
| `d5g_mkspline_nknots` | `mkspline ..., cubic nknots(4)` (check regression) | golden | GREEN (flipped 2026-07-15, was `_reject`) |
| `stata_success_resets_rc` | captured error then successful `reg3` / `test` / `lincom` | mechanical | RED before stale-`_rc` fix |

Oracle: `internal capture d45-oracle-stata.log`
(real StataBE 19.5; per-case preamble mirrors the oracle's: `sysuse
fatalities` + `gen high=(beertax>.5)` + `encode state, gen(snum)` + `gen
t=_n` + `tsset t`; d5e adds `xtset snum year`).

The three professor-pangram additions use the user-run StataNow log at
`pangram_stata_results.log` (internal capture).
Unlike the original standalone D4 cases, the pangram prepares its dataset
with `xtset snum year` before later generating `t=_n`; Stata's physical sort
is therefore part of the Newey/Prais oracle. The `_rc` case pins the same
run's successful-command return-code semantics.

## Files

- **`d45_expected.json`** — golden key, 24 cases. `_meta` documents the
  tolerance policy, the jget-style scalar-path syntax, and the schema
  provenance (every graded field name live-verified against @ff9d06a).
- **`run_d45_eval.py`** — driver (same CLI as the sibling legs). New over
  piet_d2d3: jget path resolver for nested/named-array scalars
  (`classic.F`, `firststage[beertax].F`, `coefs[beertax].diff`),
  single-element-list unwrap (durbinalt's per-lag arrays), optional
  `n_obs`, and a `mechanical` kind (display_sequence for tsfill's count
  pair; summarize_check for mkspline's basis vars).
- **`d45_cases.do`** — human-readable reference mirror (not executed by the
  driver).
- **`CURRENT-STATUS.txt`** — historical first full run of the original set
  @ff9d06a: 21/21 GREEN (predates the three professor-pangram additions).

## Reject-kind cases — the guard IS the pass condition

One case remains, PASSing by producing a documented honest reject
(coordinator directive):

1. **`margins, at()` after logit** — the engine's predictive-margins path
   re-fits plain OLS regardless of the model (silently wrong for logit);
   the wiring's non-OLS guard is exactly what the case pins. Oracle
   (unpinned): Pr(high) at income=13000 = .4074234 (se .029638). Note the
   CONTRAST with `margins, dydx()` after logit, which is genuinely wired
   (real AME path) and golden-pinned in `d5d_margins_dydx_logit`.

Three other cases lived here as tracked engine gaps until their wirings
landed, and have since FLIPPED to golden:

- **`mkspline nknots(4)`** — nknots() errored `need at least 3 knots`
  instead of computing percentile knot placement, until the Phase-F engine
  fix landed (op/cobra-beta-f @d2588a9, 2026-07-15). See
  `d5g_mkspline_nknots` below.
- **`estat endogenous`** (MI-216, B.0.1, 2026-07-15) — engine previously had
  no Durbin/Wu-Hausman test; the G-sweep wired it to cobra `iv-diag`. Now
  GREEN as golden `d5c_estat_endogenous`: live-verified against
  `op-piet/piet/native/build/piet_native` — durbin_chi2=34.49124374706312
  (oracle 34.4916), wu_hausman_F=37.979304701912504 (oracle 37.9797),
  wu_hausman_df_r=332, p-values ~0 matching the oracle's displayed 0.0000 —
  all well inside the leg's scalar_rtol=0.003.
- **`estat overid`** (MI-216, B.0.1, 2026-07-15) — engine previously had no
  Sargan/Basmann (its Hansen J is a different statistic, per the old reject
  message's own disclosure); same G-sweep wired it to cobra `iv-diag`. Now
  GREEN as golden `d5c_estat_overid`: sargan_chi2=6.809560061754693 (oracle
  6.80951, p=0.009067110110573061 vs oracle 0.0091), basmann_chi2=
  6.867677994921935 (oracle 6.86762, p=0.008776861935915755 vs oracle
  0.0088) — all inside scalar_rtol=0.003/scalar_atol=1e-4. See
  `MANIFEST-CORRECTIONS.md`-style provenance note in `d45_expected.json`'s
  `notes` field for each case.

## Other notable design points

- **`d4f_tsfill_gapfill` deviates from the oracle's construction on
  purpose**: the oracle dropped `mod(t,7)==0` obs, but `mod()` is
  unimplemented in PIET until Phase G (coordinator directive: do NOT use
  it). The case drops the contiguous block t∈[100,106] instead (329 obs →
  tsfill → 336), testing the same gap-fill semantics with arithmetic PIET
  supports today. Graded off the two `count` display blocks in order.
- **`d5f_chow` has a manual-formula oracle**: real Stata has no `chow`
  command; the oracle computed F=((rss_p−rss_0−rss_1)/k)/((rss_0+rss_1)/
  (n−2k))=28.074171 (df 2/332) from three explicit regressions. PIET's
  `chow ..., by()` wrapper reproduces it in `classic.F` to 10 significant
  digits (live-verified). The block's `robust` sub-object has no oracle and
  is not graded.
- **`d5g_mkspline_knots_mech` is mechanical-pass only**: the explicit
  knots() values (11000/13000/15000/17000) are Eval-chosen — the oracle
  only ran nknots(4) and its percentile knot placement was not recorded, so
  numeric pins for the explicit form are impossible without a new StataBE
  run (note-don't-guess). The case asserts the 3 basis vars materialize
  with N=336.
- **`d5g_mkspline_nknots` (golden, flipped 2026-07-15)**: nknots(4)'s
  percentile knot placement (5/35/65/95 pctiles = {10745.9, 12815.69,
  14541.8, 18077.87}, per `f-oracle-pins.md`) has no gradable path through
  this driver on its own (mkspline emits no `estimation`-type block, and
  `summarize_check` only compares N). The case appends a check regression
  (`regress high inc_sp1 inc_sp2 inc_sp3`) so the basis columns' correctness
  is graded through the ordinary golden coeftable mechanism instead —
  `required_scalars` pins the three `coeftable[inc_spN].b` values (b only;
  the oracle probe recorded no se's for this ancillary regression, so the
  stricter `coefs` b+se dict isn't used here). The knot values and the
  basis summarize table (mean/sd/min/max) are recorded as
  `unpinned_oracle_reference` for provenance, not directly gated.
- **prais pair as a semantic trap**: twostep keeps n=336 (PW transform
  retains obs 1) while corc pins n=335 EXACT (CO drops it) — plus distinct
  converged rho values (.8034461 vs .8038026) and both Durbin-Watson
  statistics (dw_0/dw) pinned on each.
- **dfuller pair proves the cv table switches**: bare (−3.453/−2.877/−2.570)
  vs trend (−3.987/−3.427/−3.130) critical-value rows, plus the `case`
  label pinned exactly. pperron's Z(rho) critical-value row is NOT emitted
  by the engine (only the Zt row) — recorded unpinned.
- **n_obs omissions are deliberate**: hausman's block reports n_obs=0 and
  estat firststage's has none (postestimation-summary quirks, live-verified)
  — those cases omit the n_obs check rather than pinning a meaningless 0.

## Wiring into the standing gate (the maintainer's job, at merge time)

Identical recipe to the sibling legs: fold this driver's exit code into
`run_eval.py`'s verdict as a separate subprocess step (the `--with-ctest`
pattern). Expected steady state after the fixes land: **24/24 GREEN / exit
0** — any RED
after a future op-piet change is a regression in a D4/D5 wiring (or a
schema change; Eval updates lookups, never pinned values — Art 10).

**Update 2026-07-15**: the anticipated flip described above already
happened. `d5g_mkspline_nknots_reject` (reject) is now `d5g_mkspline_nknots`
(golden) — Phase-F (op/cobra-beta-f @d2588a9) fixed nknots() percentile
knot placement; see the case's own `notes` in `d45_expected.json` and the
`d5g_mkspline_nknots` bullet above for the re-pin details.

**Update 2026-07-15 (MI-216, B.0.1)**: two more anticipated flips landed.
`d5c_estat_endogenous_reject` -> `d5c_estat_endogenous` and
`d5c_estat_overid_reject` -> `d5c_estat_overid`, both now golden — the
G-sweep wired `estat endogenous`/`estat overid` to cobra `iv-diag`. Reject-
kind count in this leg drops from 4 (original) to 1
(`d5d_margins_at_logit_reject` only). The original set remained **21/21
GREEN / exit 0**,
re-verified against `op-piet/piet/native/build/piet_native` — see
`CURRENT-STATUS.txt`'s matching UPDATE block for the fresh per-case table.
This leg remains standalone (not wired into `run_eval.py`'s default gating
set) — see the piet_d* fold decision recorded in
`<private-coordination-tree>/messenger/eval-harness/log.md` (or the the maintainer handoff) for
whether/when that changes.
