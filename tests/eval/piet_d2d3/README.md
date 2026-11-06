# Phase-D2+D3 golden set (BETA campaign, Phase D)

Owner: Eval Harness Gen (CONSTITUTION Art 10 — Eval owns `tests/eval/**`).
Status: **ADDITIVE, standalone leg — NOT wired into `run_eval.py`'s default
gating set** (that ratification is the maintainer's at merge — same wiring recipe as
`tests/eval/piet_d1/README.md`, which established this architecture; read
that README first for the shared design rationale: isolated per-case `--do`
invocations, halt-at-first-error, display-precision tolerances).

## What this gates

| Case id | Verb | Oracle | Status @9ab18ef |
|---|---|---|---|
| `d2a_tobit_ul` | `tobit ..., ul(15000)` right-censored | D2a | GREEN |
| `d2b_tobit_ll` | `tobit ..., ll(11000)` left-censored | D2b | GREEN |
| `d2c_heckman_twostep` | `heckman ..., select(...) twostep` | D2c | GREEN |
| `d2c2_heckman_twostep_rho_boundary` | `heckman ..., select(drinkd=...) twostep` (rho truncated to -1) | professor pangram S05 | RED before boundary fix |
| `d2d_heckman_mle_reject` | `heckman` bare (Stata MLE default) — **honest-reject** | D2d | GREEN |
| `d2e_qreg_median` | `qreg` (default tau=.5) | D2e | GREEN |
| `d2f_qreg_p25` | `qreg ..., quantile(.25)` | D2f | GREEN |
| `d2g_mediate_engine_parity` | `mediate (...) (...) (..., continuous(0.5 1))` — **engine-parity only** | D2g + d2b-mediate.log | GREEN |
| `d3a_poisson` | `poisson` iid | D3a | RED (in flight) |
| `d3b_poisson_offset` | `poisson ..., offset(lpop)` | D3b | RED |
| `d3c_poisson_robust` | `poisson ..., vce(robust)` | D3c | RED |
| `d3d_ppmlhdfe_absorb` | `ppmlhdfe ..., absorb(snum)` | D3d | RED |
| `d3e_nbreg` | `nbreg` (boundary: alpha→0) | D3e | RED |
| `d3f_xtpoisson_fe` | `xtpoisson ..., fe` | D3f | RED |
| `d3g_xtlogit_fe` | `xtlogit ..., fe` (n_obs=77 drop-groups) | D3g | RED |
| `d3h_drdid_dripw` | `drdid ..., ivar() time() treatment() dripw` | drdid2x2.log | RED |

Oracles (all real StataBE 19.5):
- `internal capture d2-oracle-stata.log` (D2a–D2g)
- `internal capture d2b-mediate.log` (the only
  working Stata `mediate` invocation on this dataset)
- `internal capture d3-oracle-stata.log` (D3a–D3h)
- `<repo>/tests/validate/drdid2x2.log` + `.do` + `.dta` (full-precision ATT/SE
  from the `%20.15g` di line)
- `pangram_stata_results.log` (internal capture)
  (user-run StataNow, professor pangram S05 boundary-rho Heckman)

## Files

- **`d2d3_expected.json`** — golden key, 16 cases. `_meta` documents the
  tolerance policy, the check-severity model (FATAL coefs/required/exact vs
  SECONDARY scalars vs NOT-GRADED unpinned references), and the D2 schema
  provenance (all D2 graded field names live-verified against @9ab18ef).
- **`run_d2d3_eval.py`** — driver, same CLI as piet_d1's
  (`--piet-bin` / `$PIET_BIN` / repo-default; `--only`; `--json`).
- **`d2d3_cases.do`** — human-readable reference mirror (NOT executed by the
  driver; same halt-at-first-error rationale as piet_d1).
- **`drdid2x2_v118.dta`** — in-leg fixture: bit-identical pandas format
  conversion (v114 → v118) of the original oracle fixture (see below).
- **`RED-baseline.txt`** — the captured current-status run @9ab18ef:
  **7/15 GREEN (the original D2 set), 8/15 RED (all D3)** — a historical
  baseline that predates the added boundary-rho case. The D3 rows are the
  golden-first proof, same as piet_d1's baseline.

## Key design decisions

1. **D2 = ratification evidence, D3 = golden-first.** D2 wirings landed in
   op/piet @9ab18ef; all 7 original D2 cases gate GREEN on first run with
   oracle-pinned values — that run doubles as independent ratification of
   the [BETA D2] batch. D3 wirings are in flight; all 8 D3 cases are
   authored RED-first and must flip GREEN when the wiring lands.
