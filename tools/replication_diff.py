#!/usr/bin/env python3
"""Compare two directories of replication output CELL BY CELL, at a ladder tier.

The paper-completion axis needs more than "the do-file ran". A package that runs
to the end and writes a table of different numbers has not been replicated. This
takes the Stata-produced outputs and ours and compares every cell of every table
it can parse, at the tolerance `tests/eval/TOLERANCE.md` ratifies for the tier.

    tools/gatepy tools/replication_diff.py --stata DIR --piet DIR [--tier T2]
                 [--cobra PATH] [--ignore GLOB]... [--report MD]

`--ignore GLOB` drops a relative path from the comparison entirely -- logs and
figures a package rewrites on every run, `.DS_Store`, a scratch directory. It is
a loud exclusion: the count and the patterns are printed, and an ignored file
never becomes a cell, so `cells = same + diff` stays exact arithmetic over what
was actually compared.

`--report MD` writes the cell-level diff as a markdown table (file, row, col,
both values, verdict) next to the verdict line. The adjudicated boundary flips
are rows in it too, marked as such: a PASS that was adjudicated rather than
plain is the one a reader most needs to see.

Formats, and what "a cell" means in each:

  .tex          esttab/estout LaTeX. Rules and `tablenotes` blocks are dropped,
                rows split on `\\\\`, cells on unescaped `&`. Stars and the
                parentheses around a standard error are stripped before the
                numeric parse -- a parenthesised value is an SE, still a number.
  .csv .txt     csv.reader with a sniffed delimiter. `export delimited` writes
                comma-separated content under a .txt name, so extension is not
                evidence of delimiter.
  .xls          outreg2's ".xls" is TAB-SEPARATED TEXT, not a workbook, and is
                routed here rather than through openpyxl.
  .xlsx         openpyxl, with a zipfile + ElementTree fallback for a workbook
                openpyxl will not open.
  .dta          TWO HOPS: `cobra import-dta` to .hbin, then tools/hbin_read.py.
                (There is no `cobra export` verb; the reader is native.)
  .gph .pdf     existence and size > 0. No pixel comparison -- a graph that
                differs by a pixel is not a finding this tool can make honestly,
                and pretending otherwise would put a flaky RED in a gate.

Every comparison -- including a .gph existence check and a file present on only
one side -- counts as one cell, so `cells = same + diff` is exact arithmetic and
not a summary.

The tolerance rules implemented here, with the TOLERANCE.md lines they come from:

  §2  ladder rtol by tier; T2 = 1e-9 baseline (:39)
  §3.1 p-values get atol 1e-8 and NO bare rtol (:45-47)
  §3.2 every rtol on a can-be-zero quantity carries atol = rtol_tier x scale,
       where scale is a named magnitude for the quantity family (:48-52)
  §3.3 float-storage columns are rounded to float32 on BOTH sides (:53-54)
  §3.4 comparison is value-based, never bit-based (:55-56)
  §1  display width never widens the numeric band (:22-26), and the
      boundary-flip rule (:17-20): a formatted-string mismatch adjudicates PASS
      iff the numeric compare passes at tier AND the difference is confined to
      the last displayed digit at a half-way rounding boundary. Each such
      adjudication is RECORDED -- `boundary_flips=N`, with coordinates -- never
      silent.
"""
from __future__ import annotations

import csv
import fnmatch
import io
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET
from decimal import Decimal, InvalidOperation
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import engine_arm  # noqa: E402
from hbin_read import read_hbin  # noqa: E402

# TOLERANCE.md §2. T3 and T4 are quoted there as ranges ("1e-8 … 1e-7",
# "1e-6 … 1e-5"); the LOOSE end is taken, because the tier is the permission a
# quantity has been granted and the tight end is already covered by the tier
# below it. T5 is distributional (:42) -- a draw-level agreement test, not a
# cell compare -- so it is refused by name rather than silently approximated.
LADDER = {"T0": 0.0, "T1": 1e-12, "T2": 1e-9, "T3": 1e-7, "T4": 1e-5}
P_ATOL = 1e-8  # §3.1
P_COLUMN_RE = re.compile(r"^(p|pval|pvalue|p_value|prob)$", re.I)

