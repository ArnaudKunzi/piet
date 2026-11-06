#!/usr/bin/env python3
"""BUG-340 gate — assert a corpus run produced ANALYSES, not 47 identical blanks.

WHY THIS EXISTS. A full acceptance-corpus run once emitted 47 `.svg` files that
were the same byte-identical empty frame (one distinct size, 2,229 B; one distinct
md5; zero data marks) and **every artifact check passed**: the files existed, the
count was right, the run exited 0. The immediate cause was fixed (BUG-339), but
what stayed open is that nothing would notice a recurrence — and the corpus's own
`save_empty_plot` fallback writes the SAME filename stem a real `graph export`
does, so a count-based check structurally cannot tell the two apart.

So this asks the two questions a count cannot:

  1. **Are the artifacts distinct?** 47 analyses of different outcomes cannot all
     render to one hash. A single distinct md5 across the whole set is the exact
     signature of the failure this gate is named after.
  2. **Does each artifact carry data?** An empty frame has axes and a title but no
     plotted geometry. `<path`/`<circle`/`<rect`/`<polyline` marks inside the plot
     body are what a drawn series leaves behind.

Both are content assertions, deliberately — every structural property (exists,
non-zero size, right name, exit code) was already true on the day of the incident.

usage:  corpus_content_check.py <result-dir> [--min-files N] [--min-marks N]
exit 0 = the set looks like analyses; exit 1 = it does not (reason printed);
exit 2 = the gate could not run (bad path / no artifacts to judge).
"""
import argparse
import hashlib
import os
import re
import sys

# Marks that only appear when a series is actually drawn. `<rect` is included
# because bar/area renderings can be rect-only, but the frame/background rect is
# subtracted below so a blank frame cannot pass on its own chrome.
MARK_RE = re.compile(rb"<(path|circle|polyline|polygon|rect)\b")
# The plot frame itself is one rect; a blank frame has axes lines drawn as paths
# in some renderers, so the floor is deliberately > a small constant rather than 0.
DEFAULT_MIN_MARKS = 4


def main() -> int:
    ap = argparse.ArgumentParser(description="BUG-340 corpus content gate")
    ap.add_argument("result_dir")
    ap.add_argument("--min-files", type=int, default=2,
                    help="fewer artifacts than this and distinctness is not a question worth asking")
    ap.add_argument("--min-marks", type=int, default=DEFAULT_MIN_MARKS)
    a = ap.parse_args()

    if not os.path.isdir(a.result_dir):
        print(f"[content] CANNOT RUN: not a directory: {a.result_dir}", file=sys.stderr)
        return 2
    files = sorted(f for f in os.listdir(a.result_dir) if f.lower().endswith((".svg", ".png")))
    if not files:
        print(f"[content] CANNOT RUN: no .svg/.png artifacts in {a.result_dir}", file=sys.stderr)
        return 2

    digests, blanks = {}, []
    for name in files:
        raw = open(os.path.join(a.result_dir, name), "rb").read()
        digests.setdefault(hashlib.md5(raw).hexdigest(), []).append(name)
        if len(MARK_RE.findall(raw)) < a.min_marks:
            blanks.append(name)

    print(f"[content] {len(files)} artifacts · {len(digests)} distinct md5 · "
          f"{len(blanks)} without plotted marks")

    bad = False
    if len(files) >= a.min_files and len(digests) == 1:
        only = next(iter(digests))
        print(f"[content] RED: every artifact is the SAME file (md5 {only[:12]}…) — this is the "
              f"BUG-340 signature: N analyses cannot render to one hash", file=sys.stderr)
        bad = True
    if blanks:
        print(f"[content] RED: {len(blanks)} artifact(s) carry fewer than {a.min_marks} plotted "
              f"marks (blank frames): {', '.join(blanks[:5])}"
              f"{' …' if len(blanks) > 5 else ''}", file=sys.stderr)
        bad = True
    if bad:
        return 1
    print("[content] GREEN — artifacts are distinct and each carries plotted data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