2. **Heckman MLE is a REJECT case, not a golden.** Stata's bare `heckman`
   default is full MLE; the engine implements only two-step Heckit. Per the
   coordinator's gate note, `twostep` maps to the engine's bare call (D2c's
   numbers are the pins) and the MLE form must honest-reject
   (`unsupported_option`, message naming the twostep alternative). The
   oracle's MLE numbers are recorded UNPINNED in the case for a future
   engine MLE implementation.
3. **Mediate is ENGINE-PARITY ONLY (coordinator directive).** The engine
   computes a no-interaction Baron-Kenny per-unit decomposition; Stata's
   `continuous(#L0 #L1)` mediate fits a treatment#mediator-interaction
   model (VanderWeele NDE/NIE at the contrast) — ~20-95% point divergence,
   disclosed in the [BETA D2] commit and in the wiring's own source comment.
   The case gates what IS promised: two identical-seed runs → byte-identical
   full block (including bootstrap CIs), `treatment_levels == [0.5, 1.0]`,
   and `reps(199)`/`seed(20260715)` genuinely forwarded into
   `block.bootstrap`. Stata's NDE/NIE/TE are recorded UNPINNED.
4. **drdid fixture format conversion.** The original oracle `.dta` is legacy
   format v114, which the engine's dta reader rejects outright — the case
   would have stayed RED on the fixture load even after the drdid wiring
   lands (the same wrong-RED trap as piet_d1's original boottest fixture,
   caught here at authoring time instead of at gate time). The driver
   resolves `{DRDID_DTA}` to the in-leg `drdid2x2_v118.dta`, a pandas
   round-trip verified **bit-identical** column-by-column at conversion
   (600×6: id/t/Y/x1/x2/treat); the pinned full-precision oracle ATT/SE
   apply unchanged. Original path kept as fallback; `$DRDID_DTA` overrides.
5. **Severity model** (see `_meta.check_severity_model`): coefs + n_obs +
   (D2's live-verified) required/exact scalars are FATAL; D3's in-flight
   scalar field names (ll/chi2/r2_pseudo/n_groups) are SECONDARY — absent
   logs a note, present-but-wrong fails. Oracle values that are either not
   emitted by the landed schema (tobit/qreg chi2/pseudo-R2) or degenerate
   (nbreg's boundary alpha=5.61e-20 with missing se) are recorded as
   NOT-GRADED `unpinned_oracle_reference` blocks rather than silently
   dropped — note-don't-guess, in both directions.
6. **Tolerances**: same display-precision derivation as piet_d1
   (coef rtol=1e-3, scalar rtol=3e-3), with two refinements: the scalar
   comparator's atol floor is 1e-4 (so 4dp-displayed tiny pseudo-R2s don't
   force a leg-wide rtol inflation), and `d3h_drdid` tightens to
   coef_rtol=5e-4 via per-case `tol_override` because its oracle is full
   float64 precision (the B3/MI-049 ratified cross-impl value), not display
   digits.
7. **Sharp semantic traps built in**: d3b's offset (dropping it shifts
   `_cons` by >100x — unmissable), d3c's identical-b/different-se robust
   check (parsed-and-ignored-vce detector), d3g's n_obs=77 EXACT (37
   degenerate groups must be dropped from the conditional likelihood),
   d3f/d3g's `forbid_cons` (conditional-FE estimators must not fabricate a
   constant row), d2e vs d2f's tau=0.5-default vs tau=0.25-forwarded pair.

## RED-baseline summary (@9ab18ef, 2026-07-15)

Full transcript in `RED-baseline.txt`. D3 failure signatures:

- `d3a`–`d3g` (except drdid): clean `unsupported_command` halts at exactly
  the estimator line (the fixture preambles — `gen cnt`, `encode state,
  gen(snum)`, `xtset snum year`, `gen lpop`, `gen hi2` — all run clean;
  verified the halt line number is the estimator's own line in every case).
- `d3h_drdid_dripw`: a DIFFERENT, informative signature — the fixture now
  loads (after the v118 conversion) and the halt is `csdid requires
  gvar(varname)`: `drdid` currently shares dispatcher.cpp's csdid arm
  (`cmd == "csdid" || cmd == "drdid"`, ~line 3109) and mis-parses the drdid
  option surface as csdid's. The landed D3 wiring needs a genuine drdid
  path (or arm split); this case's aliases + `att_fallback` are ready for
  whichever block shape it produces.

## Wiring into the standing gate (the maintainer's job, at merge time)

Identical recipe to `tests/eval/piet_d1/README.md`'s wiring section: confirm
GREEN end-to-end after the D3 wirings land (15/15 here), then fold this
driver's exit code into `run_eval.py`'s verdict as a separate subprocess
step (the `--with-ctest` pattern). If a landed D3 block shape spells a
graded field differently than the driver's lookup guesses, Eval updates the
LOOKUP (never the pinned oracle values) — Art 10.