TEXT_EXT = {".csv", ".txt", ".xls"}
EXIST_ONLY_EXT = {".gph", ".pdf"}


# ---------------------------------------------------------------------------
# Parsers. Each returns (header-or-None, rows), rows being lists of strings.

_TEX_NOTES = re.compile(r"\\begin\{tablenotes\}.*?\\end\{tablenotes\}", re.S)
_TEX_RULE = re.compile(r"\\(?:toprule|midrule|bottomrule|hline)\b")
_TEX_AMP = re.compile(r"(?<!\\)&")


def parse_tex(path: Path):
    text = _TEX_NOTES.sub("", path.read_text(encoding="utf-8", errors="replace"))
    # Environment lines are dropped whole rather than by regex over the argument:
    # `\begin{tabular}{l*{1}{c}}` nests braces, so a `\{[^}]*\}` argument matcher
    # stops in the middle of it and leaves `{c}}` behind as a cell.
    kept = [ln for ln in text.split("\n")
            if not ln.lstrip().startswith(("\\begin{", "\\end{"))]
    body = _TEX_RULE.sub("", "\n".join(kept))
    rows = []
    for chunk in body.split("\\\\"):
        if not chunk.strip():
            continue
        rows.append([c.strip() for c in _TEX_AMP.split(chunk)])
    return None, rows


def parse_delimited(path: Path):
    raw = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix.lower() == ".xls":
        delim = "\t"
    else:
        try:
            delim = csv.Sniffer().sniff(raw[:4096], delimiters=",\t;|").delimiter
        except csv.Error:
            delim = ","
    rows = [[c.strip() for c in r] for r in csv.reader(io.StringIO(raw), delimiter=delim)]
    return _split_header(rows)


_XL_NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"


def parse_xlsx(path: Path):
    try:
        import openpyxl
        wb = openpyxl.load_workbook(path, data_only=True, read_only=True)
    except Exception as e:  # noqa: BLE001 -- any refusal to open routes to the fallback
        print(f"[repdiff] openpyxl declined {path.name} ({type(e).__name__}: {e}); "
              "reading the sheet XML directly")
        return _split_header(_xlsx_rows_from_zip(path))
    rows = []
    for ws in wb.worksheets:
        for r in ws.values:
            rows.append(["" if c is None else str(c) for c in r])
    wb.close()
    return _split_header(rows)


def _xlsx_rows_from_zip(path: Path) -> list[list[str]]:
    """xl/worksheets/sheet1.xml, read without openpyxl.

    Cells are placed by their `r` reference rather than by document order, so a
    sheet that omits empty cells (which the writer under test does) still lines
    its columns up with the openpyxl path.
    """
    with zipfile.ZipFile(path) as z:
        shared = []
        if "xl/sharedStrings.xml" in z.namelist():
            sst = ET.fromstring(z.read("xl/sharedStrings.xml"))
            shared = ["".join(t.text or "" for t in si.iter(_XL_NS + "t"))
                      for si in sst.findall(_XL_NS + "si")]
        sheet = ET.fromstring(z.read("xl/worksheets/sheet1.xml"))
    rows = []
    for row in sheet.iter(_XL_NS + "row"):
        cells: dict[int, str] = {}
        for i, c in enumerate(row.findall(_XL_NS + "c")):
            idx = _col_index(c.get("r")) if c.get("r") else i
            t = c.get("t")
            if t == "inlineStr":
                node = c.find(_XL_NS + "is")
                val = "".join(x.text or "" for x in node.iter(_XL_NS + "t")) if node is not None else ""
            elif t == "s":
                v = c.find(_XL_NS + "v")
                val = shared[int(v.text)] if v is not None and v.text else ""
            else:
                v = c.find(_XL_NS + "v")
                val = v.text if v is not None and v.text is not None else ""
            cells[idx] = val
        width = max(cells) + 1 if cells else 0
        rows.append([cells.get(i, "") for i in range(width)])
    return rows


def _col_index(ref: str) -> int:
    n = 0
    for ch in ref:
        if not ch.isalpha():
            break
        n = n * 26 + (ord(ch.upper()) - 64)
    return n - 1


