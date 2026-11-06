#!/usr/bin/env python3
"""BUG-152 CLI gate: logit/probit accept i. factor-variable terms."""
import json
import os
import subprocess
import sys
import tempfile

from _cobra_bin import resolve_cobra  # noqa: E402


COBRA = resolve_cobra()
fails = []


def check(label, condition):
    print(("  ok   " if condition else "  FAIL ") + label)
    if not condition:
        fails.append(label)


def run(cmd, hbin, xspec):
    proc = subprocess.run(
        [COBRA, cmd, "--data", hbin, "--y", "y", "--x", xspec,
         "--no-provenance"],
        capture_output=True, text=True,
    )
    check(f"{cmd} --x {xspec}: exits 0", proc.returncode == 0)
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr)
        return None
    return json.loads(proc.stdout)


work = tempfile.mkdtemp(prefix="bug152_glm_fv_")
csv_path = os.path.join(work, "fixture.csv")
hbin = os.path.join(work, "fixture.hbin")
with open(csv_path, "w", encoding="utf-8") as stream:
    stream.write("y,x,region,d2,d3\n")
    for i in range(240):
        region = (i % 3) + 1
        x = ((i * 17) % 101 - 50) / 17.0
        # Deliberately non-separated binary outcome with both x and region signal.
        y = 1 if ((i * 7 + region * 3 + (1 if x > 0 else 0)) % 13) < 6 else 0
        stream.write(f"{y},{x:.17g},{region},{int(region == 2)},{int(region == 3)}\n")

conv = subprocess.run([COBRA, "convert", csv_path, hbin], capture_output=True, text=True)
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

for command in ("logit", "probit"):
    factor = run(command, hbin, "x,i.region")
    dummy = run(command, hbin, "x,d2,d3")
    if factor is None or dummy is None:
        continue

    factor_rows = factor["coefs"]
    dummy_rows = dummy["coefs"]
    check(f"{command}: factor expansion names/order",
          [row["name"] for row in factor_rows] ==
          ["x", "2.region", "3.region", "(intercept)"])
    check(f"{command}: plain design keeps intercept last",
          [row["name"] for row in dummy_rows] ==
          ["x", "d2", "d3", "(intercept)"])
    check(f"{command}: factor and explicit-dummy sample agree",
          factor["n_obs"] == dummy["n_obs"] == 240)
    for index, label in enumerate(("x", "level 2", "level 3", "intercept")):
        for key in ("beta", "se", "se_iid"):
            got = factor_rows[index][key]
            want = dummy_rows[index][key]
            check(f"{command}: {label} {key} matches explicit dummy",
                  abs(got - want) <= 1e-10 * max(1.0, abs(want)))

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED (BUG-152 logit/probit factor-variable expansion)")
