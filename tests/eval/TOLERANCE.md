# TOLERANCE.md — the codified Stata-parity tolerance standard (gate ① rubric)

> Authority: CONSTITUTION Part 1 gate ① ("within the per-quantity tolerance Eval ratifies") +
> Art 10 (eval owns tolerances). **Pen: eval lane** — this document is the ratification rubric
> those clauses point to. Ratified by **User ruling 2026-08-06** (interactive session; scribed by
> The maintainer under the explicit-User-directive exception, notice MSG-MI-372). Future edits: eval lane,
> within the laws below; the ladder values and §4 process law are User-ratified and change only
> with a new User ruling.

## 1. Two gates, never conflated

- **Numeric gate** — every emitted scalar vs the pinned Stata oracle, at its ladder tier (§2).
  This is what `run_eval` manifests encode via `rtol`/`atol`.
- **Display gate** — answerbook/pangram waves compare *formatted output strings*. A finite numeric
  tolerance can never guarantee string equality: if the true value sits at a rounding boundary of
  the last displayed digit (`…x4999…` vs `…x5000…`), two results agreeing to 1e-11 can still
  print differently. Therefore:
  - The display gate stays a separate comparison (strings, as today).
  - **Boundary-flip adjudication rule**: a display mismatch adjudicates PASS iff (a) the numeric
    gate passes at the quantity's tier AND (b) the difference is confined to the last displayed
    digit at a half-way rounding boundary. Each such adjudication is recorded per-pin in the wave
    close — never silently.

Display width is **never** a relaxation key for the numeric gate: the coefficient table shows ~7
significant digits (`%9.0g` family), but `e()`/`r()` expose full doubles to programs, so "it only
displays 2 digits" does not license a loose internal tolerance.

## 2. The ladder

A check takes the **tightest tier its mechanism admits**. Difficulty is not a key. The only lawful
relaxation keys are (a) the oracle's own precision limit and (b) definitional discretization —
both are properties of the target quantity, not of our engine (Art 10 unchanged: tolerances are
never relaxed for engine convenience).

| Tier | rtol | Applies to | Mechanism key |
|---|---|---|---|
| **T0** | 0 (exact) | N, df, rank, integer counts, omitted-collinear lists, factor codes/labels, sort orders on unique keys, pinned formatted strings, rc codes | Discrete quantities admit no tolerance |
| **T1** | 1e-12 | Closed-form single-pass arithmetic: sums, means, counts-weighted moments, `lincom` point values, well-conditioned OLS `b` | Disagreement can only come from accumulation order; quad/compensated accumulation makes this reachable |
| **T2** | **1e-9 (baseline)** | Post-decomposition linear algebra: SE, V, t/z, CI bounds, R², predicted values, postestimation derived quantities | 9 significant digits = displayed 7 + two guard digits |
| **T3** | 1e-8 … 1e-7 | Converged iterative MLE quantities (logit/probit/poisson/nbreg/tobit/heckman/xt-MLE b, se, ll) | Stata's own convergence tolerances (`ltolerance`/`nrtolerance` class) mean the oracle itself is not defined tighter; matching beyond that is noise-fitting |
| **T4** | 1e-6 … 1e-5, always paired with an atol | Discretization-defined quantities: Gauss–Hermite quadrature results (`xtlogit, re` family), qreg sparsity-function SEs, HAC/prewhitening-bandwidth-derived stats, kernel-density values | The quantity's *definition* carries discretization error > 1e-7. **Every T4 manifest entry carries a one-line `tolnote` naming the mechanism.** |
| **T5** | distributional | ①-S stochastic estimators (bootstrap p/CI, multiplier draws) | Fixed-seed bit-reproducible internally + distributional agreement with Stata (existing ①-S law, unchanged); a served bootstrap p is graded by §3.5 |

## 3. Special rules

