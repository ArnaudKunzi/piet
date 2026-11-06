#!/usr/bin/env python3
"""Shared fixture resolution for the `tests/validate/` python gates.

GA sweep 2026-08-29 (leg ⑤). A fresh clone builds both binaries and passes
`--selftest`, then fails 80 of 92 ctest cases with bare FileNotFoundErrors,
because the DATA fixtures these gates read are deliberately not committed (the
repo ships no datasets — .gitignore says so in a privacy comment). The tests are
not broken and the code is fine; the fixtures are simply absent.

A missing fixture and a failing assertion are different facts and must not print
the same way — that is the BUG-362 lesson, one directory over: a gate that
cannot run must SAY it cannot run, not report a failure it never measured.

So: `need(path)` exits 77, which CMakeLists marks as ctest's SKIP_RETURN_CODE.
A stranger then sees honest skips naming exactly which fixture is missing, and a
developer who has them sees the gate run as before.
"""
import os
import sys

SKIP_EXIT = 77


def need(path, what=None):
    """Return `path`, or SKIP the whole gate if it is not there."""
    if os.path.exists(path):
        return path
    name = what or os.path.basename(path)
    print(f"SKIP: fixture not present: {name}\n"
          f"      looked in: {os.path.dirname(os.path.abspath(path))}\n"
          f"      (data fixtures are not committed — see .gitignore; set "
          f"COBRA_VALIDATE_DIR to a directory that has them)")
    sys.exit(SKIP_EXIT)
