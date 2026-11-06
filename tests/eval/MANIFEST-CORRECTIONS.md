# MANIFEST-CORRECTIONS.md

Log of golden re-pins in `tests/eval/manifest.json` / `tests/eval/golden/*.csv` where
the CORRECTION direction is documented (CONSTITUTION Art 10 — goldens move only
TOWARD the oracle, never away, and every move carries a dated justification here).
Individual manifest cells cross-reference their entry via a `_comment` field of the
form `MANIFEST-CORRECTIONS.md YYYY-MM-DD: <root cause> -- <correction>.`

This file was created 2026-07-15. Several manifest cells predating it
(COUNCIL-20260710 sprint: `cmd_qreg` tau-flag re-pins ×3, `cmd_ols` --vcov no-op
drop, `cmd_feols`->`iv` command-name correction, the E5 G=1 vce(cluster) User
ruling) cite `MANIFEST-CORRECTIONS.md 2026-07-10`, but that date's entries were
never captured as file content at the time — the record lived only inline in each
cell's `_comment` in `manifest.json`. The four 2026-07-10 entries below were
**backfilled on 2026-07-15 from those manifest `_comment` strings** (User-requested);
each is a faithful expansion of its `_comment`, not a contemporaneous capture —
where an entry states something beyond the `_comment` text it says so explicitly.

---

