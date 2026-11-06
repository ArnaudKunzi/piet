#!/bin/sh
# Create or repair the gate interpreter venv `.venv-gate` (see tools/gatepy).
# Idempotent. Uses the newest python3 that brew provides, then pins numpy inside the venv so a
# later brew bump of the symlink cannot reach it.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
base="${VENV_GATE_BASE_PYTHON:-}"
if [ -z "$base" ]; then
  for c in /opt/homebrew/bin/python3.13 /opt/homebrew/bin/python3.14 /opt/homebrew/bin/python3 /usr/bin/python3; do
    [ -x "$c" ] && { base="$c"; break; }
  done
fi
[ -n "$base" ] || { echo "make_venv_gate: no python3 found" >&2; exit 2; }
if [ ! -x "$here/.venv-gate/bin/python3" ]; then
  "$base" -m venv "$here/.venv-gate"
fi
"$here/.venv-gate/bin/python3" -m pip install --quiet --upgrade pip >/dev/null
"$here/.venv-gate/bin/python3" -m pip install --quiet 'numpy>=1.26'
# openpyxl: tools/replication_diff.py reads .xlsx replication tables with it, and gatepy now
# refuses to run without it (an undeclared import is an ENVIRONMENT FAULT waiting to be read as
# a content verdict). pandas: already used by the harness but UNDECLARED here until 2026-09-07,
# so a venv rebuild silently dropped it — measured installed at 3.0.5 with no line asking for it.
"$here/.venv-gate/bin/python3" -m pip install --quiet 'openpyxl>=3.1'
"$here/.venv-gate/bin/python3" -m pip install --quiet 'pandas>=2.2'
"$here/tools/gatepy" --where
