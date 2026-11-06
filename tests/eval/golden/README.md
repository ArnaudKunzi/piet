# Root-level goldens — what a capture supports, and what it does not

The wave directories carry their own READMEs (`aer_b4/`, `aer_w1/`). This file
is for captures that live at the root of `tests/eval/golden/`, and it exists to
record the limits of one — a capture that is genuine is not automatically a
capture that a replay can consume, and the difference is invisible in a diff.

## `aer_estim2_glm_*` — the glm oracle (2026-09-10, harvested 2026-09-14)

25 arms of `glm` against StataBE 19.5 on `sysuse auto` and a 240-row synthetic
panel: family × link × `vce(cluster)`/`vce(eim)`/`scale(1)`/`irls`/`eform` ×
`offset()`/`exposure()`, plus a `STATA_OIM_CHECK` arm that measures how far
Stata's default-tolerance `e(V)` sits from the exact OIM at the reported `b`.

**The capture is sound.** The log echoes the committed 99-line `.do` exactly,
carries the standard batch banner and the `DONE:glm` sentinel, and has zero
`r(NNN);` lines; 24 arms are rc 0 with internally consistent pin counts. One
arm, `syn_C_eim`, is **r(504) "matrix has missing values"** and — by the pin
program's `if _rc != 0 exit` — emits no pins at all, so the EIM identity for
the synthetic design is UNCAPTURED. A reader counting `ARM|` lines would
otherwise assume 25 arms of evidence; there are 24.

**Two limits a consumer must know before cutting goldens from it.**

1. **The input CSVs are not bit-exact.** Both were written with a plain
   `export delimited ..., replace` — no `format %24.17e` + `datafmt`, which is
   what every other `aer_estim2_*` oracle on main uses. So the text carries
   ≤16 significant digits and small values lose more: one synthetic `x` row is
   `.001776312910837` against an exact `0.001776312910836964` (2.0e-14
   relative). The PINS were fit on Stata's in-memory doubles, so a replay that
   reads these CSVs cannot reach relative error 0 on a non-integer pin. The
   `rtol 1e-9` ladder is unaffected; the tighter self-captured-golden ratchet
   is not available against these inputs.
2. **`foreign` is exported as its VALUE LABEL, not as 0/1.** The column reads
   `Domestic`/`Foreign` because the export carried no `nolabel`, while the
   `auto_D` / `auto_D_probit` arms regress on `foreign` as a numeric dependent
   variable. No other golden CSV in this tree carries label text. Any consumer
   must recode, and a consumer that does not will silently disagree.

Neither is repaired by editing the files: the log's value is that it echoes
*this* `.do`, so a corrected export belongs in a SECOND capture, not in a
rewrite of this one. The follow-up that capture also owes, from the same read:
`vce(robust)` without a cluster (zero arms today, so the ML sandwich's
`N/(N-1)` factor is unmeasured), `fweight`/`pweight`, and plain `irls`.

**Decoding the pins.** `MATRIX|` / `SCALAR|` / `STATA_OIM_CHECK|` cells are
Stata's `%21x` hex-float, whose exponent is HEXADECIMAL. Python's
`float.fromhex` rejects them, and a naive `X`→`p` substitution mis-decodes the
exponent silently. Use `tests/eval/rederive_golden.py`'s `stata_hexfloat`.
