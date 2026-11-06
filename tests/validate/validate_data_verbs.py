#!/usr/bin/env python3
"""MI-066 differential harness: cobra's native data verbs vs the pandas oracle
PIET's Python backend actually runs today.

Oracle mapping (read-only references, never executed by this script beyond
import — the point is independence: this script re-derives expected output
from pandas directly, not from PIET's code, so a bug shared between PIET's
backend and this script would still be caught... except where the SEMANTICS
genuinely are "whatever pandas does", in which case matching pandas IS the
spec):
  merge      -> piet/backend/piet/merge_op.py           (pandas .merge(how="outer"))
  reshape    -> piet/backend/piet/reshape.py             (pandas melt/pivot_table)
  append     -> piet/backend/piet/append.py              (hand-rolled concat, matches
                                                            pandas concat semantics)
  duplicates -> piet/backend/piet/duplicates.py           (hand-rolled group-count)
  gsort      -> piet/backend/piet/gsort.py + editor.py    (np.lexsort)
  order      -> piet/backend/piet/order.py                (column reorder, pure Python)
  recode     -> piet/backend/piet/recode.py + editor.py   (hand-rolled rule application)
  .dta read  -> piet/backend/piet/api/routes_data.py       (pandas.read_stata(...,
               _convert_to_hbin)                           convert_categoricals=False))

Run: <venv-python> validate_data_verbs.py <path-to-cobra-binary>
Requires a Python venv with pandas/numpy (for example piet/.venv/bin/python).

Every check prints ok/FAIL; exits 1 if any FAIL. This is a LOCAL differential
tool (not wired into ctest — it needs the piet venv, which cobra's own build
does not depend on); Eval re-runs it independently as part of the MINOR gate.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
import pandas as pd

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
WORKDIR = tempfile.mkdtemp(prefix="cobra_data_verbs_")
fails = []
n_checks = 0


def chk(label, cond, detail=""):
    global n_checks
    n_checks += 1
    print(f"  {'ok ' if cond else 'FAIL'} {label}{('  ' + detail) if detail else ''}")
    if not cond:
        fails.append(label)


def run_cobra(args):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True, cwd=WORKDIR)
    if r.returncode != 0:
        raise RuntimeError(f"cobra {' '.join(args)} failed (exit {r.returncode}): {r.stderr[:500]}")
    return r


def read_hbin(path):
    with open(path, "rb") as f:
        raw = f.read()
    assert raw[:8] == b"CECONHB1"
    (hlen,) = struct.unpack_from("<Q", raw, 8)
    header = json.loads(raw[16:16 + hlen].decode("utf-8"))
    align = 64
    data_start = ((16 + hlen) + align - 1) // align * align
    n = header["n_rows"]
    out = {}
    for c in header["columns"]:
        off = data_start + c["offset"]
        if c["dtype"] == "f64":
            out[c["name"]] = np.frombuffer(raw, dtype="<f8", count=n, offset=off).copy()
        else:
            codes = np.frombuffer(raw, dtype="<i4", count=n, offset=off).copy()
            levels = c.get("levels", [])
            out[c["name"]] = np.array([levels[i] if i >= 0 else None for i in codes], dtype=object)
    return n, out


def hbin_to_df(path):
    n, cols = read_hbin(path)
    return pd.DataFrame(cols)


def df_to_hbin_via_csv(df, hbin_path, factor_cols=None):
    csv_path = hbin_path.replace(".hbin", "_src.csv")
    df.to_csv(csv_path, index=False, na_rep="")
    args = ["convert", csv_path, hbin_path]
    if factor_cols:
        args += ["--factor", ",".join(factor_cols)]
    run_cobra(args)


def approx_equal_frames(cobra_df, oracle_df, key_cols, value_tol=1e-9, label=""):
    """Compare two DataFrames row-set-wise (order-independent) after sorting
    by key_cols, since our verbs don't always pin the exact same row order
    pandas would produce (documented per-verb; PIET's contract does not pin
    row order for these verbs — the frontend re-sorts/re-displays)."""
    if set(cobra_df.columns) != set(oracle_df.columns):
        chk(f"{label}: column set", False,
            f"cobra={sorted(cobra_df.columns)} oracle={sorted(oracle_df.columns)}")
        return
    if len(cobra_df) != len(oracle_df):
        chk(f"{label}: row count", False, f"cobra={len(cobra_df)} oracle={len(oracle_df)}")
        return
    cols = sorted(cobra_df.columns)
    c_sorted = cobra_df.sort_values(by=key_cols).reset_index(drop=True)[cols]
    o_sorted = oracle_df.sort_values(by=key_cols).reset_index(drop=True)[cols]
    all_ok = True
    for col in cols:
        cv = c_sorted[col].to_numpy()
        ov = o_sorted[col].to_numpy()
        if cv.dtype.kind in "fc" or ov.dtype.kind in "fc":
            cv = cv.astype(float)
            ov = ov.astype(float)
            ok = np.allclose(cv, ov, atol=value_tol, rtol=value_tol, equal_nan=True)
        else:
            ok = all((a == b) or (pd.isna(a) and pd.isna(b)) for a, b in zip(cv, ov))
        if not ok:
            all_ok = False
            chk(f"{label}: column '{col}' values", False)
    if all_ok:
        chk(f"{label}: all columns match ({len(cols)} cols, {len(cobra_df)} rows)", True)


# ===========================================================================
# gsort
# ===========================================================================
def test_gsort():
    print("gsort:")
    rng = np.random.default_rng(42)
    n = 200
    df = pd.DataFrame({"g": rng.integers(0, 5, n).astype(float),
                       "v": rng.normal(size=n),
                       "w": rng.integers(-3, 3, n).astype(float)})
    hbin = os.path.join(WORKDIR, "gsort_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    out = os.path.join(WORKDIR, "gsort_out.hbin")
    run_cobra(["gsort", "--data", hbin, "--vars", "g,-v", "--out", out])
    cobra_df = hbin_to_df(out)

    # oracle: exactly PIET's gsort.py -> editor.run_sort via np.lexsort,
    # ascending g, descending v (last key = primary in lexsort => reversed order)
    idx = np.lexsort((-df["v"].to_numpy(), df["g"].to_numpy()))
    oracle_df = df.iloc[idx].reset_index(drop=True)

    ok = True
    for col in ["g", "v", "w"]:
        if not np.allclose(cobra_df[col].to_numpy(), oracle_df[col].to_numpy(), atol=1e-9):
            ok = False
    chk("gsort g,-v matches np.lexsort oracle row-for-row (order preserved)", ok)


def test_gsort_missing():
    """0.18.0 Eval gate item 2: DESCENDING gsort over missing values must put
    NaN LAST — the np.lexsort behavior of the contracted oracle (editor.run_sort
    negates values for a descending key; -NaN is still NaN, which numpy sorts to
    the end). This diverges from Stata's own gsort (missing = +inf, FIRST under
    descending) — maintainer-ruled: the frozen 1.16.0 oracle is the parity law;
    Stata-alignment is a divergence-ledger item (contract §22)."""
    print("gsort (missing values):")
    rng = np.random.default_rng(7)
    n = 100
    v = rng.normal(size=n)
    v[rng.choice(n, size=15, replace=False)] = np.nan
    g = rng.integers(0, 4, n).astype(float)
    df = pd.DataFrame({"g": g, "v": v, "row": np.arange(n, dtype=float)})
    hbin = os.path.join(WORKDIR, "gsort_na_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    # single descending key
    out = os.path.join(WORKDIR, "gsort_na_desc.hbin")
    run_cobra(["gsort", "--data", hbin, "--vars", "-v", "--out", out])
    cobra_df = hbin_to_df(out)
    idx = np.lexsort((-df["v"].to_numpy(),))
    oracle_df = df.iloc[idx].reset_index(drop=True)
    ok = all(np.allclose(cobra_df[c].to_numpy(), oracle_df[c].to_numpy(), atol=1e-9, equal_nan=True)
             for c in ["g", "v", "row"])
    chk("gsort -v with NaNs matches np.lexsort (NaN LAST under descending)", ok)
    chk("gsort -v: all NaN rows are at the tail", bool(np.all(np.isnan(cobra_df["v"].to_numpy()[-15:]))))

    # multi-key: ascending g primary, descending v secondary with NaNs
    out2 = os.path.join(WORKDIR, "gsort_na_multi.hbin")
    run_cobra(["gsort", "--data", hbin, "--vars", "g,-v", "--out", out2])
    cobra_df2 = hbin_to_df(out2)
    idx2 = np.lexsort((-df["v"].to_numpy(), df["g"].to_numpy()))
    oracle_df2 = df.iloc[idx2].reset_index(drop=True)
    ok2 = all(np.allclose(cobra_df2[c].to_numpy(), oracle_df2[c].to_numpy(), atol=1e-9, equal_nan=True)
              for c in ["g", "v", "row"])
    chk("gsort g,-v with NaNs matches np.lexsort row-for-row (NaN last within group)", ok2)


# ===========================================================================
# order
# ===========================================================================
def test_order():
    print("order:")
    df = pd.DataFrame({"a": [1], "b": [2], "c": [3], "d": [4], "e": [5]})
    hbin = os.path.join(WORKDIR, "order_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    out = os.path.join(WORKDIR, "order_out.hbin")
    run_cobra(["order", "--data", hbin, "--vars", "d,b", "--after", "a", "--out", out])
    _, cols = read_hbin(out)
    order = [k for k in cols.keys() if not k.endswith("__levels")]
    # oracle (order.py semantics): remaining = [b,c,e] with d,b removed -> [c,e]
    # after 'a': insert [d,b] block after a -> [a, d, b, c, e]
    chk("order --after(a) matches PIET order.py semantics", order == ["a", "d", "b", "c", "e"],
        f"got {order}")


# ===========================================================================
# append
# ===========================================================================
def test_append():
    print("append:")
    rng = np.random.default_rng(1)
    m = pd.DataFrame({"id": np.arange(50, dtype=float), "x": rng.normal(size=50)})
    u = pd.DataFrame({"id": np.arange(30, 80, dtype=float), "y": rng.normal(size=50)})
    m_hbin = os.path.join(WORKDIR, "append_m.hbin")
    u_hbin = os.path.join(WORKDIR, "append_u.hbin")
    df_to_hbin_via_csv(m, m_hbin)
    df_to_hbin_via_csv(u, u_hbin)

    out = os.path.join(WORKDIR, "append_out.hbin")
    run_cobra(["append", "--data", m_hbin, "--using", u_hbin, "--out", out])
    cobra_df = hbin_to_df(out)

    # oracle: pandas concat with column union, matching append.py's documented semantics
    oracle_df = pd.concat([m, u], ignore_index=True, sort=False)
    chk("append row count matches n_master+n_using", len(cobra_df) == len(m) + len(u))
    chk("append column union matches", set(cobra_df.columns) == set(oracle_df.columns))
    ok = (np.allclose(cobra_df["x"].to_numpy()[:50], m["x"].to_numpy(), atol=1e-9) and
         np.all(np.isnan(cobra_df["x"].to_numpy()[50:])) and
         np.allclose(cobra_df["y"].to_numpy()[50:], u["y"].to_numpy(), atol=1e-9) and
         np.all(np.isnan(cobra_df["y"].to_numpy()[:20])))
    chk("append values + missing-fill pattern matches oracle", ok)


# ===========================================================================
# duplicates
# ===========================================================================
def test_duplicates():
    print("duplicates:")
    rng = np.random.default_rng(2)
    keys = rng.integers(0, 20, 100)
    df = pd.DataFrame({"k": keys.astype(float), "v": rng.normal(size=100)})
    hbin = os.path.join(WORKDIR, "dup_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    # report
    r = run_cobra(["duplicates", "report", "--data", hbin, "--vars", "k"])
    result = json.loads(r.stdout)
    oracle_surplus = int(len(df) - df["k"].nunique())
    chk("duplicates report total_surplus matches n - nunique", result["total_surplus"] == oracle_surplus,
        f"got {result['total_surplus']} want {oracle_surplus}")

    # drop
    out = os.path.join(WORKDIR, "dup_drop.hbin")
    run_cobra(["duplicates", "drop", "--data", hbin, "--vars", "k", "--out", out])
    cobra_df = hbin_to_df(out)
    oracle_df = df.drop_duplicates(subset=["k"], keep="first").reset_index(drop=True)
    chk("duplicates drop row count matches pandas drop_duplicates(keep='first')",
        len(cobra_df) == len(oracle_df))
    chk("duplicates drop keeps identical key set", set(cobra_df["k"]) == set(oracle_df["k"]))

    # tag
    out2 = os.path.join(WORKDIR, "dup_tag.hbin")
    run_cobra(["duplicates", "tag", "--data", hbin, "--vars", "k", "--gen", "dtag", "--out", out2])
    cobra_df2 = hbin_to_df(out2)
    oracle_tag = df.groupby("k")["k"].transform("count").to_numpy() - 1
    chk("duplicates tag == group_size-1 matches pandas groupby transform",
        np.allclose(cobra_df2["dtag"].to_numpy(), oracle_tag))


# ===========================================================================
# recode
# ===========================================================================
def test_recode():
    print("recode:")
    df = pd.DataFrame({"rep78": [1, 2, 3, 4, 5, np.nan, 3, 1]})
    hbin = os.path.join(WORKDIR, "recode_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    out = os.path.join(WORKDIR, "recode_out.hbin")
    run_cobra(["recode", "--data", hbin, "--var", "rep78", "--rules",
              "(1 2=1)(3 4=2)(5=3)(missing=9)", "--gen", "rep3", "--out", out])
    cobra_df = hbin_to_df(out)

    mapping = {1: 1, 2: 1, 3: 2, 4: 2, 5: 3}
    oracle = df["rep78"].map(lambda v: 9 if pd.isna(v) else mapping.get(int(v)))
    chk("recode value-list + missing rule matches hand-computed oracle",
        np.allclose(cobra_df["rep3"].to_numpy(), oracle.to_numpy(dtype=float)))

    # range rule
    df2 = pd.DataFrame({"v": [1, 5, 9, 10, 15, 20]})
    hbin2 = os.path.join(WORKDIR, "recode_in2.hbin")
    df_to_hbin_via_csv(df2, hbin2)
    out2 = os.path.join(WORKDIR, "recode_out2.hbin")
    run_cobra(["recode", "--data", hbin2, "--var", "v", "--rules", "(1/9=100)(else=999)",
              "--gen", "vr", "--out", out2])
    cobra_df2 = hbin_to_df(out2)
    oracle2 = df2["v"].apply(lambda x: 100 if 1 <= x <= 9 else 999)
    chk("recode range + else matches oracle", np.allclose(cobra_df2["vr"].to_numpy(), oracle2.to_numpy(dtype=float)))


# ===========================================================================
# reshape
# ===========================================================================
def test_reshape():
    print("reshape:")
    df = pd.DataFrame({"id": [1, 2, 3], "inc80": [100, 200, 300], "inc81": [110, 210, 310],
                       "inc82": [120, 220, 320]})
    hbin = os.path.join(WORKDIR, "reshape_in.hbin")
    df_to_hbin_via_csv(df, hbin)

    out = os.path.join(WORKDIR, "reshape_long_out.hbin")
    run_cobra(["reshape", "long", "--data", hbin, "--stubs", "inc", "--i", "id", "--j", "year", "--out", out])
    cobra_long = hbin_to_df(out)

    # oracle: pandas melt/wide_to_long semantics per reshape.py
    oracle_long = pd.wide_to_long(df, stubnames="inc", i="id", j="year").reset_index()
    approx_equal_frames(cobra_long, oracle_long, key_cols=["id", "year"], label="reshape long vs wide_to_long")

    out2 = os.path.join(WORKDIR, "reshape_wide_out.hbin")
    run_cobra(["reshape", "wide", "--data", out, "--stubs", "inc", "--i", "id", "--j", "year", "--out", out2])
    cobra_wide = hbin_to_df(out2)
    approx_equal_frames(cobra_wide, df.astype(float), key_cols=["id"], label="reshape long->wide round-trip")


def _oracle_reshape_wide(df_long, stubs, i_vars, j_var):
    """Re-derives PIET reshape.py's run_reshape_wide() pandas algorithm
    exactly (pivot_table(aggfunc="first") per stub, outer-merged on i_vars,
    other_cols left-merged) — independent re-implementation from the
    documented semantics, not an import of reshape.py itself, so a shared
    bug between reshape.py and this oracle would still be structurally
    unlikely (both must independently reflect what pivot_table actually
    does). This is the exact mechanism behind the 0.18.1 divergence: per-stub
    pivot_table defaults to dropna=True, so an i-group with EVERY value
    missing for one stub is silently dropped from THAT stub's pivot — but
    can be resurrected via the outer-merge if another stub has data for it.
    ROW ORDER matters here (unlike the row-order-insensitive checks
    elsewhere in this file) because ascending-sort-by-i_vars vs
    first-occurrence order is itself one of the divergences under test."""
    j_col = df_long[j_var].dropna()
    j_vals_raw = sorted(j_col.unique())

    def _j_suffix(v):
        return str(int(v)) if float(v) == int(float(v)) else str(v)

    all_names = list(df_long.columns)
    other_cols = [c for c in all_names if c not in i_vars and c != j_var and c not in stubs]
    frames = []
    for stub in stubs:
        pivot = df_long.pivot_table(index=i_vars, columns=j_var, values=stub, aggfunc="first")
        pivot.columns = [f"{stub}{_j_suffix(c)}" for c in pivot.columns]
        pivot = pivot.reset_index()
        frames.append(pivot)
    df_wide = frames[0]
    for frame in frames[1:]:
        df_wide = pd.merge(df_wide, frame, on=i_vars, how="outer")
    if other_cols:
        other_agg = {c: "first" for c in other_cols}
        df_other = df_long.groupby(i_vars, sort=False)[other_cols].agg(other_agg).reset_index()
        df_wide = pd.merge(df_wide, df_other, on=i_vars, how="left")
    stub_wide_cols = []
    for stub in stubs:
        for jv in j_vals_raw:
            colname = f"{stub}{_j_suffix(jv)}"
            if colname in df_wide.columns:
                stub_wide_cols.append(colname)
    col_order = i_vars + stub_wide_cols + other_cols
    col_order = [c for c in col_order if c in df_wide.columns]
    return df_wide[col_order]


def _exact_equal_frames_ordered(cobra_df, oracle_df, label=""):
    """Order-SENSITIVE comparison (unlike approx_equal_frames): row i in
    cobra_df must equal row i in oracle_df exactly, column-set and row-count
    must match too. Used for the 0.18.1 dup/dropna cases where row order
    (ascending-by-i_vars vs first-occurrence) is itself part of what's under
    test — silently re-sorting before comparing would hide an order bug."""
    if set(cobra_df.columns) != set(oracle_df.columns):
        chk(f"{label}: column set", False,
            f"cobra={sorted(cobra_df.columns)} oracle={sorted(oracle_df.columns)}")
        return
    if len(cobra_df) != len(oracle_df):
        chk(f"{label}: row count", False, f"cobra={len(cobra_df)} oracle={len(oracle_df)}")
        return
    cols = sorted(cobra_df.columns)
    c = cobra_df[cols].reset_index(drop=True)
    o = oracle_df[cols].reset_index(drop=True)
    all_ok = True
    for col in cols:
        cv = c[col].to_numpy(dtype=float)
        ov = o[col].to_numpy(dtype=float)
        if not np.allclose(cv, ov, atol=1e-9, rtol=1e-9, equal_nan=True):
            all_ok = False
            chk(f"{label}: column '{col}' values (row-order-sensitive)", False,
                f"cobra={cv.tolist()} oracle={ov.tolist()}")
    if all_ok:
        chk(f"{label}: all columns + row order match ({len(cols)} cols, {len(cobra_df)} rows)", True)


def test_reshape_dup_and_dropna():
    """0.18.1 PATCH differential coverage: reshape wide duplicate-(i,j)
    first-wins collapse + dropna-driven i-group survival (contract §22).
    Root cause of the frozen-oracle kitchensink_panel divergence (n_wide 120
    cobra vs 119 oracle): NOT a literal duplicate (id,year) row anywhere in
    that fixture — id=1's `y` is simply missing at every year, and
    pandas.pivot_table(..., aggfunc="first") defaults to dropna=True, which
    silently drops an i-group whose values are ALL missing for that stub.
    With multiple stubs the per-stub pivots are outer-merged, so a group
    dropped by one stub can be resurrected (NaN-filled) by another. Also
    covers the true duplicate-(i,j) case (first-in-row-order wins, matching
    aggfunc="first"; real Stata errors here — divergence-ledger item)."""
    print("reshape wide (duplicate i,j + dropna group survival — 0.18.1):")

    def check_case(label, df, stubs, i_vars, j_var):
        hbin = os.path.join(WORKDIR, f"rwdup_{label.replace(' ', '_')}.hbin")
        df_to_hbin_via_csv(df.astype(float), hbin)
        out = os.path.join(WORKDIR, f"rwdup_{label.replace(' ', '_')}_out.hbin")
        run_cobra(["reshape", "wide", "--data", hbin, "--stubs", ",".join(stubs),
                  "--i", ",".join(i_vars), "--j", j_var, "--out", out])
        cobra_df = hbin_to_df(out)
        oracle_df = _oracle_reshape_wide(df, stubs, i_vars, j_var)
        _exact_equal_frames_ordered(cobra_df, oracle_df, label=label)

    # single duplicate (i,j): first occurrence wins
    check_case("single dup", pd.DataFrame({
        "id": [1, 1, 2], "year": [2000, 2000, 2000], "y": [10.0, 99.0, 30.0],
    }), ["y"], ["id"], "year")

    # multiple dups (different ids), dup rows at the FIRST rows of the frame
    check_case("multi dup first row", pd.DataFrame({
        "id": [1, 1, 2, 2, 3], "year": [2000, 2000, 2000, 2000, 2000],
        "y": [7.0, 8.0, 70.0, 80.0, 300.0],
    }), ["y"], ["id"], "year")

    # dup at the LAST row of the frame
    check_case("dup last row", pd.DataFrame({
        "id": [1, 2, 2], "year": [2000, 2000, 2000], "y": [1.0, 20.0, 21.0],
    }), ["y"], ["id"], "year")

    # multiple stub vars, both duplicated at the same (i,j)
    check_case("multi stub dup", pd.DataFrame({
        "id": [1, 1, 2], "year": [2000, 2000, 2000],
        "y": [5.0, 55.0, 6.0], "w": [9.0, 99.0, 10.0],
    }), ["y", "w"], ["id"], "year")

    # dropna: single stub, one group entirely missing -> dropped
    check_case("dropna single stub drop", pd.DataFrame({
        "id": [1, 1, 2, 2, 3, 3], "year": [2000, 2001, 2000, 2001, 2000, 2001],
        "y": [np.nan, np.nan, 3.0, 4.0, 5.0, 6.0],
    }), ["y"], ["id"], "year")

    # dropna: row order is ascending-by-i_vars even with unsorted source rows
    check_case("dropna unsorted order", pd.DataFrame({
        "id": [3, 3, 1, 1, 2, 2], "year": [2000, 2001, 2000, 2001, 2000, 2001],
        "y": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
    }), ["y"], ["id"], "year")

    # dropna: multi-stub resurrection (dropped by one stub, kept by another)
    check_case("dropna resurrection", pd.DataFrame({
        "id": [1, 1, 2, 2, 3, 3], "year": [2000, 2001, 2000, 2001, 2000, 2001],
        "y": [np.nan, np.nan, 3.0, 4.0, 5.0, 6.0],
        "w": [7.0, 8.0, 70.0, 80.0, np.nan, np.nan],
    }), ["y", "w"], ["id"], "year")

    # dropna: ALL stubs missing for a group -> fully dropped
    check_case("dropna full drop", pd.DataFrame({
        "id": [1, 1, 2, 2], "year": [2000, 2001, 2000, 2001],
        "y": [np.nan, np.nan, 3.0, 4.0], "w": [np.nan, np.nan, 30.0, 40.0],
    }), ["y", "w"], ["id"], "year")

    # kitchensink_panel.dta repro shape: 120 ids x 8 years, id=1 all-missing y
    # -> n_wide must be 119 (the literal M3b-reported divergence).
    ks_ids, ks_years, ks_y = [], [], []
    for _id in range(1, 121):
        for yr in range(2014, 2022):
            ks_ids.append(_id)
            ks_years.append(yr)
            ks_y.append(np.nan if _id == 1 else float(_id * 10 + yr - 2014))
    ks_df = pd.DataFrame({"id": ks_ids, "year": ks_years, "y": ks_y})
    hbin = os.path.join(WORKDIR, "rwdup_kitchensink_shape.hbin")
    df_to_hbin_via_csv(ks_df, hbin)
    out = os.path.join(WORKDIR, "rwdup_kitchensink_shape_out.hbin")
    run_cobra(["reshape", "wide", "--data", hbin, "--stubs", "y", "--i", "id", "--j", "year", "--out", out])
    cobra_df = hbin_to_df(out)
    oracle_df = _oracle_reshape_wide(ks_df, ["y"], ["id"], "year")
    chk("kitchensink-shape repro: n_wide == 119 (matches frozen 1.16.0 oracle)",
        len(cobra_df) == 119 == len(oracle_df), f"cobra={len(cobra_df)} oracle={len(oracle_df)}")
    _exact_equal_frames_ordered(cobra_df, oracle_df, label="kitchensink-shape repro full frame")


# ===========================================================================
# merge
# ===========================================================================
def test_merge():
    print("merge:")
    rng = np.random.default_rng(3)
    master = pd.DataFrame({"id": np.arange(100, dtype=float), "x": rng.normal(size=100)})
    using = pd.DataFrame({"id": np.arange(50, 150, dtype=float), "y": rng.normal(size=100)})
    m_hbin = os.path.join(WORKDIR, "merge_m.hbin")
    u_hbin = os.path.join(WORKDIR, "merge_u.hbin")
    df_to_hbin_via_csv(master, m_hbin)
    df_to_hbin_via_csv(using, u_hbin)

    out = os.path.join(WORKDIR, "merge_out.hbin")
    run_cobra(["merge", "--data", m_hbin, "--using", u_hbin, "--type", "1:1", "--key", "id", "--out", out])
    cobra_df = hbin_to_df(out)

    # oracle: exactly merge_op.py's algorithm (pandas outer merge + _merge_code)
    oracle_df = master.merge(using, on="id", how="outer")
    master_keys = set(master["id"])
    using_keys = set(using["id"])

    def merge_code(k):
        if k in master_keys and k in using_keys:
            return 3.0
        elif k in master_keys:
            return 1.0
        else:
            return 2.0
    oracle_df["_merge"] = oracle_df["id"].apply(merge_code)

    chk("merge 1:1 row count matches pandas outer merge", len(cobra_df) == len(oracle_df))
    c_counts = cobra_df["_merge"].value_counts().to_dict()
    o_counts = oracle_df["_merge"].value_counts().to_dict()
    chk("merge 1:1 _merge code distribution matches oracle",
        {k: c_counts.get(k, 0) for k in (1.0, 2.0, 3.0)} == {k: o_counts.get(k, 0) for k in (1.0, 2.0, 3.0)},
        f"cobra={c_counts} oracle={o_counts}")
    approx_equal_frames(cobra_df, oracle_df, key_cols=["id"], label="merge 1:1 full frame")

    # keepusing + nogenerate
    out2 = os.path.join(WORKDIR, "merge_out2.hbin")
    run_cobra(["merge", "--data", m_hbin, "--using", u_hbin, "--type", "1:1", "--key", "id",
              "--nogenerate", "--out", out2])
    _, cols2 = read_hbin(out2)
    chk("merge --nogenerate omits _merge column", "_merge" not in cols2)

    # cardinality violation should fail
    dup_master = pd.DataFrame({"id": [1.0, 1.0, 2.0], "x": [1.0, 2.0, 3.0]})
    dm_hbin = os.path.join(WORKDIR, "merge_dupm.hbin")
    df_to_hbin_via_csv(dup_master, dm_hbin)
    r = subprocess.run([COBRA, "merge", "--data", dm_hbin, "--using", u_hbin, "--type", "1:1",
                       "--key", "id", "--out", os.path.join(WORKDIR, "merge_bad.hbin")],
                       capture_output=True, text=True, cwd=WORKDIR)
    chk("merge 1:1 rejects non-unique master key (matches Stata/merge_op.py cardinality check)",
        r.returncode != 0)


def _oracle_merge_strings(master, using, key):
    """merge_op.py's algorithm on string-keyed frames: pandas outer merge +
    set-membership _merge codes. Master/using key columns are object (str)."""
    out = master.merge(using, on=key, how="outer")
    mk, uk = set(master[key].dropna()), set(using[key].dropna())

    def code(k):
        if pd.isna(k):
            # one-sided missing key: present only where it came from
            return 1.0  # (our fixtures only put missing keys on the master side)
        if k in mk and k in uk:
            return 3.0
        return 1.0 if k in mk else 2.0
    out["_merge"] = out[key].apply(code)
    return out


def test_merge_factor_keys():
    """0.18.0 Eval gate item 1 (BLOCKER): factor keys must join by LABEL, not
    per-file int32 code. Misaligned level tables across the two files (the
    normal case — dictionaries are first-seen order) previously produced silent
    misjoins: master [a,b,zz,c] vs using [a,b,c,d] falsely matched zz<->c and
    c<->d by raw code and dropped the using-only d row. Covers 1:1, m:1, 1:m."""
    print("merge (factor keys, misaligned level tables):")

    # --- 1:1, Eval's exact repro shape (incl. a master-side missing key) ---
    m = pd.DataFrame({"k": ["a", "b", "zz", None, "c"], "xm": [1.0, 2, 3, 4, 5]})
    u = pd.DataFrame({"k": ["a", "b", "c", "d"], "xu": [10.0, 20, 30, 40]})
    m_hbin = os.path.join(WORKDIR, "mfk_m.hbin")
    u_hbin = os.path.join(WORKDIR, "mfk_u.hbin")
    df_to_hbin_via_csv(m, m_hbin, factor_cols=["k"])
    df_to_hbin_via_csv(u, u_hbin, factor_cols=["k"])
    out = os.path.join(WORKDIR, "mfk_out.hbin")
    run_cobra(["merge", "--data", m_hbin, "--using", u_hbin, "--type", "1:1", "--key", "k", "--out", out])
    cobra_df = hbin_to_df(out)
    oracle_df = _oracle_merge_strings(m, u, "k")
    chk("merge 1:1 factor keys: row count (6 = a,b,c matched + zz + missing + d)",
        len(cobra_df) == len(oracle_df) == 6)
    approx_equal_frames(cobra_df, oracle_df, key_cols=["k", "xm"], label="merge 1:1 factor-key full frame")
    row_c = cobra_df[cobra_df["k"] == "c"]
    chk("merge 1:1 factor keys: c joins by LABEL (xm=5, xu=30, _merge=3)",
        len(row_c) == 1 and row_c["xm"].iloc[0] == 5.0 and row_c["xu"].iloc[0] == 30.0
        and row_c["_merge"].iloc[0] == 3.0)
    row_d = cobra_df[cobra_df["k"] == "d"]
    chk("merge 1:1 factor keys: using-only d row present with _merge=2",
        len(row_d) == 1 and row_d["_merge"].iloc[0] == 2.0 and np.isnan(row_d["xm"].iloc[0]))
    row_zz = cobra_df[cobra_df["k"] == "zz"]
    chk("merge 1:1 factor keys: zz stays master-only (no false code-match)",
        len(row_zz) == 1 and row_zz["_merge"].iloc[0] == 1.0 and np.isnan(row_zz["xu"].iloc[0]))

    # --- m:1 (master repeats keys, using unique, misaligned levels) ---
    m2 = pd.DataFrame({"k": ["x", "y", "y", "w"], "xm": [1.0, 2, 3, 4]})
    u2 = pd.DataFrame({"k": ["y", "w", "q"], "xu": [100.0, 200, 300]})
    m2_hbin = os.path.join(WORKDIR, "mfk_m2.hbin")
    u2_hbin = os.path.join(WORKDIR, "mfk_u2.hbin")
    df_to_hbin_via_csv(m2, m2_hbin, factor_cols=["k"])
    df_to_hbin_via_csv(u2, u2_hbin, factor_cols=["k"])
    out2 = os.path.join(WORKDIR, "mfk_out2.hbin")
    run_cobra(["merge", "--data", m2_hbin, "--using", u2_hbin, "--type", "m:1", "--key", "k", "--out", out2])
    cobra_df2 = hbin_to_df(out2)
    oracle_df2 = _oracle_merge_strings(m2, u2, "k")
    approx_equal_frames(cobra_df2, oracle_df2, key_cols=["k", "xm"], label="merge m:1 factor-key full frame")

    # --- 1:m (using repeats keys, master unique, misaligned levels) ---
    m3 = pd.DataFrame({"k": ["p", "q"], "xm": [1.0, 2]})
    u3 = pd.DataFrame({"k": ["q", "q", "r"], "xu": [10.0, 20, 30]})
    m3_hbin = os.path.join(WORKDIR, "mfk_m3.hbin")
    u3_hbin = os.path.join(WORKDIR, "mfk_u3.hbin")
    df_to_hbin_via_csv(m3, m3_hbin, factor_cols=["k"])
    df_to_hbin_via_csv(u3, u3_hbin, factor_cols=["k"])
    out3 = os.path.join(WORKDIR, "mfk_out3.hbin")
    run_cobra(["merge", "--data", m3_hbin, "--using", u3_hbin, "--type", "1:m", "--key", "k", "--out", out3])
    cobra_df3 = hbin_to_df(out3)
    oracle_df3 = _oracle_merge_strings(m3, u3, "k")
    approx_equal_frames(cobra_df3, oracle_df3, key_cols=["k", "xu"], label="merge 1:m factor-key full frame")

    # --- mixed dtype key (factor in master, numeric in using) is a hard error
    #     (the pandas oracle raises on merging object with float64) ---
    m4 = pd.DataFrame({"k": [1.0, 2.0], "xm": [1.0, 2.0]})
    m4_hbin = os.path.join(WORKDIR, "mfk_m4.hbin")
    df_to_hbin_via_csv(m4, m4_hbin)  # k numeric here
    r = subprocess.run([COBRA, "merge", "--data", m4_hbin, "--using", u3_hbin, "--type", "1:1",
                       "--key", "k", "--out", os.path.join(WORKDIR, "mfk_bad.hbin")],
                       capture_output=True, text=True, cwd=WORKDIR)
    chk("merge factor-vs-numeric key is a hard error (matches pandas oracle)", r.returncode != 0)


def test_duplicates_factor():
    """duplicates on a factor (string) column groups by label — pinned so a
    future refactor of the key path can't regress to cross-file-style code
    grouping (within one file code<->label is bijective, so this must hold)."""
    print("duplicates (factor column):")
    df = pd.DataFrame({"k": ["a", "b", "a", "c", "b", "a"], "v": [1.0, 2, 3, 4, 5, 6]})
    hbin = os.path.join(WORKDIR, "dupf_in.hbin")
    df_to_hbin_via_csv(df, hbin, factor_cols=["k"])

    out = os.path.join(WORKDIR, "dupf_drop.hbin")
    run_cobra(["duplicates", "drop", "--data", hbin, "--vars", "k", "--out", out])
    cobra_df = hbin_to_df(out)
    oracle_df = df.drop_duplicates(subset=["k"], keep="first").reset_index(drop=True)
    chk("duplicates drop on factor col matches pandas drop_duplicates by label",
        len(cobra_df) == len(oracle_df) and list(cobra_df["k"]) == list(oracle_df["k"])
        and np.allclose(cobra_df["v"].to_numpy(), oracle_df["v"].to_numpy()))

    out2 = os.path.join(WORKDIR, "dupf_tag.hbin")
    run_cobra(["duplicates", "tag", "--data", hbin, "--vars", "k", "--gen", "dtag", "--out", out2])
    cobra_df2 = hbin_to_df(out2)
    oracle_tag = df.groupby("k")["k"].transform("count").to_numpy() - 1
    chk("duplicates tag on factor col matches pandas groupby-by-label",
        np.allclose(cobra_df2["dtag"].to_numpy(), oracle_tag))


# ===========================================================================
# .dta reader (differential vs pandas.read_stata(..., convert_categoricals=False))
# ===========================================================================
def test_dta_reader():
    print(".dta reader:")
    fixdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dta_fixtures")
    for fname, fmt in [("mixed_115.dta", 115), ("mixed_117.dta", 117), ("mixed_118.dta", 118),
                       # BUG-375: was auto_* (StataCorp's bundled dataset — the one
                       # licensing blocker on shipping these fixtures). fatalities is
                       # ours; same structural coverage, no vendor asset.
                       ("fatalities_115.dta", 115), ("fatalities_117.dta", 117),
                       ("fatalities_118.dta", 118),
                       # BUG-024 regression: a string column with a genuine empty-string ("")
                       # cell must stay a distinct, non-missing factor level (Stata/pandas
                       # convention), not collapse to missing like cobra convert's CSV path
                       # does. Generic per-column comparison below (numeric/date/string
                       # branches) already exercises this once the fixture is in the loop:
                       # read_hbin() maps code>=0 -> levels[code] (incl. "" itself), code<0
                       # (missing) -> None, and the oracle branch compares against
                       # pandas.read_stata's own "" (non-null) value for that cell.
                       ("empty_str_115.dta", 115), ("empty_str_117.dta", 117), ("empty_str_118.dta", 118)]:
        path = os.path.join(fixdir, fname)
        if not os.path.exists(path):
            chk(f".dta fixture {fname} exists", False, "run gen_fixtures.do first")
            continue
        out = os.path.join(WORKDIR, fname.replace(".dta", ".hbin"))
        run_cobra(["import-dta", path, out])
        cobra_df = hbin_to_df(out)

        oracle_df = pd.read_stata(path, convert_categoricals=False)
        # oracle dtype casting for comparability: cobra always emits f64/object(factor)
        ok = True
        if set(cobra_df.columns) != set(oracle_df.columns):
            ok = False
        else:
            for col in oracle_df.columns:
                ov = oracle_df[col]
                cv = cobra_df[col]
                if pd.api.types.is_numeric_dtype(ov) and not pd.api.types.is_datetime64_any_dtype(ov):
                    ov_f = ov.to_numpy(dtype=float)
                    cv_f = cv.to_numpy(dtype=float)
                    if not np.allclose(cv_f, ov_f, atol=1e-4, rtol=1e-4, equal_nan=True):
                        ok = False
                        print(f"    mismatch numeric col '{col}': cobra={cv_f[:5]} oracle={ov_f[:5]}")
                elif pd.api.types.is_datetime64_any_dtype(ov):
                    days = (ov - pd.Timestamp("1960-01-01")).dt.total_seconds() / 86400.0
                    if not np.allclose(cv.to_numpy(dtype=float), days.to_numpy(dtype=float), atol=1e-6):
                        ok = False
                        print(f"    mismatch date col '{col}'")
                else:
                    ov_s = ov.astype(str).where(~ov.isna(), None).to_numpy()
                    cv_s = cv.to_numpy()
                    if not all((a == b) or (a is None and b is None) for a, b in zip(cv_s, ov_s)):
                        ok = False
                        print(f"    mismatch string col '{col}': cobra={cv_s[:5]} oracle={ov_s[:5]}")
        chk(f"{fname} (format {fmt}) matches pandas.read_stata(convert_categoricals=False)", ok)


def main():
    test_gsort()
    test_gsort_missing()
    test_order()
    test_append()
    test_duplicates()
    test_duplicates_factor()
    test_recode()
    test_reshape()
    test_reshape_dup_and_dropna()
    test_merge()
    test_merge_factor_keys()
    test_dta_reader()

    print()
    print(f"{n_checks - len(fails)}/{n_checks} checks passed")
    if fails:
        print(f"FAILED: {len(fails)} check(s):")
        for f in fails:
            print(f"  - {f}")
        sys.exit(1)
    print("ALL DIFFERENTIAL CHECKS PASSED")


if __name__ == "__main__":
    main()
