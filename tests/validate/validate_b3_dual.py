#!/usr/bin/env python3
"""validate_b3_dual — the B3 law, as a gate.

A Stata expression function lives in TWO evaluators that never call each other:
cobra's `frame.cpp` (what `generate`/`replace`/`keep if` run, reached from PIET as
a subprocess) and PIET's own pyeval registry (what `display`/`scalar`/`local` run).
Landing a name in one and not the other is not a compile error and not a runtime
warning -- it is either a silent all-missing column or a do-file that STOPS with
"NameError". BUG-431's `mi()` shipped exactly that way and every gate stayed green,
because its probe only exercised `gen`.

So this gate asks all three questions at once, per expression:

    engine value  ==  PIET value  ==  the StataBE value pinned in
                                      tests/validate/b3_funcs_oracle.json

Two of the three would not be enough. engine==PIET with no oracle certifies that
both layers are wrong in the same way; oracle==engine with no PIET is the BUG-431
shape. It also checks the two censuses moved together, since a function can be
implemented and still be invisible to `cobra functions` / `--dump-functions`.

Usage:
    tools/gatepy tests/validate/validate_b3_dual.py
    COBRA_BIN=... PIET_BIN=... tools/gatepy tests/validate/validate_b3_dual.py

Exit 0 GREEN, 1 RED, 2 ENVIRONMENT FAULT (a binary or the oracle is missing --
never a verdict, per the gatepy convention).
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(HERE, "b3_funcs_oracle.json")

# (expression, oracle key). Both layers are asked for the SAME expression text so
# a divergence cannot be blamed on the two sides being asked different questions.
# The negative-period cases are here on purpose: they are what a truncating
# integer division gets wrong, and 31dec1959 is period -1 on every Stata scale.
CASES = [
    ("ym(2020,3)", "ym.2020.3"), ("ym(1960,1)", "ym.1960.1"),
    ("yq(2020,1)", "yq.2020.1"), ("yw(2020,10)", "yw.2020.10"),
    ("yh(2020,1)", "yh.2020.1"),
    ("mofd(21989)", "mofd"), ("qofd(21989)", "qofd"), ("yofd(21989)", "yofd"),
    ("wofd(21989)", "wofd"), ("hofd(21989)", "hofd"), ("week(21989)", "week.d"),
    ("dofm(722)", "dofm"), ("dofq(240)", "dofq"), ("dofy(2020)", "dofy.2020"),
    ("dofw(3129)", "dofw"), ("dofh(120)", "dofh"),
    ("cofd(21989)", "cofd"), ("Cofd(21989)", "Cofd"),
    ("dofc(1899899156000)", "dofc"), ("dofC(1899899183000)", "dofC"),
    ("hh(1899899156000)", "hh.tc"), ("mm(1899899156000)", "mm.tc"),
    ("ss(1899899156000)", "ss.tc"),
    ("hhC(1899899183000)", "hhC.tC"), ("mmC(1899899183000)", "mmC.tC"),
    ("ssC(1899899183000)", "ssC.tC"),
    ("hms(13,45,56)", "hms"), ("dhms(21989,13,45,56)", "dhms"),
    ("msofseconds(90)", "msofseconds"), ("msofminutes(1.5)", "msofminutes"),
    ("msofhours(1)", "msofhours"), ("seconds(90000)", "seconds"),
    ("minutes(90000)", "minutes"), ("hours(3600000)", "hours"),
    ("mofd(-1)", "mofd.neg"), ("yofd(-1)", "yofd.neg"), ("wofd(-1)", "wofd.pre"),
    ("hofd(-1)", "hofd.pre"), ("dofm(-1)", "dofm.pre"), ("dofq(-1)", "dofq.pre"),
    ("dofw(-1)", "dofw.pre"), ("dofh(-1)", "dofh.pre"), ("dofy(1959)", "dofy.1959"),
]

# B3 stage (b): the literal pseudo-functions. These are resolved in cobra's
# PARSER and in PIET's LEXER -- two separate pieces of code reading one shared
# table -- so they are exactly the kind of thing that drifts, and exactly what
# this gate is for. Colons are left out here on purpose: tc(12:00) is correct
# through `gen` and refused on the display surface (BUG-446), which is a surface
# limitation this gate should not re-report.
LITERAL_CASES = [
    ("td(15mar2020)", "td.mar2020"), ("td(01jan1960)", "td.epoch"),
    ("td(31dec1959)", "td.pre"), ("d(15mar2020)", "d.syn"),
    ("tm(2020m3)", "tm.2020m3"), ("m(2010m4)", "L.m"),
    ("tq(2020q1)", "tq.2020q1"), ("q(2007q1)", "L.q"),
    ("tw(2020w10)", "tw.2020w10"), ("th(2020h1)", "th.2020h1"),
    ("y(2020)", "y.syn"),
]

# Names that reached cobra's FNS[] and no PIET layer, so `display <f>(x)` aborted
# the do-file. No oracle key: the point is that both layers now ANSWER, and agree.
ASYMMETRY_REPAIRS = ["mi(1)", "int(3.7)", "sin(0)", "cos(0)", "tan(0)",
                     "asinh(0)", "ln1p(0)", "ln1m(0)", "ln(1)"]


def fault(msg):
    print(f"ENVIRONMENT FAULT: {msg}", file=sys.stderr)
    sys.exit(2)


def hex_to_double(h):
    """Stata's %21x: +1.<13 hex digits>X<sign><hex exponent>. This is the stored
    double, which is the authority; the %18.0g column next to it is a display,
    and a display is the thing that has been the defect before (BUG-406)."""
    h = h.strip()
    if h.startswith("+0.0000000000000X"):
        return 0.0
    m = re.match(r"^([+-])1\.([0-9a-f]{13})X([+-])([0-9a-f]+)$", h)
    if not m:
        return None
    sign = -1.0 if m.group(1) == "-" else 1.0
    exp = int(m.group(4), 16) * (-1 if m.group(3) == "-" else 1)
    return sign * (1.0 + int(m.group(2), 16) / float(1 << 52)) * (2.0 ** exp)


def main():
    cobra = os.environ.get("COBRA_BIN", os.path.join(REPO, "build", "cobra"))
    piet = os.environ.get("PIET_BIN",
                          os.path.join(REPO, "piet", "native", "build", "piet_native"))
    for p in (cobra, piet):
        if not os.path.isfile(p) or not os.access(p, os.X_OK):
            fault(f"binary not found or not executable: {p}")
    if not os.path.isfile(ORACLE):
        fault(f"oracle not found: {ORACLE} (regenerate with b3_funcs_oracle.do)")
    records = json.load(open(ORACLE))["records"]
    residual = {}  # index -> pinned value the PIET side was asked to subtract

    exprs = [e for e, _ in CASES] + [e for e, _ in LITERAL_CASES] + ASYMMETRY_REPAIRS
    lines = ["clear", "set obs 1"]
    lines += [f"gen double g{i} = {e}" for i, e in enumerate(exprs)]     # ENGINE
    # PIET is asked for the RESIDUAL against the pinned value, not for the value.
    # A bare `display` renders at ~7 significant digits, so `display cofd(21989)`
    # prints 1.9e12 for a correctly stored 1899849600000 -- and Stata does the same,
    # so it is not a defect, just an instrument that cannot express what is being
    # compared. (An earlier version of this comment blamed PIET for dropping the
    # format spec; re-measured, %20.0f and %18.0g are honoured correctly and only
    # %21x is not -- BUG-444.) A residual of exactly 0 tests the full stored double
    # through a display that CAN express it. The literals are integers below 2^53,
    # so the parser reads them into the same double the oracle captured.
    all_keyed = CASES + LITERAL_CASES
    for i, e in enumerate(exprs):
        key = all_keyed[i][1] if i < len(all_keyed) else None
        want = hex_to_double(records[key]["hex"]) if key and "hex" in records.get(key, {}) else None
        if want is not None and abs(want) >= 1e7:
            lines.append(f'display "P|{i}|" (({e}) - ({want:.0f}))')
            residual[i] = want
        else:
            lines.append(f'display "P|{i}|" ({e})')
    lines.append("list, noobs")

    with tempfile.TemporaryDirectory() as td:
        do = os.path.join(td, "b3_dual.do")
        open(do, "w").write("\n".join(lines) + "\n")
        env = dict(os.environ, COBRA_BIN=cobra, PIET_WORKDIR=td)
        # BUG-388: a harness that drives a PIET session pins its OWN settings file.
        # Grading the product under the user's live settings.json makes the verdict
        # depend on a file the gate does not control -- the whole point of the
        # engine_arm audit leg, which caught this gate the first time it ran.
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import engine_arm  # noqa: E402
        engine_arm.pin_settings(env, os.path.join(td, "gate_settings"))
        try:
            out = subprocess.run([piet, "--do", do], capture_output=True, text=True,
                                 env=env, timeout=900).stdout
        except subprocess.TimeoutExpired:
            fault("piet_native --do did not finish in 900 s")

    i = out.find('{"blocks"')
    if i < 0:
        fault("piet_native produced no JSON envelope")
    doc = json.loads(out[i:])

    # A do-file that STOPS is the BUG-431 failure shape itself, so it is RED here
    # rather than a missing-value comparison further down.
    stopped = doc.get("stopped_at_line")
    errors = [b.get("message") for b in doc["blocks"] if b.get("type") == "error"]

    piet_vals, eng_vals = {}, {}
    for b in doc["blocks"]:
        if b.get("type") == "display":
            m = re.match(r"^P\|(\d+)\|(.*)$", b.get("text", ""))
            if m:
                t = m.group(2).strip()
                try:
                    piet_vals[int(m.group(1))] = float(t)
                except ValueError:
                    piet_vals[int(m.group(1))] = t
        elif b.get("type") == "browse" and b.get("rows"):
            names = [c["name"] for c in b["columns"]]
            for n, v in zip(names, b["rows"][0]):
                if n.startswith("g"):
                    eng_vals[int(n[1:])] = v

    problems = []
    if stopped:
        problems.append(f"do-file STOPPED at line {stopped}: "
                        f"{doc.get('stopped_at_reason')!r}")
    for e in errors:
        problems.append(f"error block: {e!r}")

    for i, expr in enumerate(exprs):
        key = all_keyed[i][1] if i < len(all_keyed) else None
        e, p = eng_vals.get(i), piet_vals.get(i)
        if e is None:
            problems.append(f"{expr}: engine produced no column")
            continue
        if p is None:
            problems.append(f"{expr}: PIET produced no display (one-layer landing)")
            continue
        if not isinstance(p, float):
            problems.append(f"{expr}: PIET answered non-numeric {p!r}")
            continue
        p_abs = p + residual[i] if i in residual else p
        if e != p_abs:
            shown = f"{p!r} (residual against {residual[i]:.0f})" if i in residual else repr(p)
            problems.append(f"{expr}: LAYERS DIFFER engine={e!r} PIET={shown}")
        if key is not None:
            rec = records.get(key)
            if rec is None or "hex" not in rec:
                problems.append(f"{expr}: no oracle record for key {key!r}")
                continue
            want = hex_to_double(rec["hex"])
            if want is None:
                problems.append(f"{expr}: oracle hex unparseable {rec['hex']!r}")
            elif e != want:
                problems.append(f"{expr}: vs StataBE want={want!r} got={e!r}")

    # The censuses are separate from the implementations and can lag them: a
    # function can work and still be invisible to a consumer reading the dumps.
    try:
        eng_names = {ln.split("\t")[0] for ln in subprocess.run(
            [cobra, "functions"], capture_output=True, text=True, timeout=900
        ).stdout.splitlines() if ln.strip()}
        piet_names = {r["name"] for r in json.loads(subprocess.run(
            [piet, "--dump-functions"], capture_output=True, text=True, timeout=900
        ).stdout)}
    except subprocess.TimeoutExpired:
        fault("a census dump did not finish in 900 s")
    # Both-layer names must appear in BOTH censuses; the two deliberate
    # single-layer ones are checked on the side they actually live.
    for name in ("ym", "mofd", "dofc", "hh", "hhC", "hms", "dhms", "cofd", "Cofd",
                 "msofseconds", "hours", "week",
                 # the parser/lexer layers, which a table-only census cannot see
                 "td", "tq", "tm", "d", "q"):
        if name not in eng_names:
            problems.append(f"census: {name} missing from `cobra functions`")
        if name not in piet_names:
            problems.append(f"census: {name} missing from `--dump-functions`")
    for name in ("mi", "int"):          # repaired PIET-side one-layer landings
        if name not in piet_names:
            problems.append(f"census: {name} missing from `--dump-functions`")
    for name in ("tin", "twithin"):     # engine-only: tsset lives there, and
        if name not in eng_names:       # `count if` bails out of PIET to cobra
            problems.append(f"census: {name} missing from `cobra functions`")

    n = len(exprs)
    if problems:
        for p in problems:
            print(f"  {p}")
        print(f"VERDICT: RED  ({n} expressions, {len(problems)} problem(s))")
        return 1
    print(f"VERDICT: GREEN  engine == PIET == StataBE on {len(all_keyed)} pinned "
          f"expressions ({len(LITERAL_CASES)} of them parser/lexer literals), plus "
          f"{len(ASYMMETRY_REPAIRS)} both-layers-answer checks; censuses carry every name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