## 2026-07-10 — `qreg_tau0p25` / `qreg_tau0p5` / `qreg_tau0p75` (golden/qreg.csv)
*(backfilled 2026-07-15 from the cells' `_comment` strings)*

**Cells:** the three `qreg` group cells (`tests/eval/manifest.json`), all graded
against the shared `golden/qreg.csv` via `tau0p25:`/`tau0p5:`/`tau0p75:`-prefixed
gkeys.

**Root cause:** `cmd_qreg` reads `--tau`, not `--quantile`. The cells' old args
passed the quantile via a flag the command does not read, so the flag was
silently ignored and **all three cells ran at the default tau=0.5 (median)** —
the tau=0.25 and tau=0.75 goldens had baked median results under quartile names.

**Correction applied (COUNCIL-20260710 sprint):**
- args corrected to `--tau` on all three cells;
- `qreg_tau0p25` and `qreg_tau0p75` re-pinned to the correct tau=0.25 / tau=0.75
  coeftables;
- `qreg_tau0p5` values **unchanged** — tau=0.5 is the silent default, so its
  golden was already the correct median fit (verified byte-identical post-fix).

**Justification for direction:** toward the oracle — the old 0.25/0.75 goldens
pinned the WRONG quantile (a silently-defaulted median), the new ones pin the
quantile the cell names.

---

## 2026-07-10 — `ols_header_scalars` (golden/header_scalars.csv)
*(backfilled 2026-07-15 from the cell's `_comment` string)*

**Cell:** `ols` / `ols_header_scalars` (`tests/eval/manifest.json`, group
`header_scalars`).

**Root cause:** `cmd_ols` ignores `--vcov` entirely (it always emits
`se_classic` + `se_hc1` regardless). The cell's args carried `--vcov iid`, a
no-op. Per E6 (COUNCIL-20260710 sprint item), the engine now hard-rejects that
no-op flag, so the stale args would have started failing for a non-substantive
reason.

**Correction applied:** dropped `--vcov iid` from the cell's args. **No golden
values changed** (verified — the flag never had any effect on output).

**Justification for direction:** not a value move at all — an args hygiene fix
keeping the cell runnable under the E6 honest-reject behavior. Golden untouched.

---

## 2026-07-10 — `iv_hansenj_cluster` (golden/iv_hansenj.csv)
*(backfilled 2026-07-15 from the cell's `_comment` string)*

**Cell:** `iv_hansenj_cluster` (`tests/eval/manifest.json`, group `iv`).

**Root cause:** the cell originally invoked `feols` — but `cmd_feols` is
OLS-only and **silently ignored** `--endog`/`--instruments`, dropping the
endogenous `treated` regressor entirely. The golden had therefore baked an OLS
result under an IV name.

**Correction applied:** command corrected `feols` -> `iv`; golden re-pinned to
the correct 2SLS coeftable (the `treated` endogenous coefficient now present,
vcov=cluster(gvar)).

**Justification for direction:** toward the oracle — the old golden certified
the wrong estimator. Caveat recorded at backfill time: the re-pinned SE values
were taken from the engine's own corrected 2SLS output (engine-echo, no
independent Stata run for this fixture existed on 07-10), under the engine's
then-current small-sample vcov convention. That residual provenance gap is
resolved by the **2026-07-15 entry below**, which supersedes this entry's SE
values (b's stand).

---

## 2026-07-10 — `E5_g1cluster` (reject cell — User ruling, no golden CSV)
*(backfilled 2026-07-15 from the cell's `_comment` string)*

**Cell:** `feols` / `E5_g1cluster` (`tests/eval/manifest.json`, group
`g1_cluster_edge`, `oracle_kind: reject`). Gates E5/BUG-061 (G=1 vce(cluster)).

**Ruling (User, 2026-07-10):** the engine REJECTS a single-cluster
vce(cluster) request as `bad_input`, **deliberately diverging from Stata**,
which runs at rc=0 and emits missing SE/t/p/CI for the affected coefficient
(real-Stata finding documented in the COUNCIL-20260710 intake
`draft_goldens.json`/`README-intake.md`). This is a ruled engineering choice,
not a parity miss. Empirically reconfirmed live against cb24e53: rc=3,
`error.code=bad_input`, message
`insufficient clusters: vce(cluster) requires at least 2 clusters, but the cluster variable has 1`.

**Correction applied:** the intake's draft `b_x`-success golden was SUPERSEDED;
the cell is pinned as a reject case
(`expect_error: {code: bad_input, message_contains: "insufficient clusters"}`).

**Justification for direction:** Art 10's documented-justification clause is
exactly for this shape — the pin moves toward the **ruled contract**, with the
divergence from raw Stata behavior explicitly recorded (User ruling, dated,
with the real-Stata counterfactual preserved in the intake docs) rather than
silently absorbed.

---

## 2026-07-15 — `iv_hansenj_cluster` (golden/iv_hansenj.csv)

**Cell:** `iv` / `iv_hansenj_cluster` (`tests/eval/manifest.json`, group `iv`) —
`iv --data {iv_hansenj} --y visits_w --x age_z --endog treated --instruments
female,edu_hi --vcov cluster --cluster gvar`.

**Root cause / provenance history:**
- The cell's SE values were re-pinned once already, on 2026-07-10, when
  `cmd_feols` was found to be OLS-only and silently dropping `--endog`/
  `--instruments` (golden had baked an OLS result under an IV name). That
  07-10 re-pin took its numbers from the ENGINE's own newly-corrected 2SLS
  output — an **engine-echo**, not independent Stata truth, since no
  StataBE oracle run for this exact fixture existed at the time.
- 2026-07-15 (Phase-F engine-stint, branch `op/cobra-beta-f` @d2588a9): the
  engine's `iv` (2SLS) standard-error convention was flipped to match Stata
  `ivregress`'s **asymptotic default** (previously it used a small-sample
  correction convention). This was proven by ratio algebra against the
  `piet_d1` StataBE 19.5 oracle fixtures (d1a-d1d): the iid factor is
  `sqrt(N/(N-K))`, robust HC1->HC0, and the cluster factor
  `(G/(G-1))*(N-1)/(N-K)` is removed under the new default. A new
  `--small` flag restores the old small-sample factors and reproduces
  Stata `ivregress, small` **exactly** — verified live: se=13684.467 (iid)
  and se=7517.013 (cluster) on the d1 fixtures, matching Stata's `small`
  output digit-for-digit. See
  `internal note f-oracle-pins.md`
  (`## Verified NO-WORK items`, ivregress `small` line) for the source
  probe.
- Net effect on this cell: the 07-10 engine-echo SEs were pinned under the
  OLD (small-sample) convention. Phase-F's flip moves the engine's default
  output to the asymptotic convention, so this golden must move with it —
  this is the correction target actually reaching Stata's own
  `ivregress`-default convention, not a second engine-echo.

**Correction applied (golden/iv_hansenj.csv):**
| key | old | new |
|---|---|---|
| `coeftable[age_z].se` | `1.851434884825637e+00` | `1.8045308127975468e+00` |
| `coeftable[treated].se` | `2.123008165358559e+01` | `2.0692240821481036e+01` |

`coeftable[age_z].b` (`-4.392831529123313e+00`) and `coeftable[treated].b`
(`6.149098686322412e+01`) are **unchanged** — the convention flip is
SE-only (2SLS point estimates are invariant to the vcov formula).

**Justification for direction:** toward the oracle. The new values are the
Phase-F gated build's `iv --vcov cluster --cluster gvar` output under the
Stata-`ivregress`-default asymptotic convention, which is independently
verified against real StataBE 19.5 (not another engine-echo) via the
`--small`-reproduces-old-values proof above. Re-pin is a strict improvement
in provenance quality over the 07-10 engine-echo baseline.

---

## 2026-07-15 — `C1F19_pmzero_fe` fixture wiring (BUG-093 root cause; no golden values touched)

**Cell:** `feols` / `C1F19_pmzero_fe` (`tests/eval/manifest.json`, group
`c1f19_signzero`, golden `golden/f19_pmzero.csv`).

**Root cause:** the cell resolves `{f19_pmzero}` to
`<data_dir>/f19_pmzero.hbin`, but **no prep block or generator ever
materialized that file**. The cell's own `_comment` claimed "Fixture hbin
rebuilt from the committed CSV" — that rebuild was never wired; the batch#2
author hand-built the .hbin into one worktree's data dir only. Any fresh data
dir (op-eval, the 07-12 ship gate, CI) hit cobra rc=3 `cannot open`, and
`run_eval.py`'s `diagnose()` labeled every rc!=0 as "engine error / crash
(non-zero exit)". That mislabel spawned **BUG-093** — the engine is proven
clean (200/200 loop + all pins exact on the committed CSV via both conversion
routes).

**Correction applied:**
- `manifest.json`: added the documented convert-prep to the cell —
  `"prep": {"kind": "convert", "csv": "golden/f19_pmzero_input.csv", "as":
  "f19_pmzero"}` — same shape as the existing convert-prep cells (e.g.
  `C1F20c_bom_firstvar`), so the fixture now materializes from the committed
  CSV through the engine's own `convert` at grade time; `_comment` extended
  with the wiring record.
- `run_eval.py` `diagnose()`: when a failing run's stderr carries cobra's
  structured error JSON with `code=="data_io"`, the suspected_cause is now
  `missing/unreadable fixture (data_io) — infra, not engine` instead of the
  generic crash label (cobra emits error JSON on STDERR, per the
  reject-grader's own parsing — checked there, not in the parsed-stdout
  field).

**Justification for direction:** no golden value moved — this is fixture
provisioning + diagnostic-label hygiene. The convert-prep route additionally
preserves the case's RED-first design (`cobra convert` preserves IEEE -0.0,
per the cell's original `_comment`), so the signed-zero grouping bug the
golden pins remains fully exercised.

---

## 2026-07-15 — BUG-101 factor-coef naming re-pin (7 cells, coeftable NAMES only)

**Cells:** `fvops_ib2`, `fvops_ibn`, `fvops_inumlist` (`golden/fvops_*.csv`),
`cont_ix_cxif`, `cont_ix_fc_full` (`golden/cont_ix_*.csv`),
`G13_collinearity_omitted` (`golden/g13_collinearity_omitted.csv` +
`properties.py::collinearity_omitted_set_equals`'s `want`/`want_absent` sets),
`MI193b_keepdrop_coefplot` (`golden/mi193b_keepdrop_coefplot.csv`).

**Root cause:** cobra (fixed build, `op-cobra` @f6c323e) renamed factor-variable
coefficient NAME strings from its own old, non-Stata `varname==level` convention
to Stata's real `level.varname` convention (e.g. `foreign==1` -> `1.foreign`;
factor#factor interaction `foreign==1#rep78==5` -> `1.foreign#5.rep78`;
factor#continuous `foreign==1#weight` -> `1.foreign#weight` — note NO `c.`
prefix on the continuous leg inside a factor interaction, a pre-existing cobra
naming gap left untouched by BUG-101; continuous#factor `weight#foreign==1` ->
`weight#1.foreign`). These 7 manifest cells' `checks[].q` keyed coeftable rows
by the old `==` name, so post-fix they hit "cobra output lacks this field"
(the query no longer resolves any row) — the manifest/golden `_comment`
convention flagged these as a golden blind-spot pinned to cobra's own (wrong)
prior convention rather than Stata's.

**Correction applied:** renamed the coef-NAME key strings only, in both
`manifest.json` `checks[].q` and the corresponding `golden/*.csv` `key` column
(golden CSVs are a flat `key,value` lookup table, not position-dependent, so no
row-reordering was needed). Also updated `properties.py`'s
`collinearity_omitted_set_equals` `want = {"foreign==1#rep78==5"}` ->
`{"1.foreign#5.rep78"}` and `want_absent` (structural-zero cells + base level)
to the same convention. **Every numeric value (b/se/t/p/n_obs/dof_resid/df_m)
was verified byte-identical** against the fixed cobra binary's live output
before and after the rename — confirmed no number moved, only the name string.

**Justification for direction:** toward the oracle — real StataBE 19.5 emits
`level.varname` (confirmed independently, e.g. `MI193b`'s own pre-existing
`_comment` already cited Stata's `_b[1.foreign]=1767.29224` under the correct
name while the cell's `q` still queried the old `foreign==1` key). The old
goldens were pinned to cobra's own wrong internal convention, not Stata's —
exactly the golden blind-spot Art 10 exists to correct. `run_eval` reaches
101/101 after this re-pin (was 94/101, these 7 the only failures).