def parse_dta(path: Path, cobra: Path, tmp: Path):
    """(header, rows, float_columns) via `cobra import-dta` -> .hbin -> read_hbin.

    `--out-meta` is asked for because the .hbin header carries only f64/factor:
    §3.3's float rounding needs the SOURCE storage type, and the sidecar's
    `stata_type` ("byte"|"int"|"long"|"float"|"double"|"strNN"|"strL",
    src/io/dta_reader.h:58) is the only place it survives the conversion.
    """
    stem = path.stem + "_" + str(abs(hash(str(path))) % 10**8)
    hbin = tmp / (stem + ".hbin")
    meta = tmp / (stem + ".meta.json")
    proc = subprocess.run([str(cobra), "import-dta", str(path), str(hbin),
                           "--out-meta", str(meta)],
                          capture_output=True, text=True, timeout=1800)
    if proc.returncode != 0 or not hbin.exists():
        raise RuntimeError(f"cobra import-dta failed on {path}: {proc.stderr.strip()[:300]}")
    n, cols = read_hbin(hbin)
    floats = set()
    try:
        for v in json.loads(meta.read_text(encoding="utf-8")).get("variables", []):
            if v.get("stata_type") == "float":
                floats.add(v.get("name"))
    except (OSError, json.JSONDecodeError):
        pass  # left empty -- an unreadable sidecar means no float rounding, not a silent pass
    names = [k for k in cols if not k.endswith("__levels")]
    rows = []
    for i in range(n):
        row = []
        for name in names:
            arr = cols[name]
            levels = cols.get(name + "__levels")
            if levels is not None:
                # to_hbin.py:14-15: factor codes are 0..K-1 indices into `levels`,
                # -1 = missing. Comparing the LEVEL rather than the code keeps this
                # value-based (§3.4): two files can hold the same strings in a
                # different appearance order and so carry different codes.
                code = int(arr[i])
                row.append(levels[code] if 0 <= code < len(levels) else "")
            else:
                row.append(repr(float(arr[i])))
        rows.append(row)
    return names, rows, floats


def _split_header(rows):
    """(header, body). The first row is a header when none of its cells is a
    number -- the only evidence these formats carry."""
    if rows and rows[0] and not any(to_number(c) is not None for c in rows[0]):
        return rows[0], rows[1:]
    return None, rows


# ---------------------------------------------------------------------------
# Cells.

_STARS = re.compile(r"\*{1,3}$")


def to_number(cell) -> float | None:
    """The number a formatted cell carries, or None.

    Significance stars and the parentheses esttab wraps a standard error in are
    formatting, not value; both are stripped before the parse.
    """
    if cell is None:
        return None
    s = str(cell).strip()
    if not s:
        return None
    s = _STARS.sub("", s).strip()
    if len(s) > 1 and s.startswith("(") and s.endswith(")"):
        s = s[1:-1].strip()
        s = _STARS.sub("", s).strip()
    try:
        return float(s)
    except ValueError:
        return None


def _decimals(s: str) -> int:
    s = _STARS.sub("", str(s).strip()).strip("()").strip()
    if "e" in s.lower():
        return 0
    return len(s.split(".", 1)[1]) if "." in s else 0


def numeric_ok(a: float, b: float, rtol: float, scale: float, is_p: bool) -> bool:
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if is_p:
        return abs(a - b) <= P_ATOL          # §3.1: atol only, never a bare rtol
    if rtol == 0.0:
        return a == b                        # T0 admits no tolerance
    atol = rtol * scale                      # §3.2 near-zero floor
    return abs(a - b) <= atol + rtol * abs(b)


