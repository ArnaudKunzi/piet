#!/usr/bin/env python3
"""Shared engine-binary resolution for the `tests/validate/` ctest gates.

Resolution order — first one that is SET wins, and if what it names is missing
we stop rather than look further:

    1. argv[1]          (how ctest invokes these: `$<TARGET_FILE:cobra>`)
    2. $COBRA_BIN       (the convention every other gate in GATES.md uses)
    3. <repo>/build/cobra
    4. hard error

**This deliberately never falls back to a bare `cobra` on $PATH.** These files
used to end with `else "cobra"`, and on 2026-07-21 that silently resolved to an
unrelated `/usr/local/bin/cobra` when an operator invoked the gate the way
GATES.md documents every *other* gate — `COBRA_BIN=<path> python3 <gate>.py`.
The gate then reported a contract feature as absent while testing a completely
different build, and it was only caught by reading this line.

A gate that tests a different binary than the caller intended is worse than a
gate that refuses to run, because its output looks like a result.

Note the "first SET wins" rule: if you pass an explicit path that does not
exist, you get an error naming it, not a silent downgrade to some other engine.
Falling through would re-introduce exactly the bug this file exists to prevent.
"""
import os
import sys
from pathlib import Path

# tests/validate/_cobra_bin.py -> <repo root>
ROOT = Path(__file__).resolve().parents[2]


def resolve_cobra(argv=None):
    """Return an absolute path to the cobra binary, or exit with a clear error."""
    argv = sys.argv if argv is None else argv

    if len(argv) > 1 and argv[1]:
        cand, src = argv[1], "argv[1]"
    elif os.environ.get("COBRA_BIN"):
        cand, src = os.environ["COBRA_BIN"], "$COBRA_BIN"
    else:
        cand, src = str(ROOT / "build" / "cobra"), "repo default <root>/build/cobra"

    path = Path(cand)
    if not path.is_file() or not os.access(path, os.X_OK):
        sys.exit(
            f"FATAL: cobra binary not usable: {cand}\n"
            f"  resolved from: {src}\n"
            f"  pass the binary as argv[1], or set COBRA_BIN=<path>.\n"
            f"  This gate will NOT fall back to a `cobra` on $PATH — testing an\n"
            f"  unintended binary silently is the failure mode this check exists for."
        )
    return str(path.resolve())
