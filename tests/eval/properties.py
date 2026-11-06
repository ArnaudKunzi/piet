"""Property checks for the Eval Harness (Constitution Art 10) -- mechanical/
structural gates that compare a fresh cobra invocation against ANOTHER cobra
invocation or an independently-recomputed ground truth, rather than a fixed
Stata golden CSV. Wired into run_eval.py's existing (pre-designed, previously
unused) `properties` hook: a manifest case lists `"properties": ["name", ...]`
and each function here runs as `fn(Ctx) -> (ok: bool, detail: str)`.

COUNCIL-20260710 golden batch #1 (PLAYBOOK.md §7.2 items 7/9/13):
  - egen group()/collapse (first)/(last) write a --out .hbin and emit NO
    stdout JSON at all, so the standard golden_csv/checks[] mechanism (which
    diffs cobra's JSON) cannot reach their values -- these read the written
    .hbin back directly via tools/hbin_read.py.
  - The G13 mechanical gates (byte-compare, sort permutation-identity) are
    pure self-consistency checks: no Stata truth is needed (structural facts
    about the engine's own documented, deterministic behavior).
"""
import filecmp
import json
import math
import sys
import tempfile
from collections import Counter
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent.parent.parent / "tools"
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))
from hbin_read import read_hbin  # noqa: E402


def _tmp(name):
    return str(Path(tempfile.gettempdir()) / f"eval_prop_{name}.hbin")


# ───────── G6 — degenerate summarize/tabstat scalars (gates F9) ─────────
# A plain `checks[].q` golden can't express "should be null" (golden_checks
# always does float(got), and float(None) raises -- reported as "cobra field
# missing", not a clean pass/fail toward a null target) -- these three are
# properties so the missing-vs-wrong-zero distinction can be asserted directly.
def summarize_allmiss_missing(ctx):
    """`summarize` on an all-missing numeric variable must report mean/sd as
    MISSING (JSON null), not 0.0 -- Stata: r(N)=0, r(mean)/r(sd) NOT POSTED
    at all (confirmed live via `return list`; see g6_degenerate_scalars.do /
    the return-list probe cited in its header comment). cobra's
    Frame::summarize early-return at N==0 leaves mean/sd at the struct
    default 0.0 -- CONFIRMED live: `cobra eval --summarize allmiss` gives
    N:0 mean:0.0 sd:0.0 today."""
    r = ctx.run_cobra("eval", ["--data", ctx.data("g6_degenerate"),
                                "--summarize", "allmiss"], tidy=False)
    if r["rc"] != 0 or r["json"] is None:
        return False, f"cobra eval --summarize failed rc={r['rc']} err={r['err'][:200]}"
    s = r["json"].get("summarize", {}).get("allmiss", {})
    n_ok, mean_ok, sd_ok = s.get("N") == 0, s.get("mean") is None, s.get("sd") is None
    if n_ok and mean_ok and sd_ok:
        return True, "N=0, mean/sd correctly missing"
    return False, (f"N={s.get('N')!r} (want 0), mean={s.get('mean')!r} (want missing), "
                    f"sd={s.get('sd')!r} (want missing) -- Stata: r(N)=0, r(mean)/r(sd) not posted")


def summarize_string_var_honest(ctx):
    """`summarize` on a string/factor variable must behave like Stata:
    succeed (rc=0) with N=0 -- CONFIRMED live IN STATA via `capture
    summarize strvar` -> rc=0, r(N)=0 (g6_degenerate_scalars.do STRVAR
    block; this is NOT a Stata error, settling the ambiguity F9's blind-spot
    note left open). cobra's Frame::summarize currently throws a raw
    runtime_error via require_f64 ('column is a factor, not f64'), caught by
    main()'s generic handler as an UNSTRUCTURED "internal" error (rc=1) --
    CONFIRMED live. RED today by construction; auto-upgrades to a real N==0
    check the moment the engine handles string/factor columns gracefully."""
    r = ctx.run_cobra("eval", ["--data", ctx.data("g6_degenerate"),
                                "--summarize", "strvar"], tidy=False)
    if r["rc"] != 0:
        return False, (f"cobra eval --summarize strvar fails rc={r['rc']} err={r['err'][:150]} "
                        f"-- Stata succeeds with r(N)=0; gates F9's engine string-var handling")
    s = (r["json"] or {}).get("summarize", {}).get("strvar", {})
    if s.get("N") == 0:
        return True, "summarize strvar succeeds with N=0, matching Stata"
    return False, f"summarize strvar succeeded but N={s.get('N')!r} (want 0)"


def tabstat_singleton_missing(ctx):
    """`tabstat ..., by(g)` on a singleton group (N=1) must report sd/semean
    as MISSING (sample variance undefined for n-1=0) -- Stata: r(sd)=.
    (g6_degenerate_scalars.do TABSTAT block). cobra's stat_of()
    (apps/cecon_cli.cpp ~4284-4305) gives sd=0.0/semean=0.0 for n==1 --
    CONFIRMED live. Bonus, same fixture: stat_of's n==0 early-return also
    makes the all-missing group's own COUNT wrongly missing -- Stata's count
    is a well-defined 0 even when mean/sd are undefined -- checked here too."""
    r = ctx.run_cobra("tabstat", ["--data", ctx.data("g6_degenerate"),
                                   "--vars", "y", "--by", "g",
                                   "--stats", "n,mean,sd,semean"], tidy=False)
    if r["rc"] != 0 or r["json"] is None:
        return False, f"cobra tabstat failed rc={r['rc']} err={r['err'][:200]}"
    groups = r["json"].get("groups", [])
    by_value = {g.get("value"): g.get("stats", {}).get("y", {}) for g in groups}
    g2, g3 = by_value.get(2.0, {}), by_value.get(3.0, {})  # singleton (N=1), all-missing (N=0)
    bad = []
    if g2.get("sd") is not None:
        bad.append(f"singleton sd={g2.get('sd')!r} (want missing)")
    if g2.get("semean") is not None:
        bad.append(f"singleton semean={g2.get('semean')!r} (want missing)")
    if g3.get("n") != 0:
        bad.append(f"all-missing group n={g3.get('n')!r} (want 0 -- Stata count is "
                    f"well-defined even when mean/sd aren't)")
    if bad:
        return False, "; ".join(bad)
    return True, "singleton sd/semean correctly missing; all-missing group n correctly 0"


