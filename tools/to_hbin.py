#!/usr/bin/env python3
"""Convert a parquet/feather table to the cecon .hbin columnar format.

Layout (single file, little-endian):
    [0:8]    magic  b"CECONHB1"
    [8:16]   uint64 header_json_len
    [16:16+L] header JSON (UTF-8)
    pad to 64-byte boundary  -> data_start
    column blobs, each padded to a 64-byte boundary; the header records each
    column's offset *relative to data_start*.

Column dtypes:
    "f64"     8-byte little-endian double; missing = NaN.
    "factor"  4-byte little-endian int32 codes (0..K-1), -1 = missing; header
              carries the dictionary of level strings in field "levels".

Numeric columns (any int/float, including pandas nullable Int64/Float64) become
f64. String/object/category columns become factor. Use --factor to force a
numeric column (e.g. a string-categorical FE key) to be stored as a factor.

The engine can also build a factor on the fly from an f64 column, so storing FE
keys as f64 is fine too; factor storage just avoids re-hashing strings.

WARNING — do NOT --factor a column you will pass as a panel key to csdid /
xtpoisson / xtlogit / xtreg (i.e. --ivar / --time / --gvar / --offset). Those
estimators read the column as numeric f64; a factor stores appearance-order
int codes, so the engine would misread the values (wrong results, or a crash on
csdid). Only --factor string/categorical FE or cluster keys used with
feols / fepois. to_hbin prints a warning if a --factor name looks like a panel key.

Input formats & dependencies (only convert what you have):
    .parquet              -> pyarrow OR fastparquet (auto-falls-back between them)
    .feather / .arrow     -> needs pyarrow
    .dta                  -> pandas only (no pyarrow)
    .sav                  -> needs pyreadstat
    .csv / .tsv / .csv.gz -> pandas only (no pyarrow); --chunksize N streams huge files
    .pkl / .pickle        -> pandas only (a pickled DataFrame)
    .npy                  -> numpy only (structured or 2D array)
If you only have .csv/.dta/.pkl you do NOT need pyarrow. Prefer reading .parquet
or .dta DIRECTLY over a CSV detour. For very large .csv use --chunksize 200000.
"""
import argparse
import json
import struct
import sys

import numpy as np
import pandas as pd

MAGIC = b"CECONHB1"
ALIGN = 64


def align_up(x: int, a: int = ALIGN) -> int:
    return (x + a - 1) // a * a


STATA_EPOCH = pd.Timestamp("1960-01-01")  # Stata %td day 0


def encode_column(name: str, s: pd.Series, force_factor: bool):
    from pandas.api.types import is_numeric_dtype, is_datetime64_any_dtype
    # Datetime (e.g. a Stata %td date read from .dta) -> numeric Stata-epoch days,
    # so panel/time variables stay usable by xtset/tssmooth instead of becoming a
    # factor. Sub-day resolution is preserved; NaT -> NaN.
    if is_datetime64_any_dtype(s) and not force_factor:
        days = (pd.to_datetime(s) - STATA_EPOCH).dt.total_seconds() / 86400.0
        arr = np.ascontiguousarray(days.to_numpy(), dtype="<f8")
        return {"name": name, "dtype": "f64"}, arr.tobytes()
    # Anything not numeric is a factor. This catches object, pandas "string"/"str"
    # (pyarrow-backed), and category dtypes robustly — guarding against silently
    # coercing a non-numeric string column to all-NaN f64.
    if force_factor or not is_numeric_dtype(s):
        # pandas.factorize: codes are int (>=0), -1 for NA when use_na_sentinel
        codes, uniques = pd.factorize(s, use_na_sentinel=True)
        codes = np.ascontiguousarray(codes, dtype="<i4")
        levels = [str(u) for u in list(uniques)]
        return {"name": name, "dtype": "factor", "levels": levels}, codes.tobytes()
    # numeric (including nullable) -> f64 with NaN for missing
    arr = pd.to_numeric(s, errors="coerce").astype("float64").to_numpy()
    arr = np.ascontiguousarray(arr, dtype="<f8")
    return {"name": name, "dtype": "f64"}, arr.tobytes()


def read_parquet_any(path, columns):
    """Read parquet via whichever engine is installed (pyarrow OR fastparquet)."""
    try:
        return pd.read_parquet(path, columns=columns)  # default engine (pyarrow)
    except Exception:
        pass
    try:
        return pd.read_parquet(path, columns=columns, engine="fastparquet")
    except Exception as e:
        sys.exit(f"cannot read parquet '{path}': install pyarrow or fastparquet "
                 f"(pip install fastparquet), or convert to .csv/.dta. [{e}]")