def boundary_flip(sa: str, sb: str) -> bool:
    """TOLERANCE.md :17-20 clause (b), checked rather than assumed.

    The two formatted strings must be identical except for their last character,
    both of those must be digits, and the two must be ADJACENT values of the last
    displayed place -- which is exactly when a half-way rounding boundary lies
    between them. Clause (a) -- the numeric compare passing at tier -- is the
    caller's, and is the half that discriminates: on a source that carries ONLY
    display precision, two cells whose last digit differs are usually far outside
    a T2 band, so this rule fires where the last displayed place is finer than
    the tier (wide integers, large magnitudes) and correctly does not where it is
    coarser.

    The adjacency test is DECIMAL, not binary. It used to be `hi - lo > step`
    over the parsed doubles, and `166423868.74 - 166423868.73` is
    0.010000020265579224 in a double: a genuine last-digit pair failed that guard
    and was reported as a finding instead of adjudicated. Measured over 18,561
    random last-digit pairs (1-4 decimals, 5-9 integer digits) the guard rejected
    54.1% of them and the half-way term a further 2.1%, so the rule recognised
    44% of exactly the input it exists for. `Decimal` parses the printed text, so
    the comparison is the one the printed text makes; an exponent's last digit is
    a decade rather than a rounding place, and `_decimals` returning 0 for those
    keeps `1.5e-08` vs `1.5e-09` from adjudicating.
    """
    sa, sb = sa.strip(), sb.strip()
    if len(sa) != len(sb) or sa[:-1] != sb[:-1]:
        return False
    if not (sa[-1].isdigit() and sb[-1].isdigit()):
        return False
    try:
        da, db = Decimal(sa), Decimal(sb)
    except InvalidOperation:
        return False
    return abs(da - db) == Decimal(1).scaleb(-_decimals(sa))


# ---------------------------------------------------------------------------

def column_scales(rows, ncols: int) -> list[float]:
    """§3.2's `scale`: a named magnitude per quantity family. The family here is
    the COLUMN, and the magnitude is the largest finite absolute value in it --
    the table's own `max|b|`, computed rather than assumed."""
    scales = [0.0] * ncols
    for row in rows:
        for j, cell in enumerate(row[:ncols]):
            v = to_number(cell)
            if v is not None and math.isfinite(v):
                scales[j] = max(scales[j], abs(v))
    return scales


def compare_table(rel: str, sa_hdr, sa_rows, pi_hdr, pi_rows, rtol: float,
                  float_cols: set | None = None, display_gate: bool = True,
                  rows_out: list | None = None):
    """(cells, same, flips, findings) for one parsed pair; `rows_out` collects
    a structured record per finding and per adjudicated flip, for --report.

    `display_gate` is False for sources whose cells are stored VALUES rather than
    formatted output (.dta): TOLERANCE.md §1 keeps the two gates apart, and a
    full-precision double rendered by `repr` is not a display string, so holding
    it to string equality would invent a display finding the file cannot carry.

    The header and row-count checks each count as one cell so that
    `cells = same + diff` stays exact -- a finding that moved no counter would
    make a RED verdict print `diff=0`.
    """
    findings = []
    cells = same = flips = 0
    def note(row, col, a, b, verdict):
        if rows_out is not None:
            rows_out.append({"file": rel, "row": row, "col": col,
                             "stata": a, "piet": b, "verdict": verdict})
    if sa_hdr != pi_hdr:
        cells += 1
        findings.append(f"{rel}: header differs\n      stata: {sa_hdr}\n      piet : {pi_hdr}")
        note("-", "header", sa_hdr, pi_hdr, "DIFF")
    nrows = max(len(sa_rows), len(pi_rows))
    if len(sa_rows) != len(pi_rows):
        cells += 1
        findings.append(f"{rel}: {len(sa_rows)} data row(s) in stata, {len(pi_rows)} in piet")
        note("-", "nrows", len(sa_rows), len(pi_rows), "DIFF")
    ncols = max([len(r) for r in sa_rows + pi_rows] or [0])
    scales = column_scales(sa_rows + pi_rows, ncols)
    for i in range(nrows):
        ra = sa_rows[i] if i < len(sa_rows) else []
        rb = pi_rows[i] if i < len(pi_rows) else []
        for j in range(max(len(ra), len(rb))):
            ca = ra[j] if j < len(ra) else None
            cb = rb[j] if j < len(rb) else None
            cells += 1
            name = sa_hdr[j] if sa_hdr and j < len(sa_hdr) else None
            is_p = bool(name and P_COLUMN_RE.match(str(name).strip()))
            va, vb = to_number(ca), to_number(cb)
            if va is not None and vb is not None:
                if float_cols and name in float_cols:
                    # §3.3: a float column carries ~7 significant digits; comparing
                    # the doubles behind it is comparing artifacts.
                    va, vb = float(np.float32(va)), float(np.float32(vb))
                if not numeric_ok(va, vb, rtol, scales[j] if j < len(scales) else 0.0, is_p):
                    findings.append(f"{rel} (row {i}, col {j}): {ca!r} vs {cb!r}")
                    note(i, j, ca, cb, "DIFF")
                    continue
                if display_gate and str(ca).strip() != str(cb).strip():
                    if boundary_flip(str(ca), str(cb)):
                        flips += 1
                        note(i, j, ca, cb, "FLIP (adjudicated PASS)")
                        print(f"[repdiff] boundary flip adjudicated PASS: {rel} "
                              f"(row {i}, col {j}) {ca!r} vs {cb!r}")
                    else:
                        findings.append(f"{rel} (row {i}, col {j}): formatted {ca!r} vs {cb!r} "
                                        "(numeric compare passes at tier; not a last-digit "
                                        "boundary flip)")
                        note(i, j, ca, cb, "DIFF (display)")
                        continue
                same += 1
            else:
                if (ca or "").strip() == (cb or "").strip():
                    same += 1
                else:
                    findings.append(f"{rel} (row {i}, col {j}): {ca!r} vs {cb!r}")
                    note(i, j, ca, cb, "DIFF")
    return cells, same, flips, findings