# ───────────────────── G7 — egen group() label order (gates F8) ─────────────────────
def egen_group_alpha_order(ctx):
    """`egen g=group(grp)` must number levels by SORTED (alphabetical) label,
    not by the factor's first-seen storage order (F8). Fixture:
    g7_egen_order.hbin, grp in {A..E}, stored (first-seen) order B,C,D,E,A --
    see g7_egen_tabulate_order.do/.csv for the Stata cross-check. Runs
    --egen fresh (eval's own stdout carries only n_rows/n_cols, never the
    row-level mapping) and reads the written column back."""
    out = _tmp("egen_group")
    r = ctx.run_cobra("eval", ["--data", ctx.data("g7_egen_order"),
                                "--egen", "g=group(grp)", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"cobra eval --egen failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    levels = cols["grp__levels"]      # stored (first-seen) dictionary order
    codes = cols["grp"]               # per-row factor code into `levels`
    g = cols["g"]                     # per-row egen group id (what we're checking)
    want_rank = {lab: i + 1 for i, lab in enumerate(sorted(levels))}  # Stata: A=1..E=5
    bad = []
    for i in range(n):
        label = levels[codes[i]]
        want = want_rank[label]
        got = g[i]
        if got != want:
            bad.append(f"row {i} grp={label!r} got g={got:g} want g={want}")
    if bad:
        return False, f"egen group() ids not in Stata alphabetical order: {'; '.join(bad[:5])}"
    return True, f"egen group() ids match Stata alphabetical rank for all {n} rows"


# ───────── G9 — collapse (first)/(last)/(firstnm)/(lastnm) (gates F13) ─────────
# Stata truth (g9_collapse_firstlast.do / .csv), raw within-group row order 1..3:
#   g=1 (x: ., 5, 7)   first=.  last=7   firstnm=5  lastnm=7
#   g=2 (x: 3, 4, .)   first=3  last=.   firstnm=3  lastnm=4
#   g=3 (x: ., ., 8)   first=.  last=8   firstnm=8  lastnm=8
#   g=4 (x: 9, ., .)   first=9  last=.   firstnm=9  lastnm=9
_G9_WANT = {
    1: {"x_first": None, "x_last": 7.0, "x_firstnm": 5.0, "x_lastnm": 7.0},
    2: {"x_first": 3.0, "x_last": None, "x_firstnm": 3.0, "x_lastnm": 4.0},
    3: {"x_first": None, "x_last": 8.0, "x_firstnm": 8.0, "x_lastnm": 8.0},
    4: {"x_first": 9.0, "x_last": None, "x_firstnm": 9.0, "x_lastnm": 9.0},
}


def _close(got, want):
    if want is None:
        return math.isnan(got)
    return (not math.isnan(got)) and abs(got - want) < 1e-9


def collapse_first_last_raw(ctx):
    """`collapse (first)/(last)` must return the RAW first/last row of the
    group (missing included) -- cobra currently gathers non-missing values
    only (== what Stata calls firstnm/lastnm), CONFIRMED wrong on this
    fixture today. (firstnm)/(lastnm) are checked separately since they
    currently hard-error and would abort this whole --stat string if
    combined into one collapse call."""
    out = _tmp("collapse_firstlast")
    r = ctx.run_cobra("collapse",
                       ["--data", ctx.data("g9_collapse_firstlast"),
                        "--stat", "(first) x_first=x (last) x_last=x",
                        "--by", "g", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"collapse (first)/(last) failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    bad = []
    for i in range(n):
        gi = int(cols["g"][i])
        want = _G9_WANT[gi]
        for k in ("x_first", "x_last"):
            got = cols[k][i]
            if not _close(got, want[k]):
                bad.append(f"g={gi} {k} got={got!r} want={want[k]!r}")
    if bad:
        return False, f"collapse (first)/(last) diverges from Stata raw semantics: {'; '.join(bad)}"
    return True, f"collapse (first)/(last) matches Stata raw (missing-included) semantics for all {n} groups"


def collapse_firstnm_lastnm(ctx):
    """(firstnm)/(lastnm) don't exist in cobra yet (F13 fix sketch: 'add
    firstnm/lastnm') -- CONFIRMED live: 'unknown stat firstnm'. RED today by
    construction; auto-upgrades to a real value check the moment the engine
    implements them."""
    out = _tmp("collapse_firstnmlastnm")
    r = ctx.run_cobra("collapse",
                       ["--data", ctx.data("g9_collapse_firstlast"),
                        "--stat", "(firstnm) x_firstnm=x (lastnm) x_lastnm=x",
                        "--by", "g", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, (f"(firstnm)/(lastnm) not yet implemented (rc={r['rc']} "
                        f"err={r['err'][:150]}) -- gates F13's addition")
    n, cols = read_hbin(out)
    bad = []
    for i in range(n):
        gi = int(cols["g"][i])
        want = _G9_WANT[gi]
        for k in ("x_firstnm", "x_lastnm"):
            got = cols[k][i]
            if not _close(got, want[k]):
                bad.append(f"g={gi} {k} got={got!r} want={want[k]!r}")
    if bad:
        return False, f"(firstnm)/(lastnm) values wrong: {'; '.join(bad)}"
    return True, f"(firstnm)/(lastnm) match Stata for all {n} groups"


# ───────────── G13a — hbin byte-compare, same op twice (mechanical) ─────────────
def hbin_byte_identical_rerun(ctx):
    """Running the SAME data-transform verb twice on the SAME input must
    produce BYTE-IDENTICAL .hbin output (deterministic engine; no timestamps/
    randomness baked into the file). Freezes today's determinism so a future
    .dta/CSV reader rewrite or native-hbin-writer change (C2-F4 x C3-F9 x
    C1-F14, C3-F7) can't silently introduce nondeterminism. No Stata truth
    needed -- PLAYBOOK.md §7.2 item 13."""
    a, b = _tmp("bytecmp_a"), _tmp("bytecmp_b")
    args = ["--data", ctx.data("auto"), "--vars", "-price,foreign", "--out"]
    ra = ctx.run_cobra("gsort", args + [a], tidy=False)
    rb = ctx.run_cobra("gsort", args + [b], tidy=False)
    if ra["rc"] != 0 or rb["rc"] != 0:
        return False, f"gsort rerun failed rc=({ra['rc']},{rb['rc']})"
    same = filecmp.cmp(a, b, shallow=False)
    return same, ("byte-identical rerun" if same else "SAME op, SAME input produced DIFFERENT bytes")


# ───────── G13b — sort permutation-identity (mechanical, radix-sort baseline) ─────────
def _expected_stable_order(k, ascending):
    """Independent Python re-derivation of cobra's OWN documented gsort rule
    (src/data/frame.cpp Frame::gsort comment): stable sort, NaN sorts LAST
    under BOTH ascending and descending (a deliberate divergence from Stata's
    own gsort, ledgered in contract §22 -- NOT what this property checks;
    this checks self-consistency/stability only, the exact property
    RAW-speed.md's F8 (radix-sort rewrite) verification calls for)."""
    idx = list(range(len(k)))

    def keyfn(i):
        v = k[i]
        if v != v:  # NaN
            return (1, 0.0)  # NaN group sorts after all real numbers, both directions
        return (0, v if ascending else -v)
    return sorted(idx, key=keyfn)  # Python's sort is stable, matches std::stable_sort


def sort_permutation_identity(ctx):
    """gsort's stable_sort must produce EXACTLY the permutation the documented
    rule implies (stable ties, NaN-last-both-directions) -- the baseline any
    future radix-sort rewrite (C2-F8, gated 'AFTER F8 ordering fix') must keep
    reproducing bit-for-bit. Checked both ascending and descending on a
    fixture with duplicate keys AND missings (g13_sort_fixture.hbin). No
    Stata truth needed -- PLAYBOOK.md §7.2 item 13."""
    n0, src = read_hbin(ctx.data("g13_sort_fixture"))
    k0 = src["k"]
    bad = []
    for asc, flag in ((True, "k"), (False, "-k")):
        out = _tmp(f"sortperm_{flag.replace('-', 'd')}")
        r = ctx.run_cobra("gsort", ["--data", ctx.data("g13_sort_fixture"),
                                     "--vars", flag, "--out", out], tidy=False)
        if r["rc"] != 0:
            bad.append(f"{flag}: gsort failed rc={r['rc']}")
            continue
        n, cols = read_hbin(out)
        got = [int(x) for x in cols["idx"]]
        want = _expected_stable_order(k0, asc)
        if got != want:
            bad.append(f"{flag}: got idx order {got} != expected stable permutation {want}")
    if bad:
        return False, "; ".join(bad)
    return True, "gsort permutation matches the independently-recomputed stable/NaN-last order (asc + desc)"


# ───── G13c — collinearity omitted[]-set equality boundary fixture (mechanical) ─────
def collinearity_omitted_set_equals(ctx):
    """`i.foreign##i.rep78` on auto.hbin has 2 structurally-empty interaction
    cells (foreign==1 at rep78 in {1,2} both have 0 obs -- CONFIRMED live via
    the cross-tab) plus ONE further term caught by the collinearity sweep
    once those empty cells are excluded from the design -- src/core/
    collinear.h's own header comment cites 'i.foreign##i.rep78 on sysuse
    auto' as ITS documented worked example. Freezes today's (already
    Stata-matching, per that header's convention) omitted[] SET so a future
    Cholesky-based collinearity rewrite (C2-F10, REPIN batch, 'omitted[]-set
    equality gate') can't silently change WHICH term gets dropped. No Stata
    run needed: purely mechanical/structural (PLAYBOOK.md §7.2 item 13)."""
    r = ctx.run_cobra("feols", ["--data", ctx.data("auto"), "--y", "price",
                                 "--x", "i.foreign##i.rep78", "--vcov", "iid"])
    if r["rc"] != 0 or r["json"] is None:
        return False, f"feols failed rc={r['rc']} err={r['err'][:200]}"
    got = set(r["json"].get("omitted", []))
    want = {"1.foreign#5.rep78"}
    names = {c["name"] for c in r["json"].get("coeftable", [])}
    want_absent = {"1.foreign#1.rep78", "1.foreign#2.rep78", "1.rep78"}  # structural-zero cells + base level (BUG-101: level.var naming)
    bad = []
    if got != want:
        bad.append(f"omitted={sorted(got)} want={sorted(want)}")
    present_that_shouldnt_be = want_absent & names
    if present_that_shouldnt_be:
        bad.append(f"unexpectedly present terms: {sorted(present_that_shouldnt_be)}")
    if bad:
        return False, "; ".join(bad)
    return True, f"omitted[] == {sorted(want)}, structural-zero terms correctly absent from coeftable"


# ───────── L1 M3 CSV-shim retirement — new native verbs (fix/l1-m3-native e31133b) ─────────
# rename/clonevar/encode/setcol are all --data/--out data-transform verbs with no stdout JSON
# body worth diffing (rename/clonevar/setcol print only a one-line confirmation; encode's stdout
# JSON is checked directly, but its written column still needs a hbin read-back) -- property-
# driven, same convention as the COUNCIL-20260710 G7/G9/G13 batch above. Every expected value
# below is HAND-DERIVED in Python straight from the committed fixture CSV under tests/eval/
# golden/ -- never captured from the candidate verb's own output (mock-fidelity/pinning law).
def l1m3_rename_multipair_identity(ctx):
    """`rename` must be a pure column-NAME swap: every value, the grp factor
    dictionary (order + -1 missing code), and column order stay exactly what
    the raw fixture CSV implies -- only the header names change. Fixture
    golden/l1m3_rename_fixture.csv (5 rows): id/score f64 (score row 2
    missing), grp factor with a deliberately non-alphabetical first-seen
    dictionary (zeta,alpha) and one missing cell (row 4, code -1) -- picked
    so a future accidental re-sort-on-rename regression would be caught.
    Renames id/score/grp -> identifier/metric/category in one multi-pair
    call."""
    src, out = _tmp("l1m3_rename_src"), _tmp("l1m3_rename_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_rename_fixture.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("rename", ["--data", src, "--pairs",
                                  "id=identifier,score=metric,grp=category",
                                  "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"rename failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    want_id = [1.0, 2.0, 3.0, 4.0, 5.0]
    want_score = [10.5, None, 30.25, 40.75, 50.5]          # None == missing (NaN)
    want_grp_code = [0, 1, 0, -1, 1]                       # first-seen: zeta=0, alpha=1
    want_grp_levels = ["zeta", "alpha"]
    bad = []
    if n != 5:
        bad.append(f"n_rows={n} want 5")
    present = [k for k in cols if not k.endswith("__levels")]
    if present != ["identifier", "metric", "category"]:
        bad.append(f"columns={present} want [identifier,metric,category] in that order "
                    f"(old names must be gone, order preserved)")
    else:
        for i, want in enumerate(want_id):
            if cols["identifier"][i] != want:
                bad.append(f"identifier[{i}]={cols['identifier'][i]!r} want {want}")
        for i, want in enumerate(want_score):
            got = cols["metric"][i]
            ok = math.isnan(got) if want is None else (got == want)
            if not ok:
                bad.append(f"metric[{i}]={got!r} want {want!r}")
        codes = [int(x) for x in cols["category"]]
        levels = list(cols["category__levels"])
        if codes != want_grp_code:
            bad.append(f"category codes={codes} want {want_grp_code}")
        if levels != want_grp_levels:
            bad.append(f"category__levels={levels} want {want_grp_levels}")
    if bad:
        return False, "; ".join(bad)
    return True, "rename is a pure name swap: all 5 rows' values/dictionary/order match the hand-derived fixture truth"


def l1m3_clonevar_factor_append(ctx):
    """`clonevar` must APPEND the clone as the LAST column, leave the source
    column completely untouched, and give the clone an IDENTICAL factor
    dictionary (level strings, order, every row's code including the
    missing row's -1) -- Stata clonevar semantics. Same fixture as the
    rename case (golden/l1m3_rename_fixture.csv), cloning grp -> grp_clone."""
    src, out = _tmp("l1m3_clonevar_src"), _tmp("l1m3_clonevar_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_rename_fixture.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("clonevar", ["--data", src, "--pairs", "grp=grp_clone",
                                    "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"clonevar failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    want_code = [0, 1, 0, -1, 1]
    want_levels = ["zeta", "alpha"]
    present = [k for k in cols if not k.endswith("__levels")]
    bad = []
    if present != ["id", "score", "grp", "grp_clone"]:
        bad.append(f"columns={present} want [id,score,grp,grp_clone] (clone appended LAST, "
                    f"source untouched)")
    for name in ("grp", "grp_clone"):
        codes = [int(x) for x in cols.get(name, [])]
        levels = list(cols.get(name + "__levels", []))
        if codes != want_code:
            bad.append(f"{name} codes={codes} want {want_code}")
        if levels != want_levels:
            bad.append(f"{name} levels={levels} want {want_levels}")
    if bad:
        return False, "; ".join(bad)
    return True, "clonevar appended grp_clone at end with a dictionary/codes identical to the untouched source grp"


def l1m3_encode_alpha_missing(ctx):
    """`encode` must assign 1-based codes by ALPHABETICAL level order (Stata
    encode), independent of the source factor's first-seen storage order,
    and leave a missing source cell missing (NaN) in the generated f64
    column -- checked against both the stdout `levels` array and the
    written column. Fixture golden/l1m3_encode_fixture.csv: region
    first-seen order zeta,alpha,mid (deliberately non-alphabetical) with
    one blank cell (row 3) -- alphabetical codes alpha=1,mid=2,zeta=3 are
    hand-derived by sorting the fixture's own distinct labels in Python."""
    src, out = _tmp("l1m3_encode_src"), _tmp("l1m3_encode_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_encode_fixture.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("encode", ["--data", src, "--var", "region", "--gen",
                                  "region_code", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"encode failed rc={r['rc']} err={r['err'][:200]}"
    want_levels = ["alpha", "mid", "zeta"]
    j = r["json"] or {}
    bad = []
    if j.get("levels") != want_levels:
        bad.append(f"stdout levels={j.get('levels')} want {want_levels}")
    if j.get("n_levels") != 3:
        bad.append(f"stdout n_levels={j.get('n_levels')} want 3")
    n, cols = read_hbin(out)
    want_code = [3.0, 1.0, None, 2.0, 3.0, 1.0]   # zeta,alpha,missing,mid,zeta,alpha
    got = cols.get("region_code", [])
    for i, want in enumerate(want_code):
        g = got[i] if i < len(got) else None
        ok = (g is not None and math.isnan(g)) if want is None else (g == want)
        if not ok:
            bad.append(f"region_code[{i}]={g!r} want {want!r}")
    if bad:
        return False, "; ".join(bad)
    return True, "encode assigns alphabetical 1-based codes (alpha=1,mid=2,zeta=3) matching stdout levels[], missing cell stays NaN"


def l1m3_setcol_f64_patch(ctx):
    """`setcol --dtype f64` (in-place, no --new) must patch ONLY the named
    column's values from the supplied file (blank row == missing), while
    every neighbor column stays completely untouched. Fixture golden/
    l1m3_rename_fixture.csv (score patched) + golden/
    l1m3_setcol_f64_values.csv (5 headerless values, row 3 blank)."""
    src, out = _tmp("l1m3_setcolf64_src"), _tmp("l1m3_setcolf64_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_rename_fixture.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("setcol", ["--data", src, "--var", "score", "--values",
                                  "{repo:golden/l1m3_setcol_f64_values.csv}",
                                  "--dtype", "f64", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"setcol failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    want_id = [1.0, 2.0, 3.0, 4.0, 5.0]
    want_score = [111.1, 222.2, None, 444.4, 555.5]
    want_grp_code = [0, 1, 0, -1, 1]
    want_grp_levels = ["zeta", "alpha"]
    bad = []
    for i, want in enumerate(want_id):
        if cols["id"][i] != want:
            bad.append(f"id[{i}]={cols['id'][i]!r} want {want} (neighbor must be untouched)")
    for i, want in enumerate(want_score):
        got = cols["score"][i]
        ok = math.isnan(got) if want is None else (got == want)
        if not ok:
            bad.append(f"score[{i}]={got!r} want {want!r}")
    codes = [int(x) for x in cols["grp"]]
    levels = list(cols["grp__levels"])
    if codes != want_grp_code or levels != want_grp_levels:
        bad.append(f"grp codes/levels={codes}/{levels} want {want_grp_code}/{want_grp_levels} "
                    f"(neighbor must be untouched)")
    if bad:
        return False, "; ".join(bad)
    return True, "setcol f64 patch: score matches the supplied values (blank->missing), id/grp neighbors untouched"


def l1m3_setcol_factor_retype(ctx):
    """`setcol --dtype f64` on an EXISTING factor column must destring it in
    place (dtype flips factor -> f64, the __levels dictionary disappears,
    name/position preserved), while non-target columns stay untouched.
    Fixture golden/l1m3_rename_fixture.csv (grp retyped) + golden/
    l1m3_setcol_destring_values.csv (5 headerless numeric values, row 3
    blank == missing)."""
    src, out = _tmp("l1m3_setcolretype_src"), _tmp("l1m3_setcolretype_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_rename_fixture.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("setcol", ["--data", src, "--var", "grp", "--values",
                                  "{repo:golden/l1m3_setcol_destring_values.csv}",
                                  "--dtype", "f64", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"setcol failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    want_id = [1.0, 2.0, 3.0, 4.0, 5.0]
    want_score = [10.5, None, 30.25, 40.75, 50.5]
    want_grp = [7.0, 8.0, None, 9.0, 10.0]
    bad = []
    if "grp__levels" in cols:
        bad.append("grp still carries a __levels dictionary -- retype to f64 did not drop factor-ness")
    for i, want in enumerate(want_id):
        if cols["id"][i] != want:
            bad.append(f"id[{i}]={cols['id'][i]!r} want {want}")
    for i, want in enumerate(want_score):
        got = cols["score"][i]
        ok = math.isnan(got) if want is None else (got == want)
        if not ok:
            bad.append(f"score[{i}]={got!r} want {want!r}")
    for i, want in enumerate(want_grp):
        got = cols["grp"][i]
        ok = math.isnan(got) if want is None else (got == want)
        if not ok:
            bad.append(f"grp[{i}]={got!r} want {want!r}")
    if bad:
        return False, "; ".join(bad)
    return True, "setcol factor->f64 retype: grp destrings to the supplied numeric values, id/score untouched"


# Independent ground truth for the collapse label-integrity guard below -- hand-derived
# straight from golden/l1m3_collapse_multiby.csv, never from cobra's own collapse output.
_L1M3_COLLAPSE_WANT_MEAN = {
    ("zeta", "z2"): 20.0, ("zeta", "z1"): 5.0,
    ("alpha", "a2"): 150.0, ("alpha", "a1"): 7.0,
    ("mid", "m2"): 2000.0, ("mid", "m1"): 9.0,
}
_L1M3_COLLAPSE_WANT_MEMBERSHIP = {
    "z1": "zeta", "z2": "zeta", "a1": "alpha", "a2": "alpha", "m1": "mid", "m2": "mid",
}


def l1m3_collapse_multiby_label_integrity(ctx):
    """`collapse --by cat,member` with TWO factor by-vars must keep each
    output row's (cat,member) pair aligned to the TRUE grouping -- the
    permanent class guard for the corpus-scale silent mislabel the OLD PIET
    CSV restore dance produced on the 2nd factor by-var (latent through
    B.5.1, fixed on this native engine path -- BUG-124/BUG-125 campaign).
    Fixture golden/l1m3_collapse_multiby.csv: 3 categories (zeta,alpha,mid
    -- first-seen order deliberately non-alphabetical) x 2 members each,
    members ALSO first-seen in non-alphabetical order within their category
    (z2 before z1, a2 before a1, m2 before m1) -- exactly the shape where a
    first-seen-vs-sorted dictionary mixup on EITHER by-var would swap a row
    into an impossible category (e.g. emit (mid,z1), since z1 never
    legitimately co-occurs with mid). mean(x) per group and the true
    membership map are both hand-derived directly from the raw CSV."""
    src, out = _tmp("l1m3_collapse_src"), _tmp("l1m3_collapse_out")
    rc = ctx.run_cobra("convert", ["{repo:golden/l1m3_collapse_multiby.csv}", src], tidy=False)
    if rc["rc"] != 0:
        return False, f"convert fixture failed rc={rc['rc']} err={rc['err'][:200]}"
    r = ctx.run_cobra("collapse", ["--data", src, "--stat", "(mean) mx=x",
                                    "--by", "cat,member", "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"collapse failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    if n != 6:
        return False, f"n_rows={n} want 6 groups"
    cat_lv, mem_lv = cols["cat__levels"], cols["member__levels"]
    seen = set()
    bad = []
    for i in range(n):
        cat = cat_lv[cols["cat"][i]]
        mem = mem_lv[cols["member"][i]]
        mx = cols["mx"][i]
        seen.add((cat, mem))
        true_cat = _L1M3_COLLAPSE_WANT_MEMBERSHIP.get(mem)
        if true_cat != cat:
            bad.append(f"row {i}: member={mem!r} labeled under cat={cat!r}, true cat={true_cat!r} "
                        f"-- LABEL MISALIGNMENT")
            continue
        want_mx = _L1M3_COLLAPSE_WANT_MEAN[(cat, mem)]
        if not math.isclose(mx, want_mx, rel_tol=1e-9):
            bad.append(f"({cat},{mem}) mx={mx!r} want {want_mx!r}")
    missing = set(_L1M3_COLLAPSE_WANT_MEAN) - seen
    if missing:
        bad.append(f"missing groups: {sorted(missing)}")
    if bad:
        return False, "; ".join(bad)
    return True, f"all {n} (cat,member) groups correctly aligned to the true membership with matching hand-derived means"


# ───────── L1b — native joinby (fix/l1b-rowverbs abd9825) ─────────
# `joinby` writes an --out .hbin with only a one-line stderr confirmation -- property-driven,
# same convention as the L1M3/G7/G9/G13 batches above. Every expected multiset below is
# HAND-DERIVED straight from the committed fixture CSVs under tests/eval/golden/ (Stata joinby
# semantics: per shared key, full master x using Cartesian; unmatched rows on EITHER side are
# dropped) -- never captured from the candidate verb's own output. Row ORDER is deliberately
# not pinned (multiset compare): Stata's own joinby does not promise an output order without
# an explicit sort, so content+count is the parity surface.
def _l1b_join_f64(ctx):
    """Shared runner: convert the two f64-key fixtures fresh + joinby on k.
    Returns (err_or_None, n, cols)."""
    m, u, out = _tmp("l1b_join_m"), _tmp("l1b_join_u"), _tmp("l1b_join_out")
    r = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_master.csv}", m], tidy=False)
    if r["rc"] != 0:
        return f"convert master failed rc={r['rc']} err={r['err'][:200]}", None, None
    r = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_using.csv}", u], tidy=False)
    if r["rc"] != 0:
        return f"convert using failed rc={r['rc']} err={r['err'][:200]}", None, None
    r = ctx.run_cobra("joinby", ["--data", m, "--using", u, "--keys", "k",
                                  "--out", out], tidy=False)
    if r["rc"] != 0:
        return f"joinby failed rc={r['rc']} err={r['err'][:200]}", None, None
    n, cols = read_hbin(out)
    return None, n, cols


# Hand-derived truth for the f64-key pair (master 5 rows x using 6 rows):
#   key 1: master {10,20} x using {100,200,300} -> 6 rows
#   key 2: {30} x {400}                         -> 1 row
#   key 3: master-only                          -> dropped
#   key 4: using-only                           -> dropped
#   key -0.0 (master) vs +0.0 (using): MATCH    -> 1 row (engine make_key convention --
#     deliberate, documented divergence from PIET's old text-repr compare, per the L1b brief)
# Python tuple equality/hashing treats -0.0 == 0.0, so the Counter compare below asserts the
# +-0 match transparently: the (0.0, 70.0, 600.0) row can only exist if -0.0 matched +0.0.
_L1B_JOIN_WANT = Counter([
    (1.0, 10.0, 100.0), (1.0, 10.0, 200.0), (1.0, 10.0, 300.0),
    (1.0, 20.0, 100.0), (1.0, 20.0, 200.0), (1.0, 20.0, 300.0),
    (2.0, 30.0, 400.0),
    (0.0, 70.0, 600.0),
])


def l1b_joinby_mm_fanout(ctx):
    """m:m fan-out: key groups present on BOTH sides must emit the full
    master x using Cartesian per key -- 8 rows total on this fixture pair,
    every (k, mval, uval) triple matching the hand-derived multiset,
    including the +-0.0 key match (master -0.0 row mval=70 joined to using
    +0.0 row uval=600)."""
    err, n, cols = _l1b_join_f64(ctx)
    if err:
        return False, err
    got = Counter((float(cols["k"][i]), float(cols["mval"][i]), float(cols["uval"][i]))
                   for i in range(n))
    if got != _L1B_JOIN_WANT:
        missing = _L1B_JOIN_WANT - got
        extra = got - _L1B_JOIN_WANT
        return False, (f"n={n} (want 8); missing rows: {sorted(missing.elements())[:4]}; "
                        f"unexpected rows: {sorted(extra.elements())[:4]}")
    return True, "8-row m:m Cartesian matches the hand-derived multiset, incl. the -0.0/+0.0 key match"


def l1b_joinby_unmatched_dropped(ctx):
    """Unmatched-key rows must be DROPPED from BOTH sides: master key 3
    (mval=40) has no using partner, using key 4 (uval=500) has no master
    partner -- neither value may appear anywhere in the output, and the
    surviving key set is exactly {0, 1, 2}."""
    err, n, cols = _l1b_join_f64(ctx)
    if err:
        return False, err
    bad = []
    mvals = {float(x) for x in cols["mval"]}
    uvals = {float(x) for x in cols["uval"]}
    keys = {float(x) for x in cols["k"]}
    if 40.0 in mvals:
        bad.append("master-only key 3's row (mval=40) leaked into the output")
    if 500.0 in uvals:
        bad.append("using-only key 4's row (uval=500) leaked into the output")
    if keys != {0.0, 1.0, 2.0}:
        bad.append(f"surviving key set {sorted(keys)} != [0.0, 1.0, 2.0]")
    if bad:
        return False, "; ".join(bad)
    return True, "unmatched rows dropped on both sides; surviving keys exactly {0,1,2}"


# Hand-derived truth for the factor-key pair. The two fixtures encode region with
# DELIBERATELY different first-seen dictionaries (master: zeta=0,alpha=1,mid=2; using:
# mid=0,alpha=1,zeta=2) -- a code-based join would pair master zeta(0) rows with using
# mid(0) rows etc.; the label-based truth is:
#   zeta : master {1,4} x using {80}    -> (zeta,1,80), (zeta,4,80)
#   alpha: master {2}   x using {60,70} -> (alpha,2,60), (alpha,2,70)
#   mid  : master {3}   x using {50}    -> (mid,3,50)
_L1B_JOIN_FACTOR_WANT = Counter([
    ("zeta", 1.0, 80.0), ("zeta", 4.0, 80.0),
    ("alpha", 2.0, 60.0), ("alpha", 2.0, 70.0),
    ("mid", 3.0, 50.0),
])


def l1b_joinby_factor_key_levels(ctx):
    """FACTOR keys must match by LEVEL STRING across files, not by per-file
    integer code -- the case that breaks code-based joins. The fixtures'
    dictionaries are deliberately misaligned (see _L1B_JOIN_FACTOR_WANT
    derivation above); multiset equality against the label-based truth is
    only satisfiable by a string-keyed join (any code-based pairing yields
    impossible rows like (zeta,1,50))."""
    m, u, out = _tmp("l1b_joinf_m"), _tmp("l1b_joinf_u"), _tmp("l1b_joinf_out")
    r = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_fmaster.csv}", m], tidy=False)
    if r["rc"] != 0:
        return False, f"convert fmaster failed rc={r['rc']} err={r['err'][:200]}"
    r = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_fusing.csv}", u], tidy=False)
    if r["rc"] != 0:
        return False, f"convert fusing failed rc={r['rc']} err={r['err'][:200]}"
    r = ctx.run_cobra("joinby", ["--data", m, "--using", u, "--keys", "region",
                                  "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"joinby failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    levels = cols["region__levels"]
    got = Counter((levels[cols["region"][i]], float(cols["mscore"][i]),
                    float(cols["ubonus"][i])) for i in range(n))
    if got != _L1B_JOIN_FACTOR_WANT:
        missing = _L1B_JOIN_FACTOR_WANT - got
        extra = got - _L1B_JOIN_FACTOR_WANT
        return False, (f"n={n} (want 5); missing rows: {sorted(missing.elements())[:4]}; "
                        f"unexpected rows: {sorted(extra.elements())[:4]} -- a code-based "
                        f"join (per-file dictionary mixup) produces exactly this failure shape")
    return True, "factor keys matched by level string across misaligned per-file dictionaries (5-row truth)"


def l1b_joinby_column_union(ctx):
    """Output schema must be the column union: ALL master columns (key
    included, master's values/positions) followed by the using side's
    NON-KEY columns -- checked in exact order on both fixture pairs."""
    err, n, cols = _l1b_join_f64(ctx)
    if err:
        return False, err
    bad = []
    present = [k for k in cols if not k.endswith("__levels")]
    if present != ["k", "mval", "uval"]:
        bad.append(f"f64 pair columns={present} want [k, mval, uval]")
    m, u, out = _tmp("l1b_joinu_m"), _tmp("l1b_joinu_u"), _tmp("l1b_joinu_out")
    r = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_fmaster.csv}", m], tidy=False)
    r2 = ctx.run_cobra("convert", ["{repo:golden/l1b_joinby_fusing.csv}", u], tidy=False)
    if r["rc"] != 0 or r2["rc"] != 0:
        return False, f"convert failed rc=({r['rc']},{r2['rc']})"
    r = ctx.run_cobra("joinby", ["--data", m, "--using", u, "--keys", "region",
                                  "--out", out], tidy=False)
    if r["rc"] != 0:
        return False, f"joinby failed rc={r['rc']} err={r['err'][:200]}"
    n2, cols2 = read_hbin(out)
    present2 = [k for k in cols2 if not k.endswith("__levels")]
    if present2 != ["region", "mscore", "ubonus"]:
        bad.append(f"factor pair columns={present2} want [region, mscore, ubonus]")
    if bad:
        return False, "; ".join(bad)
    return True, "column union correct on both pairs: master columns then using non-key columns, in order"


# ───────── C1-F16 %tw value-golden wiring (FIX:EVAL-F16TW-PROPWIRE) ─────────
# `import-dta` is a --out-only verb with no stdout JSON body (same shape as
# rename/clonevar/setcol above), so this is property-driven per the L1M3
# convention -- l1m3_rename_multipair_identity is the direct precedent.
def f16_tw_value(ctx):
    """`import-dta` on golden/f16_tw_fixture.dta must read back the raw Stata
    %tw family-index (Q1=(b) DECISION, P1-W1-REREVIEW-20260729.md §2.3 --
    cobra stores the value-identical raw week-index, not a %td day-snap
    conversion). Truth source: golden/f16_tw_value.csv (Stata truth-source
    golden/f16_tw_oracle.log, StataNow 19.5, tw(2020w1)_raw=3120). GREEN as of
    P1-A1-TW-CALENDAR (2026-07-30), which made the reader store %tw with no
    rescale and retired C1F16_tw_misscale_reject in the same commit. Prior RED
    arms, for the record: pre-P1-A0 the reader mis-scaled by *7 (%td epoch-day
    treatment) and read back {21840,21847,21854,21861} at rc=0; P1-A0 (3da80bd)
    turned that into an honest rc=3 reject, so this property failed on the
    import itself until the decode landed."""
    out = _tmp("f16_tw_value")
    r = ctx.run_cobra("import-dta", ["{repo:golden/f16_tw_fixture.dta}", out], tidy=False)
    if r["rc"] != 0:
        return False, f"import-dta failed rc={r['rc']} err={r['err'][:200]}"
    n, cols = read_hbin(out)
    golden_path = Path(__file__).resolve().parent / "golden" / "f16_tw_value.csv"
    rows = [ln.split(",") for ln in golden_path.read_text().splitlines()[1:] if ln.strip()]
    want_wk = [float(wk) for wk, y in rows]
    want_y = [float(y) for wk, y in rows]
    bad = []
    if n != len(want_wk):
        bad.append(f"n_rows={n} want {len(want_wk)}")
    else:
        for i, want in enumerate(want_wk):
            got = cols["wk"][i]
            if got != want:
                bad.append(f"wk[{i}]={got!r} want {want!r}")
        for i, want in enumerate(want_y):
            got = cols["y"][i]
            if got != want:
                bad.append(f"y[{i}]={got!r} want {want!r}")
    if bad:
        return False, "; ".join(bad)
    return True, "wk/y read back exactly matches golden/f16_tw_value.csv (raw Stata %tw family-index, value-identical import-dta)"


# ───────── P1-A3-DATASIG-ORACLE — `datasignature` reverse-oracle (writer Q9) ─────────
def p1a3_datasignature_oracle(ctx):
    """`datasignature` reverse-oracle (P1 ARC 3 head, writer Q9). The verb is
    absent from the engine today (grep-confirmed `datasignature|data_signature`
    = src/ apps/ 0 hit @bd3dc3f) -- P1-A3-DATASIG-VERB lands it; THIS property
    is the gate its own DoD names as the GREEN flip target. Truth-source: Stata
    BE19.5 live capture, golden/datasig_oracle.do/.log (sysuse auto +
    golden/f16_tw_fixture.dta) and golden/datasig_oracle_kfirm.do/.log
    (internal fixture kitchensink_firm.dta, read-only cross-lane reference).
    Findings there: deterministic on rerun · sensitive to variable reorder ·
    sensitive to a single cell value edit · sensitive to double->float storage
    narrowing · INSENSITIVE to display-format-only changes (%tw->%td left the
    signature byte-identical). The parenthetical width term in Stata's own
    output moves inconsistently across fixtures on reorder (changes on the
    12-var and 2-var fixtures, not the 3-var one) -- no literal Stata text
    format can be assumed portable. Q9 RULING: engine-deterministic, NOT
    Stata-exact (no published algorithm to target bit-for-bit, and the
    observed field is not even self-consistent across fixtures to reverse-
    engineer). So this property asserts the REPRODUCIBILITY CONTRACT Stata's
    own signature exhibits -- deterministic + reorder/value-sensitive --
    against whatever opaque string cobra's `datasignature` emits, rather than
    literal equality with a captured Stata value."""
    base = ctx.data("auto")
    r0 = ctx.run_cobra("datasignature", ["--data", base], tidy=False)
    if r0["rc"] != 0:
        return False, (
            f"datasignature not yet implemented (rc={r0['rc']}, "
            f"{(r0['err'] or r0['out']).strip()[:150]!r}) -- RED expected until "
            f"P1-A3-DATASIG-VERB lands. Stata oracle (golden/datasig_oracle.log): "
            f"MARK_A_BASE=74:12(71728):3831085005:1395876116 · MARK_A_REORDER="
            f"74:12(10689):514868382:2788714433 · MARK_A_CELLEDIT="
            f"74:12(71728):1869568254:3678136668 (determinism/reorder/value-sensitivity "
            f"already confirmed against live Stata; see manifest _comment for the full "
            f"Q9 ruling and the kitchensink/f16_tw cross-checks)."
        )
    sig_base_1 = r0["out"].strip()
    r1 = ctx.run_cobra("datasignature", ["--data", base], tidy=False)
    if r1["rc"] != 0:
        return False, f"datasignature became flaky on rerun: rc={r1['rc']} err={r1['err'][:150]}"
    sig_base_2 = r1["out"].strip()
    if sig_base_1 != sig_base_2:
        return False, (f"determinism violated: rerun on identical, unmodified data gave "
                        f"{sig_base_1!r} then {sig_base_2!r}")

    reordered = _tmp("p1a3_datasig_reordered")
    ro = ctx.run_cobra("order", ["--data", base, "--vars", "foreign", "--first",
                                  "--out", reordered], tidy=False)
    if ro["rc"] != 0:
        return False, f"order (mutation setup) failed rc={ro['rc']} err={ro['err'][:150]}"
    r2 = ctx.run_cobra("datasignature", ["--data", reordered], tidy=False)
    if r2["rc"] != 0:
        return False, f"datasignature on reordered copy failed rc={r2['rc']} err={r2['err'][:150]}"
    sig_reordered = r2["out"].strip()
    if sig_reordered == sig_base_1:
        return False, (
            f"reorder-insensitive: `order foreign,first` left the signature unchanged "
            f"({sig_reordered!r}) -- Stata's own signature DID change under the identical "
            f"mutation (golden/datasig_oracle.log MARK_A_REORDER); Q9 requires the engine "
            f"signature to be at least as sensitive as Stata's."
        )
    return True, (f"datasignature landed and passes the reproducibility contract: deterministic "
                   f"on rerun ({sig_base_1!r}) and sensitive to variable reorder ({sig_reordered!r}).")


# ───────── P1-A3-REVORACLE-SCAFFOLD — reverse-oracle harness, legs 2+3 (writer ARC 3) ─────────
# "Stata as TRANSPORT, not SIGNER" (internal note P1-A3-WRITER-IMPL-brief.md Sec2.2): with no cobra
# `export-dta` writer yet (P1-A3-WRITER-NUMERIC lands it), the transport-identity property is
# still provable in full by round-tripping an EXISTING golden .dta through Stata's own
# `use`+`save` (golden/f16_tw_transport_oracle.do/.log) and reading BOTH the original and the
# round-tripped file with our own `import-dta` -- zero writer code exercised. Truth-source
# capture, StataBE19.5, `describe, fullnames` (golden/f16_tw_transport_oracle.log): wk =
# float/%tw, y = float/%9.0g, IDENTICAL on both the original and the round-tripped file.
def p1a3_transport_leg2_identity(ctx):
    """Leg (2) transport identity (brief Sec2.3 row 2): `datasignature` computed by cobra's OWN
    `import-dta` must be BYTE-IDENTICAL between golden/f16_tw_fixture.dta and its StataBE
    use+save round-trip golden/f16_tw_fixture_roundtrip.dta. This is ground truth, not a
    self-confirming mock (brief Sec2.2): a shared reader/writer bug could make
    read(write(X))==X pass, but it cannot make a REAL, independent StataBE accept the file and
    re-emit it -- the middle hop is a foreign implementation we do not control. Comparator
    sharpness measured live at authoring time (both arms on the SAME binary,
    build/cobra 0.34.3): un-mutated pair ->
    orig=rtrip='4:2(2393558379):4284342262:1582813414' (GREEN); patching ONE cell of `wk`
    (`setcol --var wk --values <3121,3121,3122,3123>`, i.e. obs 1 changed from 3120->3121) ->
    mutated signature '4:2(2393558379):3586241278:2770830045' != orig (RED) -- the comparator
    fires on a single-cell divergence, confirming it is not vacuously true."""
    orig_hbin = _tmp("p1a3_transport_leg2_orig")
    rtrip_hbin = _tmp("p1a3_transport_leg2_roundtrip")
    r1 = ctx.run_cobra("import-dta", ["--data", "{repo:golden/f16_tw_fixture.dta}",
                                       "--out", orig_hbin], tidy=False)
    if r1["rc"] != 0:
        return False, f"import-dta(original) failed rc={r1['rc']} err={r1['err'][:200]}"
    r2 = ctx.run_cobra("import-dta", ["--data", "{repo:golden/f16_tw_fixture_roundtrip.dta}",
                                       "--out", rtrip_hbin], tidy=False)
    if r2["rc"] != 0:
        return False, f"import-dta(roundtrip) failed rc={r2['rc']} err={r2['err'][:200]}"
    s1 = ctx.run_cobra("datasignature", ["--data", orig_hbin], tidy=False)
    s2 = ctx.run_cobra("datasignature", ["--data", rtrip_hbin], tidy=False)
    if s1["rc"] != 0 or s2["rc"] != 0:
        return False, f"datasignature failed rc={s1['rc']}/{s2['rc']} err={(s1['err'] or s2['err'])[:200]}"
    sig1, sig2 = s1["out"].strip(), s2["out"].strip()
    if sig1 != sig2:
        return False, f"transport identity broken: sig(orig)={sig1!r} != sig(roundtrip)={sig2!r}"
    return True, f"transport identity holds across StataBE use+save: both signatures = {sig1!r}"


def p1a3_transport_leg3_metadata(ctx):
    """Leg (3) Stata metadata (brief Sec2.3 row 3, NOT redundant with leg 2 -- `format` is
    deliberately excluded from `datasignature`, datasignature.cpp:89-93, so a metadata
    regression is invisible to leg 2/leg 1). Checks cobra's own `import-dta --out-meta` JSON
    for `wk`/`y`'s `stata_type`+`format` (a) match EACH OTHER across the StataBE round-trip and
    (b) match the StataBE `describe, fullnames` truth-source captured in
    golden/f16_tw_transport_oracle.log (wk=float/%tw, y=float/%9.0g on BOTH files) -- so a
    drift either between our two reads or against Stata's own authoritative describe both fire
    RED. Comparator sharpness verified at authoring time by mutating a copy of the parsed JSON
    (e.g. wk.format '%tw'->'%td'): the `want` dict equality check fires on that single field,
    same mechanism this function runs live -- one only needs to see the diff to know the
    per-field loop below (not a coarse whole-object compare) would reject it."""
    orig_hbin = _tmp("p1a3_transport_leg3_orig")
    rtrip_hbin = _tmp("p1a3_transport_leg3_roundtrip")
    orig_meta = _tmp("p1a3_transport_leg3_orig_meta").replace(".hbin", ".json")
    rtrip_meta = _tmp("p1a3_transport_leg3_roundtrip_meta").replace(".hbin", ".json")
    r1 = ctx.run_cobra("import-dta", ["--data", "{repo:golden/f16_tw_fixture.dta}",
                                       "--out", orig_hbin, "--out-meta", orig_meta], tidy=False)
    if r1["rc"] != 0:
        return False, f"import-dta(original) failed rc={r1['rc']} err={r1['err'][:200]}"
    r2 = ctx.run_cobra("import-dta", ["--data", "{repo:golden/f16_tw_fixture_roundtrip.dta}",
                                       "--out", rtrip_hbin, "--out-meta", rtrip_meta], tidy=False)
    if r2["rc"] != 0:
        return False, f"import-dta(roundtrip) failed rc={r2['rc']} err={r2['err'][:200]}"
    try:
        m1 = json.loads(Path(orig_meta).read_text())
        m2 = json.loads(Path(rtrip_meta).read_text())
    except (OSError, json.JSONDecodeError) as e:
        return False, f"--out-meta sidecar unreadable: {e}"

    want = {"wk": {"stata_type": "float", "format": "%tw"},
            "y": {"stata_type": "float", "format": "%9.0g"}}
    v1 = {v["name"]: v for v in m1.get("variables", [])}
    v2 = {v["name"]: v for v in m2.get("variables", [])}
    bad = []
    for name, exp in want.items():
        for field, want_val in exp.items():
            got1 = v1.get(name, {}).get(field)
            got2 = v2.get(name, {}).get(field)
            if got1 != want_val:
                bad.append(f"{name}.{field}(orig)={got1!r} want {want_val!r} (StataBE describe truth)")
            if got2 != want_val:
                bad.append(f"{name}.{field}(roundtrip)={got2!r} want {want_val!r} (StataBE describe truth)")
    if bad:
        return False, "; ".join(bad)
    return True, ("leg-3 metadata transport identity holds: wk/y stata_type+format match both "
                  "each other and the StataBE describe truth-source across the round-trip "
                  "(golden/f16_tw_transport_oracle.log)")


# ───────── EVAL-P1A3-WRITER-GATE1 — export-dta round-trip gate①, committed harness ─────────
# Promotes today's shell-verified 16/16 (P1-A3-WRITER-NUMERIC stint, 2026-07-30) into a
# permanent eval cell (brief Sec2.3 designs gate① at every-commit cadence -- an uncommitted
# gate rots). TWO arms are mandatory, not one, per that same stint's own finding: a
# `--in-meta` arm alone never calls the narrow() ladder when the declared type is already
# lossless, so the f32 guard is structurally unreachable and dead code on that arm alone.
_GATE1_FIXTURES = [
    ("hettest_input", "data/hettest_input.dta"),
    ("heckman_input", "data/heckman_input.dta"),
    ("reg3_input", "data/reg3_input.dta"),
    ("neweyprais_input", "data/neweyprais_input.dta"),
    ("recon_prepped", "data/recon_prepped.dta"),
    ("tsma_golden", "data/tsma_golden.dta"),
    ("f16_tw_fixture", "golden/f16_tw_fixture.dta"),
    ("tc_datetime_fixture", "golden/tc_datetime_fixture.dta"),
    # EVAL-P1A3-WRITER-GATE1-FACTORS -- the 8 fixtures above are all-numeric (0 factor
    # columns each, confirmed live 2026-07-30), so str#/value-label writer paths
    # (P1-A3-WRITER-STRINGS-IMPL, ARC 3 step 4) were structurally untested by gate① until
    # these 4. Each isolates one edge named in dta_writer.h's FACTOR ASYMMETRY note / str#
    # width policy: "" coexisting with non-empty levels · every level "" (width-floor,
    # str0-forbidden path) · multi-byte UTF-8 (width = dictionary's longest value in BYTES,
    # not characters) · value-label + variable-label together (Stata auto/auto2 shape).
    ("factor_emptylevel_fixture", "golden/factor_emptylevel_fixture.dta"),
    ("factor_allempty_fixture", "golden/factor_allempty_fixture.dta"),
    ("factor_utf8_fixture", "golden/factor_utf8_fixture.dta"),
    ("factor_labeled_fixture", "golden/factor_labeled_fixture.dta"),
]


def _gate1_roundtrip(ctx, use_meta):
    """Shared driver for both arms: import(X, --out-meta) -> export-dta[--in-meta] ->
    import -> compare datasignature against import(X) directly. `use_meta` selects the
    --out-meta/--in-meta arm; False is the auto (no sidecar) arm that actually exercises the
    narrow() ladder."""
    fails = []
    oks = []
    for name, relpath in _GATE1_FIXTURES:
        src = "{repo:%s}" % relpath
        orig = _tmp(f"gate1_{name}_orig")
        meta = _tmp(f"gate1_{name}_meta").replace(".hbin", ".json")
        r0 = ctx.run_cobra("import-dta", ["--data", src, "--out", orig, "--out-meta", meta],
                            tidy=False)
        if r0["rc"] != 0:
            fails.append(f"{name}: import-dta failed rc={r0['rc']} {r0['err'][:150]}")
            continue
        s0 = ctx.run_cobra("datasignature", ["--data", orig], tidy=False)
        if s0["rc"] != 0:
            fails.append(f"{name}: datasignature(orig) failed rc={s0['rc']}")
            continue
        sig0 = s0["out"].strip()

        rt_dta = _tmp(f"gate1_{name}_rt").replace(".hbin", ".dta")
        export_args = [orig, rt_dta] + (["--in-meta", meta] if use_meta else [])
        re = ctx.run_cobra("export-dta", export_args, tidy=False)
        if re["rc"] != 0:
            fails.append(f"{name}: export-dta failed rc={re['rc']} {re['err'][:150]}")
            continue
        rt_hbin = _tmp(f"gate1_{name}_rt2")
        ri = ctx.run_cobra("import-dta", ["--data", rt_dta, "--out", rt_hbin], tidy=False)
        if ri["rc"] != 0:
            fails.append(f"{name}: import-dta(roundtrip) failed rc={ri['rc']}")
            continue
        s1 = ctx.run_cobra("datasignature", ["--data", rt_hbin], tidy=False)
        if s1["rc"] != 0:
            fails.append(f"{name}: datasignature(roundtrip) failed rc={s1['rc']}")
            continue
        sig1 = s1["out"].strip()
        if sig0 != sig1:
            fails.append(f"{name}: signature drift {sig0!r} -> {sig1!r}")
        else:
            oks.append(f"{name}={sig0!r}")
    if fails:
        return False, "; ".join(fails)
    return True, (f"{len(_GATE1_FIXTURES)}/{len(_GATE1_FIXTURES)} round-trip identity holds: "
                  + ", ".join(oks))


def p1a3_writer_gate1_meta(ctx):
    """Gate① meta arm (brief Sec2.3): datasignature(import(export(import(X, --out-meta),
    --in-meta))) == datasignature(import(X)) for every fixture. `--in-meta` hands the writer
    the reader's own recorded stata_type/format so a lossless declared type is honoured as-is
    (spec Sec3.3 "wide hint wins"). Always expected GREEN -- a RED here means the writer no
    longer round-trips even the easy, type-annotated path. Seeded-RED certificate (this same
    8-fixture corpus, 2026-07-30 P1-A3-WRITER-NUMERIC stint, `fits(Float32)`->`return true`
    scratch build): meta arm stayed 8/8 GREEN even with the guard forced always-on -- confirms
    this arm never reaches narrow() when the declared type is already lossless (dead code on
    this arm ALONE, which is why the auto arm below is non-negotiable, not redundant)."""
    return _gate1_roundtrip(ctx, use_meta=True)


def p1a3_writer_gate1_auto(ctx):
    """Gate① auto arm (brief Sec2.3): same round-trip with NO --out-meta/--in-meta -- the
    writer must infer the narrowest LOSSLESS type per column (spec Sec3.3
    `(double)(float)v == v` guard) with no declared-type hint to lean on. This is the arm
    that actually exercises narrow(). Seeded-RED certificate: the 2026-07-30
    P1-A3-WRITER-NUMERIC stint ran this exact mechanism against a scratch build with the
    guard forced to `return true` (always-narrow) and got exactly 5 GREEN / 3 RED -- RED on
    heckman_input/reg3_input/tc_datetime_fixture, GREEN on the other 5. Reproduced here at
    authoring time by a direct per-column check on this same corpus (no rebuild needed --
    heavy-token budget was held elsewhere at claim time, P=2/one already live): for every
    numeric column, `v.astype(float32).astype(float64) != v` is nonzero ONLY for
    heckman_input/reg3_input/tc_datetime_fixture (each holds at least one float32-INEXACT
    double) and zero for hettest_input/neweyprais_input/recon_prepped/tsma_golden/
    f16_tw_fixture (genuinely float32-exact) -- fixture-for-fixture identical to the stint's
    live-rebuild split, since an always-narrow guard changes the signature iff the column
    actually loses precision. The comparator therefore fires only on real value loss, not
    vacuously.

    EVAL-P1A3-WRITER-GATE1-FACTORS addendum (2026-07-30): the 4 factor fixtures added to
    `_GATE1_FIXTURES` are the FIRST str# coverage this gate has ever had (the original 8 are
    all-numeric, 0 factor columns each, confirmed live via `import-dta`'s own column-count
    line). Live 2026-07-30 measured all 12/12 GREEN on both arms against the current
    (correct) writer -- no RED to report on the real binary. Seeded-RED certificate for the
    str# WIDTH policy specifically (data-level, no scratch rebuild -- same escape hatch as
    the numeric cert above, heavy-token budget not spent on a 2nd compile this stint):
    `import-dta --out-meta`'s JSON sidecar records `stata_type` as the ORIGINAL declared
    Stata width (`str10`/`str5`/`str30` for factor_emptylevel/factor_allempty/factor_utf8
    respectively -- verified live in the sidecar, not the freshly-scanned dictionary width),
    and `--in-meta` honours that declared width verbatim (brief's "wide hint wins" policy,
    same clause the f32 guard uses). So an injected off-by-one bug in the AUTO arm's
    width-from-scan logic (`computed_width - 1`) would corrupt exactly the 3 fixtures whose
    longest level exceeds that shrunk width -- factor_emptylevel (max level 3 bytes,
    "abc"/"xyz" truncate to 2) · factor_allempty (max level 0 bytes, floor(-1) is the
    str0-forbidden crash path itself) · factor_utf8 (max level 24 bytes, truncating by 1
    byte lands mid-codepoint inside the 3-byte-per-char Korean level) -- while the META arm
    stays GREEN on all 3 because it never re-scans, it just replays the declared str10/
    str5/str30 from the sidecar: dead code on that arm, identical structure to the f32
    finding. factor_labeled has no string column (0 factor, value-label-only) so it is
    unaffected by a str#-width bug either way. Predicted split under that injected bug:
    META 12/12 GREEN, AUTO 9/12 GREEN + 3 RED (the 3 named above) -- not independently
    scratch-rebuilt this stint (would need a 2nd heavy token + a revert-and-rebuild pass to
    leave the tree clean; deferred, same class of gap this docstring itself exists to
    prevent from rotting uncommitted)."""
    return _gate1_roundtrip(ctx, use_meta=False)


# ───────── FIX:EVAL-DTA-READER-CHARS-GATE — <characteristics> round-trip, committed ─────────
# FIX:DTA-READER-CHARS-DROP (cobra @bf48e51, unmerged `fix/dta-reader-chars-drop`) closed
# dta_reader.cpp:306's `c.skip(8)` gap -- the reader used to discard the entire
# `<characteristics>` section outright. That stint's manifest was lane:cobra-only (3 files),
# so it could not commit a gate here (manifest escape); shell round-trip was verified live but
# an uncommitted gate rots (EVAL-P1A3-WRITER-GATE1 precedent, same reasoning). Characteristics
# are NOT part of `datasignature` (metadata, not data values -- datasignature.cpp excludes
# them same as `format`), so this compares the `--out-meta` sidecar's `characteristics` array
# directly, byte-for-byte, alongside a `datasignature` check that the chars leg left the
# actual data untouched.
_CHARS_GATE_FIXTURES = [
    ("ts_diag_input", "data/ts_diag_input.dta"),
    ("csdid_input", "data/csdid_input.dta"),
    ("ivdiag_input", "data/ivdiag_input.dta"),
    ("tobit_input", "data/tobit_input.dta"),
]

_DTA_MAGIC = b"<stata_dta>"


def _not_a_dta(relpath):
    """Return an error string if the on-disk fixture at `relpath` (resolved the
    same way `{repo:...}` does -- relative to this file's directory) isn't a real
    Stata .dta (117+/118+ files start with the literal `<stata_dta>` header), else
    None. Fixtures under tests/validate/ are gitignored corpus data with no git
    history to fall back on, and have been observed silently clobbered with raw
    hbin bytes (`CECONHB1` magic) by a `.do` golden script's `save ..., replace`
    line run through PIET/cobra instead of real Stata (FIX:EVAL-CHARSGATE-FIXTURE-
    CLOBBER) -- without this check that shows up as an opaque
    `import-dta failed rc=3 "unsupported legacy format version ..."`, indistinguishable
    from an actual reader regression."""
    path = Path(__file__).resolve().parent / relpath
    try:
        head = path.read_bytes()[:len(_DTA_MAGIC)]
    except OSError as e:
        return f"cannot read fixture {path}: {e}"
    if head != _DTA_MAGIC:
        return (f"fixture {path} is not a real .dta (header {head!r}, expected "
                f"{_DTA_MAGIC!r}) -- corpus fixture likely clobbered by a non-Stata writer")
    return None


def dta_reader_chars_gate1(ctx):
    """FIX:EVAL-DTA-READER-CHARS-GATE: import(X, --out-meta) -> export(--in-meta) ->
    import(--out-meta) round-trip over 4 real-corpus fixtures carrying tsset
    `<characteristics>` (_dta[tis]/_TSitrvl/_TSdelta/_TStvar and similar). Two checks per
    fixture: (1) `characteristics` in the --out-meta sidecar is non-empty on the FIRST import
    -- a silently-empty array is exactly the FIX:DTA-READER-CHARS-DROP regression this gate
    exists to catch, and an empty-vs-empty compare on both sides would otherwise pass
    vacuously; (2) the array is byte-identical (order + varname/charname/value all preserved)
    before vs. after the round-trip, AND `datasignature` is unchanged, confirming the chars
    leg didn't perturb the actual data values. Live 2026-07-30 shell-verified GREEN on the
    real (fixed) binary: ts_diag_input (4 chars) + csdid/ivdiag/tobit_input (6 chars each,
    values up to 21 bytes), all byte-identical, datasignature unchanged
    (73081:7(1899724068):4263137197:1805535364 for csdid_input, etc.). Seeded-RED certificate
    (no scratch rebuild needed -- the CURRENT main-branch binary, pre-fix, IS the negative
    control): `build/cobra import-dta --data tests/eval/data/ts_diag_input.dta --out-meta
    <json>` against `<repo>/build/cobra` (main HEAD, `c.skip(8)` still in place) yields
    `characteristics: []` -- confirmed live -- which trips check (1) above. GREEN requires
    `COBRA_BIN` pointed at a binary built from `fix/dta-reader-chars-drop` (or post-merge
    main)."""
    fails = []
    oks = []
    for name, relpath in _CHARS_GATE_FIXTURES:
        bad_magic = _not_a_dta(relpath)
        if bad_magic:
            fails.append(f"{name}: {bad_magic}")
            continue
        src = "{repo:%s}" % relpath
        orig = _tmp(f"charsgate_{name}_orig")
        meta0 = _tmp(f"charsgate_{name}_meta0").replace(".hbin", ".json")
        r0 = ctx.run_cobra("import-dta", ["--data", src, "--out", orig, "--out-meta", meta0],
                            tidy=False)
        if r0["rc"] != 0:
            fails.append(f"{name}: import-dta failed rc={r0['rc']} {r0['err'][:150]}")
            continue
        s0 = ctx.run_cobra("datasignature", ["--data", orig], tidy=False)
        if s0["rc"] != 0:
            fails.append(f"{name}: datasignature(orig) failed rc={s0['rc']}")
            continue
        sig0 = s0["out"].strip()
        try:
            chars0 = json.loads(Path(meta0).read_text()).get("characteristics", [])
        except Exception as e:
            fails.append(f"{name}: meta0 parse failed {e}")
            continue
        if not chars0:
            fails.append(f"{name}: characteristics EMPTY on first import (reader regressed to "
                          f"the FIX:DTA-READER-CHARS-DROP c.skip(8) bug)")
            continue

        rt_dta = _tmp(f"charsgate_{name}_rt").replace(".hbin", ".dta")
        re = ctx.run_cobra("export-dta", [orig, rt_dta, "--in-meta", meta0], tidy=False)
        if re["rc"] != 0:
            fails.append(f"{name}: export-dta failed rc={re['rc']} {re['err'][:150]}")
            continue

        rt_hbin = _tmp(f"charsgate_{name}_rt2")
        meta1 = _tmp(f"charsgate_{name}_meta1").replace(".hbin", ".json")
        ri = ctx.run_cobra("import-dta", ["--data", rt_dta, "--out", rt_hbin, "--out-meta", meta1],
                            tidy=False)
        if ri["rc"] != 0:
            fails.append(f"{name}: import-dta(roundtrip) failed rc={ri['rc']}")
            continue
        s1 = ctx.run_cobra("datasignature", ["--data", rt_hbin], tidy=False)
        if s1["rc"] != 0:
            fails.append(f"{name}: datasignature(roundtrip) failed rc={s1['rc']}")
            continue
        sig1 = s1["out"].strip()
        try:
            chars1 = json.loads(Path(meta1).read_text()).get("characteristics", [])
        except Exception as e:
            fails.append(f"{name}: meta1 parse failed {e}")
            continue

        if sig0 != sig1:
            fails.append(f"{name}: datasignature drift {sig0!r} -> {sig1!r}")
        elif chars0 != chars1:
            fails.append(f"{name}: characteristics drift {len(chars0)} -> {len(chars1)} entries "
                          f"(before={chars0!r} after={chars1!r})")
        else:
            oks.append(f"{name}={len(chars0)} chars, sig={sig0!r}")
    if fails:
        return False, "; ".join(fails)
    return True, (f"{len(_CHARS_GATE_FIXTURES)}/{len(_CHARS_GATE_FIXTURES)} characteristics "
                  "round-trip identity holds: " + ", ".join(oks))


def pca_cov_no_sds(ctx):
    """`pca ..., covariance` posts NO e(sds) (Stata posts sds only for a correlation
    PCA; live `capture confirm matrix e(sds)` -> rc 111, asserted in
    golden/aer_estim_oracle.do and echoed there as INFO|pca_cov|confirm_matrix_e_sds_rc|111).
    cobra emits `sds` only for correlation PCA (cmds_aer_estim.cpp:349). Absence cannot be a
    float pin (AUTHORING.md cardinal rule)."""
    j = ctx.result
    if not isinstance(j, dict):
        return False, "no JSON from cobra pca"
    if "sds" in j:
        return False, f"sds present ({j['sds']!r}) -- Stata posts no e(sds) for covariance PCA"
    if j.get("ctype") != "covariance":
        return False, f"ctype={j.get('ctype')!r}, expected 'covariance'"
    return True, "sds absent and ctype == covariance, as Stata"



# ───────── B4/W7 — ivreg2 `savefirst`: the stored-estimate NAMING rule ─────────
# The numbers of the saved first stage are gated by the golden checks of case
# B4_savefirst_ivreg2 (and cell-for-cell by ctest b4_savefirst). What no numeric
# check can reach is the NAME ivreg2 mints and whether it stores the equation at
# all — and that is what a consumer registers, so a wrong name is a silently
# missing stored estimate, not a wrong number.
#
# Every expectation below is a cell of tests/eval/golden/aer_b4/ivreg2-savefirst.log:
#   arm A  savefprefix(fs_)                 -> e(firsteqs) = "fs_mpg fs_turn"
#   arm B  no prefix, endogenous `L.mpg`    -> e(firsteqs) = "_ivreg2_L_mpg"
#          (the default prefix is `_ivreg2_`, and ivreg2's
#           `subinstr "." "_"` fires on the time-series operator's dot)
#   arm G  a 30-char prefix + "mpg" = 33    -> e(firsteqs) EMPTY and `estimates
#          restore` is r(111): the `est store` step is `capture`d, so an
#          over-length name is SILENTLY not stored.
def b4_savefirst_eqnames(ctx):
    base = ["--data", ctx.data("auto"), "--y", "price", "--x", "weight",
            "--instruments", "headroom,trunk", "--vcov", "hc1", "--savefirst"]
    fails = []

    def firsteqs(args, tag):
        r = ctx.run_cobra("iv", args, tidy=False)
        if r["rc"] != 0 or r["json"] is None:
            fails.append(f"{tag}: rc={r['rc']} err={r['err'][:160]}")
            return None, None
        return r["json"].get("firsteqs"), r["json"].get("first_stage") or {}

    # arm A — two endogenous regressors, an explicit prefix, and ORDER (ivreg2
    # emits them in the endogenous varlist's order, not alphabetically).
    eqs, fs = firsteqs(["--data", ctx.data("auto"), "--y", "price", "--x", "weight",
                        "--endog", "mpg,turn", "--instruments", "headroom,trunk,length",
                        "--vcov", "hc1", "--savefirst", "--savefprefix", "fs_"], "armA")
    if eqs is not None and eqs != ["fs_mpg", "fs_turn"]:
        fails.append(f"armA firsteqs={eqs!r} want ['fs_mpg', 'fs_turn']")

    # arm B — the DEFAULT prefix.
    eqs, fs = firsteqs(base + ["--endog", "mpg"], "armB-default-prefix")
    if eqs is not None and eqs != ["_ivreg2_mpg"]:
        fails.append(f"armB firsteqs={eqs!r} want ['_ivreg2_mpg'] (default prefix _ivreg2_)")

    # arm G — 30-char prefix + "mpg" = 33 > 32: not stored, and absent from firsteqs.
    long_prefix = "abcdefghijklmnopqrstuvwxyzabcd"
    eqs, fs = firsteqs(base + ["--endog", "mpg", "--savefprefix", long_prefix], "armG")
    if eqs is not None:
        if eqs != []:
            fails.append(f"armG firsteqs={eqs!r} want [] (a 33-char name is silently NOT stored)")
        ent = fs.get("mpg") or {}
        if ent.get("stored") is not False:
            fails.append(f"armG first_stage.mpg.stored={ent.get('stored')!r} want False")
        if ent.get("eqname") != long_prefix + "mpg":
            fails.append(f"armG eqname={ent.get('eqname')!r} want {long_prefix + 'mpg'!r}")

    if fails:
        return False, " | ".join(fails)
    return True, "fs_mpg/fs_turn, _ivreg2_mpg, and the 33-char name silently unstored"


# ───────── B4/W7 — the first stage is INVARIANT to the parent's `small` ────────
# (help ivreg2) states that the first-stage results are always reported with
# small-sample statistics (paraphrased). The log's arm F measures it as
# fs_mreldif_V_small_vs_not = +0.0000000000000X-3ff (EXACTLY zero) while the
# PARENT's own V moves by 1.0422 — both halves are asserted here, because a probe
# that did not also move the parent would prove nothing.
def b4_savefirst_small_invariant(ctx):
    def fit(extra):
        args = ["--data", ctx.data("auto"), "--y", "price", "--x", "weight",
                "--endog", "mpg", "--instruments", "headroom,trunk", "--vcov", "hc1",
                "--savefirst"] + extra
        r = ctx.run_cobra("iv", args, tidy=False)
        if r["rc"] != 0 or r["json"] is None:
            raise RuntimeError(f"cobra iv failed rc={r['rc']} err={r['err'][:160]}")
        return r["json"]
    a, b = fit([]), fit(["--small"])
    fa = a["first_stage"]["mpg"]["se"]
    fb = b["first_stage"]["mpg"]["se"]
    if fa != fb:
        return False, f"first-stage SEs MOVED with --small: {fa} vs {fb}"
    # ... and the parent DID move, so the comparison above is not vacuous.
    pa, pb = a["se"]["mpg"], b["se"]["mpg"]
    ratio = (pb / pa) ** 2
    want = float.fromhex("0x1.0ad12073615a2p+0")  # arm F null_parent_ratio_V11, %21x
    if abs(ratio - want) / want > 1e-6:
        return False, (f"parent V11 ratio small-vs-not = {ratio!r}, want {want!r} "
                       "(arm F null_parent_ratio_V11)")
    return True, f"first stage identical under --small; parent V11 moved x{ratio:.6f}"


PROPERTIES = {
    "summarize_allmiss_missing": summarize_allmiss_missing,
    "summarize_string_var_honest": summarize_string_var_honest,
    "tabstat_singleton_missing": tabstat_singleton_missing,
    "egen_group_alpha_order": egen_group_alpha_order,
    "collapse_first_last_raw": collapse_first_last_raw,
    "collapse_firstnm_lastnm": collapse_firstnm_lastnm,
    "hbin_byte_identical_rerun": hbin_byte_identical_rerun,
    "sort_permutation_identity": sort_permutation_identity,
    "collinearity_omitted_set_equals": collinearity_omitted_set_equals,
    "l1m3_rename_multipair_identity": l1m3_rename_multipair_identity,
    "l1m3_clonevar_factor_append": l1m3_clonevar_factor_append,
    "l1m3_encode_alpha_missing": l1m3_encode_alpha_missing,
    "l1m3_setcol_f64_patch": l1m3_setcol_f64_patch,
    "l1m3_setcol_factor_retype": l1m3_setcol_factor_retype,
    "l1m3_collapse_multiby_label_integrity": l1m3_collapse_multiby_label_integrity,
    "l1b_joinby_mm_fanout": l1b_joinby_mm_fanout,
    "l1b_joinby_unmatched_dropped": l1b_joinby_unmatched_dropped,
    "l1b_joinby_factor_key_levels": l1b_joinby_factor_key_levels,
    "l1b_joinby_column_union": l1b_joinby_column_union,
    "f16_tw_value": f16_tw_value,
    "p1a3_datasignature_oracle": p1a3_datasignature_oracle,
    "p1a3_transport_leg2_identity": p1a3_transport_leg2_identity,
    "p1a3_transport_leg3_metadata": p1a3_transport_leg3_metadata,
    "p1a3_writer_gate1_meta": p1a3_writer_gate1_meta,
    "p1a3_writer_gate1_auto": p1a3_writer_gate1_auto,
    "dta_reader_chars_gate1": dta_reader_chars_gate1,
    "pca_cov_no_sds": pca_cov_no_sds,
    "b4_savefirst_eqnames": b4_savefirst_eqnames,
    "b4_savefirst_small_invariant": b4_savefirst_small_invariant,
}
