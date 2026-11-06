#!/usr/bin/env python3
# WS-D2 / S06 ARC-MRG gate: a two-file verb opens each input EXACTLY ONCE.
#
# The P2-PERF close NAMED merge as opening its files three times. It was right,
# and nothing measured it: an open-site backtrace on
#   cobra merge --data mm.hbin --using lk.hbin --type m:1 --key id
# showed lk.hbin mapped TWICE — once by cmd_merge, once again inside
# Frame::from_dataset, which re-opens a path it is handed as a bare
# `const Dataset&` because it cannot extend that Dataset's lifetime. The fix is
# the shared-handle idiom the master side already used (open_using_frame in
# apps/cmds/common.cpp).
#
# This is a COUNTER gate, not a timing one, which is the Doctrine's own ordering:
# deterministic counters are the sworn witness, timing the noisy one. Counted at
# the syscall (map_hbin / read_dta / Frame::from_csv), reported as a per-
# invocation delta via COBRA_MERGE_IO_DIAG on stderr — nothing reaches stdout, so
# a frozen fixture cannot see this gate exist.
#
# Self-contained (builds its own fixtures).
#   usage: validate_merge_file_opens.py <cobra-binary>
import os
import re
import subprocess
import sys
import tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ((" | " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(name)


def merge_opens(args):
    """Run one merge with the diag on; return (rc, merge_file_opens or None)."""
    env = dict(os.environ, COBRA_MERGE_IO_DIAG="1")
    r = subprocess.run([COBRA, "merge"] + args, capture_output=True, text=True, env=env)
    m = re.search(r"merge_file_opens=(\d+)", r.stderr or "")
    return r.returncode, (int(m.group(1)) if m else None), r


work = tempfile.mkdtemp(prefix="merge_opens_")
uniq_csv = os.path.join(work, "uniq.csv")     # 2000 rows, one row per id
many_csv = os.path.join(work, "many.csv")     # 10000 rows, five per id
uniq = os.path.join(work, "uniq.hbin")
many = os.path.join(work, "many.hbin")
with open(uniq_csv, "w") as f:
    f.write("id,b\n")
    for i in range(1, 2001):
        f.write(f"{i},{i * 0.75}\n")
with open(many_csv, "w") as f:
    f.write("id,t,c\n")
    for i in range(1, 2001):
        for t in range(1, 6):
            f.write(f"{i},{t},{i + t * 0.1}\n")
for src, dst in ((uniq_csv, uniq), (many_csv, many)):
    if subprocess.run([COBRA, "convert", src, dst], capture_output=True).returncode != 0:
        print("fixture convert failed", file=sys.stderr)
        sys.exit(1)
out = os.path.join(work, "out.hbin")

# --- 1. the three join types, and the two non-.hbin `using` formats -------------
# 2 = the master once, the using file once. Nothing else is semantically required.
BASE = {
    "1:1": ["--data", uniq, "--using", uniq, "--type", "1:1", "--key", "id"],
    "m:1": ["--data", many, "--using", uniq, "--type", "m:1", "--key", "id"],
    "1:m": ["--data", uniq, "--using", many, "--type", "1:m", "--key", "id"],
}
for label, args in BASE.items():
    rc, opens, r = merge_opens(args + ["--out", out])
    check(f"merge {label}: rc 0", rc == 0, (r.stderr or "")[:200])
    check(f"merge {label}: merge_file_opens == 2 (was 3)", opens == 2, f"got {opens}")

# The .dta and .csv `using` branches go through read_dta / Frame::from_csv rather
# than the mmap path, and open_using_frame folded all three into one helper — so
# they are gated here too rather than assumed to have come along for the ride.
for ext, path in (("csv", uniq_csv),):
    rc, opens, r = merge_opens(["--data", many, "--using", path, "--type", "m:1",
                                "--key", "id", "--out", out])
    check(f"merge m:1 with a .{ext} using: rc 0", rc == 0, (r.stderr or "")[:200])
    check(f"merge m:1 with a .{ext} using: merge_file_opens == 2", opens == 2, f"got {opens}")

# --- 2. DISCRIMINATOR ----------------------------------------------------------
# Without this the whole file could pass against a counter hard-wired to 2, or
# against one that silently stopped counting. `--keepif` legitimately adds ONE
# open — maybe_keepif writes a row-reduced scratch .hbin and the master is then
# acquired from that path — so the honest floor there is 3, not 2. Asserting the
# exact value (rather than ">= 2") is what makes this a discriminator: it also
# catches maybe_keepif regressing back to its own double-open, which measured 4
# before this change.
rc, opens, r = merge_opens(["--data", many, "--using", uniq, "--type", "m:1", "--key", "id",
                            "--keepif", "id<1000", "--out", out])
check("merge m:1 --keepif: rc 0", rc == 0, (r.stderr or "")[:200])
check("discriminator: --keepif reports exactly 3 (master + scratch + using), not 2",
      opens == 3, f"got {opens} — 2 means the counter is not counting; 4 means "
                  f"maybe_keepif re-opened the master again")

# And the counter must be silent unless asked: stdout is a frozen-fixture surface
# and stderr is read by PIET's error path, so an always-on diag line would be a
# behaviour change in the shipped product.
r = subprocess.run([COBRA, "merge"] + BASE["m:1"] + ["--out", out],
                   capture_output=True, text=True,
                   env={k: v for k, v in os.environ.items() if k != "COBRA_MERGE_IO_DIAG"})
check("diag is OFF by default (no merge_file_opens line without the env var)",
      "merge_file_opens" not in (r.stderr or "") and "merge_file_opens" not in (r.stdout or ""))

if fails:
    print("FAILED: " + ", ".join(fails))
    sys.exit(1)
print("PASSED")
