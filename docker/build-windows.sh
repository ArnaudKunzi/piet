#!/usr/bin/env bash
# Build cobra for Windows x64 via Docker cross-compile (MinGW-w64 / Debian 12).
# Run from the repo root or from docker/.
#
# Prerequisites: Docker Desktop running (any platform — arm64 Mac is fine).
# Output: dist/windows-amd64/cobra.exe  (~35 MB, self-contained)
#
# Usage:
#   bash docker/build-windows.sh          # build latest
#   bash docker/build-windows.sh --no-cache  # force full rebuild

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$REPO_ROOT/dist/windows-amd64"
DOCKERFILE="$REPO_ROOT/docker/Dockerfile.windows"

echo "=== cobra Windows x64 cross-compile ==="
echo "    repo   : $REPO_ROOT"
echo "    output : $OUTDIR"
echo ""

mkdir -p "$OUTDIR"

docker buildx build \
    "$@" \
    -f "$DOCKERFILE" \
    --target export \
    --output "type=local,dest=$OUTDIR" \
    "$REPO_ROOT"

EXE="$OUTDIR/cobra.exe"
if [ -f "$EXE" ]; then
    SIZE=$(du -sh "$EXE" | cut -f1)
    echo ""
    echo "==> dist/windows-amd64/cobra.exe  $SIZE  OK"
    echo ""
    echo "Next: update cobra.plugin to include bin/cobra-windows-amd64.exe"
    echo "      (copy $EXE into the plugin ZIP as bin/cobra-windows-amd64.exe)"
else
    echo "ERROR: cobra.exe not found in $OUTDIR" >&2
    exit 1
fi
