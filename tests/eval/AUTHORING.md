# Golden-authoring edge-class checklist

Standing authoring discipline for the eval harness. **Process rule, not a gate** — adds no
code and changes no tolerance. Adopted from `EVAL-BACKLOG-ADJUDICATE` (2026-07-22, MI-228 #1),
motivated by `internal note RETRO-20260715-bugclass.md`.

## Why this exists

The 07-15 bug-class census (62 bugs, BUG-050..112) found that **gates catch only ~6.5% of NEW
bugs**. Gates check exactly the dimensions a golden *encodes* — a defect in a dimension no cell
encodes is invisible to the suite forever, no matter how many times the suite runs. Discovery
therefore happens in audits + dogfooding; the suite's job is to *hold the line once a dimension is
encoded*.

The misses cluster on a small, recurring set of numeric/keying edge cases:

- **±0 key class ×3** — BUG-076 → BUG-093 → BUG-108 (the same signed-zero keying trap, re-found
  three times because no golden pinned it).
- **weight validation** — BUG-107 (negative `fweight` silently used; `w=0` rows wrongly kept in
  min/max).
- **missing-vs-zero** — degenerate-N scalars emitted as `0.0` instead of Stata-missing (G6 /
  tabstat family).

This checklist makes those dimensions cheap to encode **at authoring time**: the moment you add
*any* golden whose command touches keys, weights, by-groups, or a possibly-missing variable, walk
the seven classes below. Cost per class ≈ one cell or one property; downside ≈ zero.

## Cardinal rule: missing (`.`) is not `0`

Stata missing is not zero and not a small number. It sorts as **+∞** (largest value), is
**excluded** from statistics, and for a degenerate group its scalars are **not posted at all**
(`r(N)=0`, `r(mean)`/`r(sd)` absent, confirm-number `rc=7`) — they are *not* `0.0`.

A wrong-`0.0` **cannot be pinned by a `checks[].q` float diff**: the correct target has no float,
`float(None)` never parses, and the cell would silently **skip** — reading GREEN. That silent-skip
is the exact class this guards against. Assert missing/degenerate targets with a
`properties.py` function instead (see the encoding table).

## Encoding vocabulary — map the trap to the mechanism

| mechanism | manifest field(s) | use it for |
|---|---|---|
| numeric golden | `checks[].q` (abs diff `< tol` toward the Stata value) | a well-defined **finite** number |
| reject | `oracle_kind:"reject"` (+ `stderr_regex`; rc-only if the future error code is unpinned) | input Stata **refuses** (validate-before-execute) |
| smoke both-arms | `exit_zero` **and** `stderr_regex` | behavior where a regression could flip **either** the rc **or** the message (a golden pinning only one arm half-covers) |
| property | `properties.py` fn | target that is Stata-**missing**/null (float pin impossible), or an **order/determinism** invariant |

## The seven edge classes

For each: the trap · Stata truth · how to encode · a live precedent to copy.

### 1. ±0.0 keys (signed zero in merge / join / duplicates / sort keys)
- **Trap:** master key `−0.0` and using key `+0.0` are IEEE-equal and **must match**; a keyer that
  distinguishes their bit patterns silently drops the join.
- **Encode:** smoke `--assert match` with **both** `exit_zero` and `stderr_regex` on the matched
  count, so a regression fails on both arms.
- **Precedent:** `MI227_pm0_merge_match` (BUG-108); thread BUG-076/093/108.

### 2. Negative values / weights
- **Trap:** `fweight` (and count-like inputs) must be validated non-negative **before** use;
  silently consuming a negative weight produces a wrong-but-plausible number.
- **Encode:** `oracle_kind:"reject"` (`rc=3` `bad_input`) with a `stderr_regex` naming the offending
  value — validate-then-compute.
- **Precedent:** `MI227_neg_fweight_reject` (BUG-107).

### 3. Zero-weight rows
- **Trap:** `w=0` rows must be **excluded** from min/max (and from N/sum semantics); keeping them
  yields a wrong extremum that no ordinary golden would notice.
- **Encode:** `checks[].q` numeric pin on the affected statistic (e.g. `max=3`, not `100`).
- **Precedent:** `MI227_w0_minmax` (BUG-107).

### 4. Empty / all-missing groups
- **Trap:** an `N=0` (all-missing) group must report mean/sd as **missing**, not `0.0`; on current
  engines the degenerate early-return can make even the **count** come back null.
- **Encode:** `properties.py` — not a float pin (cardinal rule). Assert count `= 0` and
  mean/sd missing directly.
- **Precedent:** `summarize_allmiss_missing`; `tabstat`'s all-missing group in
  `tabstat_singleton_missing` (G6 degenerate-scalars family).

### 5. Duplicate keys
- **Trap:** duplicate keys change join cardinality (`m:m` fan-out vs first-match) and
  `duplicates`/`collapse` grouping; Stata's specific rule must be pinned, not assumed.
- **Encode:** smoke with a row-count / matched-count check plus `--assert`; when the semantics is
  "raw missing-included" vs "non-missing gathered", use a property.
- **Precedent:** `collapse_first_last_raw` / `collapse_firstnm_lastnm` (first/last missing
  semantics); keying thread BUG-108.

### 6. Single-observation group
- **Trap:** an `N=1` group has **missing** sd/semean (division by `N−1`), not `0.0`.
- **Encode:** `properties.py` (same null-vs-float reason as class 4).
- **Precedent:** `tabstat_singleton_missing`.

### 7. Missing vs zero (the umbrella)
- **Trap:** any time a fixture *can* produce missing — in a stat, a sort position, a comparison, or
  a generated value — decide explicitly: is the correct answer `0`, or missing? They are different
  targets with different encodings.
- **Encode:** finite → `checks[].q`; missing/null → property. Sort/compare invariants involving
  missing (NaN-last, both directions) → a property that re-derives the permutation independently.
- **Precedent:** `summarize_allmiss_missing`, `tabstat_singleton_missing`, gsort NaN-last identity
  (G13b baseline-freeze property).

## How to apply

1. **Trigger.** Authoring or reviewing **any** golden whose command consumes keys
   (`merge`/`join`/`duplicates`/`sort`/`gsort`/`collapse ... by`/`egen ... group`), weights
   (`[fw=]`/`[aw=]`/`[pw=]`), by-groups, or a variable that can be missing.
2. **Walk the seven rows.** For each class the fixture *could* exercise, either add a cell/property
   that pins it, or record in the cell `_comment` why it is out of scope. Silence is not coverage.
3. **Every cell `_comment` carries** (harness convention, already universal in `manifest.json`):
   (a) the Stata oracle anchor — do-file **and** committed golden log; (b) a RED-on-current
   confirmation (or "GREEN today = baseline freeze" for regression pins); (c) the flip condition
   (what change makes it turn green).
4. **Missing/degenerate targets → property, never a float pin.** A `checks[].q` toward a null
   target silently skips and reads GREEN — the silent-wrong class this whole checklist exists to
   pre-empt.
5. **Reject cases pin the refusal, not just the absence.** Prefer a `stderr_regex` that names the
   cause; rc-only is acceptable when the future error code is intentionally unpinned (note it in the
   `_comment`).

## Scope note

This is a discretionary Eval-pen process rule (Constitution Art 10 — eval additions are
by-request, no-gaming, tolerances only toward the oracle). It governs how new goldens are authored
and reviewed; it does not modify any existing cell, tolerance, or gate.
