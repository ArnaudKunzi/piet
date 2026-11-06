#!/usr/bin/env bash
#==============================================================================
# run_demos.sh — durability gate for the demo kits under demos/<name>/.
#
# Runs EVERY demos/*/*.do headless through a shipped piet_native binary and
# fails if any produces an error block. This is release-checklist step 5b
# (VERSIONING.md): a demo that no longer runs GREEN blocks the release.
#
# Usage:
#   demos/run_demos.sh [PIET_NATIVE_BIN]
#
#   PIET_NATIVE_BIN  Optional path to a piet_native binary. If omitted, the
#                    newest releases/product/*/PIET.app/Contents/MacOS/piet_native
#                    is auto-discovered.
#
# Each kit runs with cwd = its own directory, so a kit's .do can load its data
# with a relative path (e.g. `use "fatalities.csv"`).
#
# Dependencies: bash + python3 only (python3 just parses the JSON the binary
# emits — no third-party packages).
#
# Exit: 0 iff every .do in every kit ran to completion with zero error blocks.
#==============================================================================
set -u

# --- locate this script's directory (demos/) and the repo root ---------------
DEMOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$DEMOS_DIR/.." && pwd)"

# --- resolve the piet_native binary ------------------------------------------
BIN="${1:-}"
if [ -z "$BIN" ]; then
  # Releases live in the private coordination tree: a SIBLING directory of the
  # checkout that carries `releases/CURRENT` and `claims/`. A public checkout
  # has no such tree, so auto-discovery finds nothing and the binary must be
  # passed explicitly.
  #
  # BUG-209 (an internal bug ledger entry, the hop-counting class): fixed `..`
  # hops from the repo root break for checkouts nested one level deeper (a
  # worktree pool). Search UPWARD instead, using the same semantics as
  # piet/native/include/piet/coord_root.h: at each ancestor, probe every
  # immediate child directory and qualify it only by structure (a regular file
  # `releases/CURRENT` AND a directory `claims/`). This site failed LOUDLY
  # rather than silently, so it was the least harmful member of the class —
  # but it still could not find the root from a pool worktree.
  demos_coord_root() {
    local dir="$1" max_depth=10 depth=0 parent d found=""
    while [ "$depth" -le "$max_depth" ]; do
      [ "$dir" = "/" ] && break
      parent="$(cd "$dir/.." && pwd)"
      [ "$parent" = "$dir" ] && break
      for d in "${parent%/}"/*/; do
        d="${d%/}"
        [ -d "$d" ] || continue          # unmatched glob stays literal
        [ "$d" = "$dir" ] && continue    # the checkout / ancestor we came from
        # Walk the full bound rather than returning on the first hit, so a second
        # qualifying tree further up is reported instead of silently shadowed.
        if [ -f "$d/releases/CURRENT" ] && [ -d "$d/claims" ]; then
          if [ -n "$found" ] && [ "$found" != "$d" ]; then
            echo "FATAL: ambiguous coordination trees (releases/CURRENT + claims/) above $1 — refusing to guess: $found AND $d" >&2
            exit 1
          fi
          found="$d"
        fi
      done
      dir="$parent"
      depth=$((depth + 1))
    done
    [ -n "$found" ] && echo "$found"
  }
  COORD_ROOT="$(demos_coord_root "$REPO_ROOT")"
  # newest releases/product/*/PIET.app binary, by version-sorted directory name.
  for REL_GLOB in \
      ${COORD_ROOT:+"$COORD_ROOT/releases/product"}; do
    [ -d "$REL_GLOB" ] || continue
    BIN="$(ls -d "$REL_GLOB"/*/PIET.app/Contents/MacOS/piet_native 2>/dev/null | sort -V | tail -1)"
    [ -n "$BIN" ] && break
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "FATAL: no usable piet_native binary." >&2
  echo "  Pass one explicitly:  demos/run_demos.sh /path/to/PIET.app/Contents/MacOS/piet_native" >&2
  echo "  (auto-discovery looked under releases/product/*/PIET.app/…)" >&2
  exit 2
fi

echo "demos gate — binary: $BIN"
echo "----------------------------------------------------------------------"
printf "%-28s %-24s %s\n" "KIT" "DO-FILE" "VERDICT"
echo "----------------------------------------------------------------------"

# --- python one-liner: read the --do JSON, report error blocks ---------------
# Prints "GREEN" or "RED <n> error(s): <first message>"; exit code mirrors it.
read -r -d '' PYCHK <<'PY'
import json,sys
try:
    d=json.load(open(sys.argv[1]))
except Exception as e:
    print("RED  unreadable JSON: %s" % e); sys.exit(1)
errs=[b for b in d.get("blocks",[]) if isinstance(b,dict) and b.get("type")=="error"]
reason=d.get("stopped_at_reason")
if errs or reason:
    n=len(errs)
    first=(errs[0].get("message") if errs else reason) or reason or "?"
    print("RED  %d error block(s); %s" % (n, str(first)[:100]))
    sys.exit(1)
print("GREEN")
sys.exit(0)
PY

overall=0
found=0
# iterate kits deterministically
for do_file in $(ls "$DEMOS_DIR"/*/*.do 2>/dev/null | sort); do
  found=1
  kit_dir="$(dirname "$do_file")"
  kit="$(basename "$kit_dir")"
  do_name="$(basename "$do_file")"
  json_out="$(mktemp -t demogate.XXXXXX)"
  # cwd = kit dir so relative data paths resolve; discard stderr (workdir note).
  ( cd "$kit_dir" && "$BIN" --do "$do_name" ) >"$json_out" 2>/dev/null
  bin_rc=$?
  verdict="$(python3 -c "$PYCHK" "$json_out")"
  rc=$?
  # A clean-looking transcript does not clear the engine of a nonzero exit —
  # a teardown/atexit crash after a full, error-free flush must still fail.
  if [ $bin_rc -ne 0 ] && [ $rc -eq 0 ]; then
    verdict="RED  engine exited $bin_rc after a clean transcript (teardown crash?)"
    rc=1
  fi
  printf "%-28s %-24s %s\n" "$kit" "$do_name" "$verdict"
  rm -f "$json_out"
  [ $rc -ne 0 ] && overall=1
done

echo "----------------------------------------------------------------------"
if [ "$found" -eq 0 ]; then
  echo "FATAL: no demos/*/*.do files found." >&2
  exit 2
fi
if [ "$overall" -eq 0 ]; then
  echo "ALL DEMOS GREEN"
else
  echo "DEMOS RED — a kit no longer runs clean (see table above)"
fi
exit $overall