1. **p-values / tail probabilities: no bare rtol.** Relative error near p→0 explodes without
   meaning. Compare the upstream statistic (t, z, χ², F) at its tier; p itself gets `atol`
   (default 1e-8).
   Manifest form (P-FIELD-RULING 2026-10-14): a tier row carrying this `atol` names the p field it
   reaches in `field`, by label (a `label= number` field, e.g. `p`) or by position (`{"rtable_row": 4}`:
   el(r(table),4,1), printed 4 fields after the record's `cols=` field). That atol reaches only that field;
   every other field keeps the row's rtol. A whole-record p atol is not admitted.
2. **Near-zero floor.** Every rtol check on a quantity that can legitimately be ≈0 (coefficients,
   differences, covariances) carries `atol = rtol_tier × scale`, where `scale` is a named
   magnitude for the quantity family (e.g. `max|b|` of the model, or the quantity's own se).
   Bare-rtol checks on near-zero quantities are a defect class, not a strictness virtue.
3. **Float-storage variables**: both sides are rounded to float before comparison (a float column
   carries ~7 significant digits total; comparing doubles behind it is comparing artifacts).
4. **Comparisons are value-based, never bit-based, against the oracle.** Bit identity is an
   *internal* property (freeze corpus, run-to-run determinism), not the parity criterion.
5. **T5 bootstrap p.** (User ruling 2026-10-13, option A adopted unamended: the private coordination tree
   `internal note boottest-family/T5-RULING.md`.) A manifest tier entry

       {arms: <regex>, tier: T5, reps: B, level: 0.99, tolnote: <mechanism>, ratified_by: <this ruling>}

   grades a served bootstrap p record PASS iff

       |p_piet - p_stata| <= z(0.995) * sqrt(pbar * (1 - pbar) / B) + 1/B

   with pbar = (p_piet + p_stata) / 2 and z(0.995) = 2.5758293035489 (two-sided 99%).
   - **B** = the arm's reps: 99 unless the arm's do line sets `reps()` otherwise; the manifest entry's `reps`
     must equal the do line's value. **level** = 0.99.
   - **Granularity rule**: p_piet must be a multiple of 1/B or 1/(B+1) exactly as the oracle's value is, so a
     non-bootstrap p cannot pass as T5. A p that is on neither grid FAILS. (The ruling left the reading of
     "exactly as the oracle's value is" to the harness task, to be settled from boottest's p definition; it is
     settled in the private coordination tree `internal note boottest-family/T5-GRID.md`.)
   - **Scope**: T5 applies to the bootstrap p field only and is never applied to a deterministic quantity. rc,
     record prefix, field count and text fields stay T0 (exact); the fit's own statistics in the same record
     (t, z, df_r, N_clust, clustvars) stay T2. (In the internal supervisor suites the entry names that field:
     `field: p`.)
   - **Double replay** (the internal half of ①-S): the receipt replays each T5 arm twice with the same binary
     and the same seed and requires a byte-identical p. A pair that differs FAILS the arm, whatever its
     distance from Stata.
   - **Paperwork per row**: like T4, every T5 manifest row carries a one-line `tolnote` naming the mechanism
     (resampling draws differ between two correct implementations) and
     `ratified_by: T5-RULING 2026-10-13 (User)`.

## 4. Process law (User ruling 2026-08-06)

1. **Bit-moving performance patches pass the merge gate iff ladder-GREEN.** A speed change that
   moves output bits (accumulation-order changes, compensated/dd accumulation, vectorized math,
   reordered kernels) is mergeable when every affected quantity still meets its ladder tier vs the
   oracle. The freeze corpus is re-captured **in the same merge session** (Art 8 coupled-delta
   clause); the comparison authority for adjudication is the *oracle number + ladder*, never the
   previous engine build's bits.
2. **Parallel/multicore kernels are held to the same standard, plus determinism:**
   - ladder-GREEN vs the oracle at every supported thread count;
   - **run-to-run deterministic on a given build**: the floating-point reduction DAG must be a
     function of the data only — fixed block constants (never `n/nthreads`), fixed-order block
     merge, no atomic scatter-add into shared accumulators, no OpenMP `reduction`, BLAS pinned to
     one thread under our own pool. Under these laws 1-core and N-core outputs are byte-identical,
     which remains P9's exit evidence.
   - Boundary of the ruling: it legalizes bit movement **across builds** (via re-pin, clause 1);
     it does **not** waive same-build determinism — byte-comparable freeze fixtures are the
     mechanism that keeps every re-pin auditable.
3. **Tail audit (one-time, then standing).** Every existing check looser than 1e-9 is classified
   into a tier with its mechanism key, or tightened, or filed as a defect being laundered by a
   loose tolerance. Baseline snapshot 2026-08-06, from the live manifests:
   `1e-6 ×428 · 1e-8 ×272 · 1e-7 ×122 · 1e-5 ×56 · 5e-6 ×30 · 1e-3 ×18 · 1e-4 ×10 · 1e-10 ×6`
   (+ 460 already at 1e-9, 43 exact, 36 at 1e-12). The 1e-3/1e-4 rows are presumed-guilty until
   they carry a T4/T5 mechanism note. New checks land with a tier tag from day one.

   **ADDENDUM 2026-08-06 (WS-J, EVAL-TOL-AUDIT execution — verification, not a re-ratification).**
   A direct parse of `manifest.json` at execution time (`python3 -c 'json.load(...)'`, counting
   every `checks[]` dict with an `rtol`/`atol` key, cross-checked against `report.json`'s own
   per-check output) measures **743** total numeric checks, not the ~1481 the row above sums to —
   almost exactly **half** at every tier except the exact-value row (18 measured vs 43 stated). The
   per-tier shape matches (each bucket here is a clean 2× or near-2× of the corresponding bucket
   above), so this reads as a double-count at the *original* measurement time, not a manifest
   change since ratification (`manifest.json`'s last content commit predates today's ratification
   commit). This addendum does not alter the ratified snapshot line above — the User ruling that
   line documents is a process ratification, not a rescindable measurement — it records the
   as-executed recount for whoever audits this audit next. The tail-audit tiering work in this
   session's manifest commits was done against the **measured 743**, not the stated ~1481; every
   check has a `tol_tier`, confirmed by direct grep (0 untagged of 743).

## 5. Consequences acknowledged at ratification

- T1/T2 make compensated (double-double/Neumaier) accumulation for `X'X`/`X'y`/RSS/robust-meat
  effectively mandatory on large-N/ill-conditioned surfaces (Stata accumulates in quad precision —
  `[M-5] quadcross`). That engine change and this standard land under §4.1 in one re-pin ceremony.
- The pangram/answerbook string comparison stays the outer gate for what users see; this document
  governs the scalar layer beneath it.

## Adjudicated cell: `nbreg` `e(alpha)` vs live Stata (2026-09-01, BUG-383 #7)

Captured Stata `0.7252590019748644`, ours `0.7252590079097877` — relative
difference **8.2e-9**, just outside the default rtol 1e-9. `alpha` is the
overdispersion parameter of an iteratively maximized likelihood; the deviation
is convergence-tolerance shape (a T3/T4 item on this ladder), not an algebra
error: the coefficient vector and `e(ll)` agree within the default band on the
same fixture. Oracle self-precision is the relaxation key here — Stata's own
`nbreg` stops on its `ltolerance()`/`tolerance()` defaults, so the last ~1e-8
of alpha is not a pinned quantity on either side. **Ruling: accepted at T4
(rtol 1e-7) for this member; not a defect.** The off-pin sweep counts it RED at
the default band by design — the sweep reports at rtol 1e-9 and this file is
where exceptions are named, so the RED row is the pointer to this paragraph
rather than a bug to chase again.
