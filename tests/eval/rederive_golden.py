#!/usr/bin/env python3
"""rederive_golden.py — replace self-captured golden values with real-Stata captures (BUG-227 tranche 1).

Reads a Stata capture log containing lines of the form

    ORACLE|<golden csv basename>|<key>|<%21.15e value>

(the `*_oracle.do` convention under tests/eval/golden/), rewrites the named
key(s) in tests/eval/golden/<csv> IN PLACE (key order preserved, %.15e
formatting as the existing goldens use), and prints an adjudication table:

    key · old golden · new oracle · cobra got (from report.json) ·
    relerr(cobra vs oracle) · manifest rtol · verdict

A cobra-vs-oracle relerr beyond the manifest tolerance is a FINDING, not a
capture error (TOLERANCE.md §4.3): it is printed as GAP and must be filed in
the BUGLOG and laundered via a measured-guilty tolnote — never tuned away.

Usage: python3 tests/eval/rederive_golden.py LOG [LOG...] [--dry-run]
"""
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden"


def stata_hexfloat(s):
    """Parse Stata %21x output, e.g. '+1.91eb851eb851fX+001' -> 3.14 (exact double)."""
    s = s.strip()
    sign = -1.0 if s[0] == "-" else 1.0
    mant, exp = s.lstrip("+-").split("X")
    return sign * float.fromhex("0x" + mant + "p0") * 2.0 ** int(exp, 16)


def parse_logs(paths):
    """-> {csv: {key: (float, precise)}} from ORACLE| (%21.15e) and ORACLEX| (%21x
    hexfloat) lines. An ORACLEX line for the same key wins: it carries the full
    IEEE double, needed where closed-form quantities may legitimately be
    bit-identical across implementations. precise=True keys are written at
    17-significant-digit round-trip precision."""
    out = {}
    for p in paths:
        for line in Path(p).read_text(errors="replace").splitlines():
            line = line.strip()
            if line.startswith("ORACLEX|"):
                _, csv, key, val = line.split("|", 3)
                out.setdefault(csv, {})[key] = (stata_hexfloat(val), True)
            elif line.startswith("ORACLE|"):
                _, csv, key, val = line.split("|", 3)
                out.setdefault(csv, {}).setdefault(key, (float(val), False))
    return out


def load_context():
    man = json.loads((HERE / "manifest.json").read_text())["cases"]
    rep_path = HERE / "report.json"
    rep = json.loads(rep_path.read_text()) if rep_path.exists() else []
    repcases = rep if isinstance(rep, list) else rep.get("cases", rep.get("results", []))
    got = {}     # (csv, csv_key) -> cobra got
    tol = {}     # (csv, csv_key) -> (rtol, atol, case_id)
    rep_by_id = {c.get("id"): c for c in repcases if isinstance(c, dict)}
    for case in man:
        csv = case.get("golden_csv")
        if not csv:
            continue
        rc = rep_by_id.get(case["id"], {})
        rep_checks = {ch.get("q"): ch for ch in rc.get("checks", []) if isinstance(ch, dict)}
        for ch in case.get("checks", []):
            key = ch.get("gkey") or ch["q"]
            tol[(csv, key)] = (ch.get("rtol"), ch.get("atol", 0.0), case["id"])
            r = rep_checks.get(ch["q"])
            if r and "got" in r:
                got[(csv, key)] = r["got"]
    return got, tol


def relerr(a, b):
    if a == b:
        return 0.0
    d = abs(a - b)
    return d / max(abs(a), abs(b)) if max(abs(a), abs(b)) else d


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    oracle = parse_logs(args)
    got, tol = load_context()
    any_gap = False
    for csv, kv in sorted(oracle.items()):
        path = GOLDEN / csv
        lines = path.read_text().splitlines()
        keys_seen = set()
        out = []
        for line in lines:
            # golden keys may themselves contain commas (cell[g2,t2].att) — the
            # VALUE is always the last comma-separated field, so split from the right.
            k = line.rsplit(",", 1)[0] if "," in line else line
            if k in kv:
                keys_seen.add(k)
                v, precise = kv[k]
                out.append(f"{k},{v:.16e}" if precise else f"{k},{v:.15e}")
            else:
                out.append(line)
        missing = set(kv) - keys_seen
        print(f"== {csv}")
        for k in [line.rsplit(',', 1)[0] for line in lines if line.rsplit(',', 1)[0] in kv]:
            old = float([line for line in lines if line.rsplit(',', 1)[0] == k][0].rsplit(",", 1)[1])
            new = kv[k][0]
            g = got.get((csv, k))
            rt, at, cid = tol.get((csv, k), (None, None, "?"))
            moved = relerr(old, new)
            if g is not None and rt is not None:
                re_c = relerr(g, new)
                ok = re_c <= rt or (at and abs(g - new) <= at)
                verdict = "ok" if ok else "GAP"
                if not ok:
                    any_gap = True
                print(f"  {k:42s} old={old:.15e} new={new:.15e} moved={moved:.2e} "
                      f"cobra={g:.15e} relerr={re_c:.2e} rtol={rt} [{verdict}] ({cid})")
            else:
                print(f"  {k:42s} old={old:.15e} new={new:.15e} moved={moved:.2e} "
                      f"(unchecked key — provenance only)")
        for k in sorted(missing):
            print(f"  {k}: NOT IN {csv} — skipped (new key refusal; goldens are append-only by review)")
        if not dry:
            path.write_text("\n".join(out) + "\n")
    if any_gap:
        print("FINDING(S) present: file in BUGLOG + measured-guilty tolnote; do NOT tune the golden.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