def _opt(name: str, default=None):
    a = sys.argv[1:]
    for i, tok in enumerate(a):
        if tok == name and i + 1 < len(a):
            return a[i + 1]
        if tok.startswith(name + "="):
            return tok.split("=", 1)[1]
    return default


def _opts(name: str) -> list[str]:
    """Every occurrence of a repeatable option, in order."""
    out, a = [], sys.argv[1:]
    for i, tok in enumerate(a):
        if tok == name and i + 1 < len(a):
            out.append(a[i + 1])
        elif tok.startswith(name + "="):
            out.append(tok.split("=", 1)[1])
    return out


def write_report(path: Path, rows: list, stata: Path, piet: Path, tier: str,
                 cells: int, same: int, flips: int, ignored: list, skipped: list):
    """The cell-level diff as markdown. Written even when there is nothing to
    report: an empty table over a stated denominator is a result, and a missing
    file would be indistinguishable from a run that never happened."""
    diff = cells - same
    lines = [f"# replication diff — tier {tier}", "",
             f"- stata: `{stata}`", f"- piet: `{piet}`",
             f"- cells **{cells}** · same **{same}** · diff **{diff}** · "
             f"boundary flips **{flips}**"]
    if ignored:
        lines.append(f"- ignored by `--ignore`: {len(ignored)} file(s) — "
                     + ", ".join(f"`{x}`" for x in ignored[:20]))
    if skipped:
        lines.append(f"- no parser for the extension: {len(skipped)} file(s) — "
                     + ", ".join(f"`{x}`" for x in skipped[:20]))
    lines += ["", "| file | row | col | stata | piet | verdict |",
              "|---|---|---|---|---|---|"]
    for r in rows:
        cell = lambda v: str(v).replace("|", "\\|")
        lines.append(f"| `{r['file']}` | {cell(r['row'])} | {cell(r['col'])} | "
                     f"`{cell(r['stata'])}` | `{cell(r['piet'])}` | {r['verdict']} |")
    if not rows:
        lines.append("| — | — | — | — | — | every compared cell agreed |")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[repdiff] report written to {path}")