def main():
    ap = argparse.ArgumentParser(description="Convert tabular data -> cecon .hbin")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--columns", default=None, help="comma-separated subset of columns")
    ap.add_argument("--factor", default="", help="comma-separated columns to force to factor")
    ap.add_argument("--chunksize", type=int, default=0,
                    help="for .csv/.tsv: read+encode in row-chunks of this size "
                         "(keeps peak memory low on huge files; 0 = read whole file)")
    args = ap.parse_args()

    cols = args.columns.split(",") if args.columns else None
    inp = args.input.lower()
    if inp.endswith(".parquet"):
        df = read_parquet_any(args.input, cols)
    elif inp.endswith((".feather", ".arrow")):
        df = pd.read_feather(args.input, columns=cols)
    elif inp.endswith(".dta"):
        # Stata file: %td dates -> datetime (then -> epoch days in encode_column);
        # value-labeled vars -> categorical -> factor.
        df = pd.read_stata(args.input, columns=cols, convert_categoricals=True)
    elif inp.endswith(".sav"):
        df = pd.read_spss(args.input)  # requires pyreadstat
        if cols:
            df = df[cols]
    elif inp.endswith((".pkl", ".pickle")):
        df = pd.read_pickle(args.input)  # pure pandas, no pyarrow
        if cols:
            df = df[cols]
    elif inp.endswith(".npy"):
        arr = np.load(args.input, allow_pickle=False)
        df = pd.DataFrame(arr)  # structured dtype -> named cols; plain 2D -> 0..k-1
        if cols:
            df = df[cols]
    elif inp.endswith((".csv", ".csv.gz", ".tsv")):
        sep = "\t" if inp.endswith(".tsv") else ","
        if args.chunksize and args.chunksize > 0:
            # Stream the CSV in row-chunks and concatenate per column — keeps peak
            # memory near one chunk instead of the whole file (fixes huge-CSV OOM).
            parts = []
            for chunk in pd.read_csv(args.input, sep=sep, usecols=cols,
                                     chunksize=args.chunksize):
                parts.append(chunk)
            df = pd.concat(parts, ignore_index=True) if parts else pd.read_csv(
                args.input, sep=sep, usecols=cols, nrows=0)
        else:
            df = pd.read_csv(args.input, sep=sep, usecols=cols)  # pure pandas, no pyarrow
    else:
        sys.exit("input must be .parquet, .feather, .arrow, .dta, .sav, .csv, "
                 ".tsv, .csv.gz, .pkl, or .npy")

    force_factor = {c for c in args.factor.split(",") if c}

    # Warn if a forced-factor column looks like a panel key — csdid/xtreg/xtpois/
    # xtlogit need ivar/time/gvar/offset as NUMERIC f64, not factor codes.
    PANEL_KEYISH = ("year", "yr", "wave", "wv", "month", "date", "time", "period",
                    "gvar", "cohort", "first_treat", "firsttreat", "ivar", "panelid",
                    "pid", "hpid", "id")
    for c in sorted(force_factor):
        lc = str(c).lower()
        if any(k in lc for k in PANEL_KEYISH):
            print(f"WARNING: forcing '{c}' to --factor. If you pass it to csdid/xtreg/"
                  f"xtpoisson/xtlogit as --ivar/--time/--gvar/--offset it MUST stay "
                  f"numeric — drop it from --factor.", file=sys.stderr)

    n = len(df)

    meta_cols = []
    blobs = []
    rel = 0
    for name in df.columns:
        meta, blob = encode_column(name, df[name], name in force_factor)
        meta["offset"] = rel  # relative to data_start
        meta["nbytes"] = len(blob)
        meta_cols.append(meta)
        blobs.append(blob)
        rel = align_up(rel + len(blob))

    header = {"magic": "CECONHB1", "version": 1, "n_rows": int(n), "columns": meta_cols}
    header_json = json.dumps(header, ensure_ascii=False).encode("utf-8")

    data_start = align_up(16 + len(header_json))
    with open(args.output, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<Q", len(header_json)))
        f.write(header_json)
        f.write(b"\0" * (data_start - (16 + len(header_json))))
        pos = data_start
        for meta, blob in zip(meta_cols, blobs):
            want = data_start + meta["offset"]
            if pos < want:
                f.write(b"\0" * (want - pos))
                pos = want
            f.write(blob)
            pos += len(blob)

    nfac = sum(1 for m in meta_cols if m["dtype"] == "factor")
    print(f"wrote {args.output}: {n:,} rows, {len(meta_cols)} cols "
          f"({nfac} factor), data_start={data_start}, file_end={pos:,} bytes")


if __name__ == "__main__":
    main()