def main() -> int:
    stata = _opt("--stata")
    piet = _opt("--piet")
    tier = _opt("--tier", "T2")
    cobra_opt = _opt("--cobra")
    if not stata or not piet:
        print(__doc__)
        return 2
    if tier not in LADDER:
        print(f"PROBE PREREQ MISSING: --tier {tier} is not a comparable tier. "
              f"Pick one of {sorted(LADDER)}; T5 is distributional (TOLERANCE.md :42) and "
              "needs a draw-level test, not a cell compare. Not a skip.")
        return 2
    sdir, pdir = Path(stata).resolve(), Path(piet).resolve()
    for d in (sdir, pdir):
        if not d.is_dir():
            print(f"PROBE PREREQ MISSING: {d} is not a directory. Not a skip.")
            return 2
    rtol = LADDER[tier]

    ignore_globs = _opts("--ignore")
    report_path = _opt("--report")
    rels = sorted({str(p.relative_to(sdir)) for p in sdir.rglob("*") if p.is_file()} |
                  {str(p.relative_to(pdir)) for p in pdir.rglob("*") if p.is_file()})
    ignored = [r for r in rels
               if any(fnmatch.fnmatch(r, g) or fnmatch.fnmatch(Path(r).name, g)
                      for g in ignore_globs)]
    rels = [r for r in rels if r not in set(ignored)]
    if ignored:
        print(f"[repdiff] {len(ignored)} file(s) ignored by --ignore "
              f"{ignore_globs}: " + ", ".join(ignored[:12]))
    cells = same = flips = 0
    findings: list[str] = []
    skipped: list[str] = []
    report_rows: list[dict] = []
    cobra = None
    tmp = Path(tempfile.mkdtemp(prefix="repdiff_"))
    try:
        for rel in rels:
            a, b = sdir / rel, pdir / rel
            ext = a.suffix.lower()
            if not a.exists() or not b.exists():
                cells += 1
                side = "stata" if a.exists() else "piet"
                findings.append(f"{rel}: present in {side} only")
                report_rows.append({"file": rel, "row": "-", "col": "file",
                                    "stata": "present" if a.exists() else "absent",
                                    "piet": "absent" if a.exists() else "present",
                                    "verdict": "DIFF"})
                continue
            if ext in EXIST_ONLY_EXT:
                cells += 1
                sa, sb = a.stat().st_size, b.stat().st_size
                if sa > 0 and sb > 0:
                    same += 1
                else:
                    findings.append(f"{rel}: existence check -- stata {sa} bytes, piet {sb} bytes")
                    report_rows.append({"file": rel, "row": "-", "col": "bytes",
                                        "stata": sa, "piet": sb, "verdict": "DIFF"})
                continue
            try:
                float_cols = None
                if ext == ".tex":
                    ha, ra = parse_tex(a)
                    hb, rb = parse_tex(b)
                elif ext in TEXT_EXT:
                    ha, ra = parse_delimited(a)
                    hb, rb = parse_delimited(b)
                elif ext == ".xlsx":
                    ha, ra = parse_xlsx(a)
                    hb, rb = parse_xlsx(b)
                elif ext == ".dta":
                    if cobra is None:
                        cobra = engine_arm.resolve_cobra(cobra_opt)
                        print(f"[repdiff] {engine_arm.describe_arm(cobra)}")
                    ha, ra, fa = parse_dta(a, cobra, tmp)
                    hb, rb, fb = parse_dta(b, cobra, tmp)
                    float_cols = fa | fb
                else:
                    skipped.append(rel)
                    continue
            except Exception as e:  # noqa: BLE001 -- an unparsable pair is a finding, not a crash
                cells += 1
                findings.append(f"{rel}: could not be parsed ({type(e).__name__}: {e})")
                report_rows.append({"file": rel, "row": "-", "col": "parse",
                                    "stata": type(e).__name__, "piet": str(e)[:60],
                                    "verdict": "DIFF"})
                continue
            c, s, f, fnd = compare_table(rel, ha, ra, hb, rb, rtol, float_cols,
                                         display_gate=(ext != ".dta"),
                                         rows_out=report_rows)
            cells += c
            same += s
            flips += f
            findings.extend(fnd)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if skipped:
        print(f"[repdiff] {len(skipped)} file(s) skipped -- no parser for the extension: "
              + ", ".join(skipped[:12]))
    diff = cells - same
    if report_path:
        write_report(Path(report_path), report_rows, sdir, pdir, tier,
                     cells, same, flips, ignored, skipped)
    if findings:
        print(f"REPLICATION DIFF RED (tier {tier}, rtol {rtol}):")
        for f in findings[:200]:
            print("  - " + f)
        if len(findings) > 200:
            print(f"  ... {len(findings) - 200} more")
        print(f"VERDICT: RED cells={cells} same={same} diff={diff} boundary_flips={flips}")
        return 1
    print(f"VERDICT: GREEN cells={cells} same={same} diff={diff} boundary_flips={flips}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
