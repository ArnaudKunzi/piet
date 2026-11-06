#!/usr/bin/env bash
# Assembles the flat Windows bundle staging directory that piet_setup.iss
# compresses into the installer. This is the Windows analogue of
# package_native_app.sh (the macOS .app assembler): same inputs (piet_native binary, piet/frontend/ verbatim, the pinned cobra
# binary, LICENSE.txt, a generated THIRD_PARTY_NOTICES.txt, the sample
# dataset), different output shape (a flat directory Program Files can hold,
# not a .app bundle).
#
# UNLIKE the macOS flow, every input here can be produced ENTIRELY on the Mac
# via Docker cross-compilation (docker/Dockerfile.windows for cobra.exe,
# docker/Dockerfile.piet-windows for piet_native.exe) — no on-Windows freeze
# step is needed anymore (that was a PyInstaller-only constraint; its
# WINDOWS_BUILD.md runbook is deleted — docker/Dockerfile.piet-windows is
# the recipe). Only `iscc` (Inno Setup
# Compiler) still needs to run on Windows (or under Wine), since Inno itself
# has no macOS/Linux build.
#
# scripts/package-windows.sh is the usual way in: it sets the two inputs below
# from a native scripts/build-windows.sh build and calls this script.
#
# Usage (from the repo root, after both Docker cross-builds have produced
# their exports):
#   piet/packaging/stage_native_windows.sh [OUTPUT_DIR]
#
# Inputs (override any of these; defaults match the two Dockerfiles' default
# --output paths):
#   $PIET_NATIVE_EXE   default: dist/windows-amd64-piet/build/piet_native.exe
#   $COBRA_EXE         default: dist/windows-amd64/cobra.exe
#
# OUTPUT_DIR defaults to dist/windows-amd64-piet/PIET-windows (the directory
# piet_setup.iss's DistDir points at by default).
#
# Produces: OUTPUT_DIR/
#   PIET.exe                     <- renamed copy of piet_native.exe
#   LICENSE.txt                  <- root: piet_setup.iss LicenseFile= reads it
#                                   here, and LEGAL-DISTRIBUTION §6 wants the
#                                   EULA visible at {app}\LICENSE.txt
#   THIRD_PARTY_NOTICES.txt      <- root copy: the .iss Start-menu shortcut
#                                   points at {app}\THIRD_PARTY_NOTICES.txt
#   resources\                   <- EVERYTHING the app resolves at runtime.
#     frontend\{index.html,css\,js\}
#     cobra.exe
#     THIRD_PARTY_NOTICES.txt    <- generated: shared licenses/*.txt +
#                                    licenses/windows/*.txt (see that dir's
#                                    README for why Windows gets extra parts
#                                    the macOS bundle does not)
#     sample\fatalities.csv
#   demo\                       <- the Fatalities teaching kit, user-visible:
#     fatalities_tour.do · HANDBOOK.md · fatalities.csv (Stata-side copy)
#     (tour loads via sysuse -> runs from ANY directory; Start-menu shortcut
#      "Fatalities Demo Kit" points here)
#
# The resources\ layer is NOT cosmetic: bundle_paths.cpp's Windows branch
# (bundleResourcesDir, WP-W5) defines "packaged" as `resources\` existing
# beside the exe — the flat analogue of the mac .app's Contents/Resources —
# and frontend (main.cpp resolveFrontendIndexPath), the engine
# (cobra_runner.cpp resolveCobraBin -> resources\cobra.exe), the About/Legal
# notices (routes.cpp legal.notices) and the sample dataset (main.cpp
# dataDefault) ALL resolve through it. The original flat layout shipped in
# the first 2.0.5 Windows artifact set made bundleResourcesDir() return
# "not bundled", so the app fell back to the Docker build machine's
# __FILE__ paths: the piet.invalid virtual-host mapping pointed at a
# nonexistent folder and WebView2 did a real DNS lookup on it —
# ERR_NAME_NOT_RESOLVED on first canary launch (2026-07-08).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"      # .../piet/packaging
WORKTREE="$(cd "${HERE}/../.." && pwd)"                    # repo root

PIET_NATIVE_EXE="${PIET_NATIVE_EXE:-${WORKTREE}/dist/windows-amd64-piet/build/piet_native.exe}"
COBRA_EXE="${COBRA_EXE:-${WORKTREE}/dist/windows-amd64/cobra.exe}"
FRONTEND_SRC="${WORKTREE}/piet/frontend"
LICENSES_DIR="${HERE}/licenses"
LICENSES_WIN_DIR="${LICENSES_DIR}/windows"
SAMPLE_SRC="${HERE}/sample/fatalities.csv"
DEMO_KIT_SRC="${WORKTREE}/demos/fatalities"
LICENSE_SRC="${WORKTREE}/LICENSE.txt"

OUTPUT_DIR="${1:-${WORKTREE}/dist/windows-amd64-piet/PIET-windows}"

# ── Preflight ────────────────────────────────────────────────────────────
[[ -f "${PIET_NATIVE_EXE}" ]] || { echo "FATAL: piet_native.exe not found at ${PIET_NATIVE_EXE} — build it first with docker/Dockerfile.piet-windows" >&2; exit 1; }
[[ -f "${COBRA_EXE}" ]]       || { echo "FATAL: cobra.exe not found at ${COBRA_EXE} — build it first (docker/build-windows.sh)" >&2; exit 1; }
[[ -d "${FRONTEND_SRC}" ]]    || { echo "FATAL: frontend source not found at ${FRONTEND_SRC}" >&2; exit 1; }
[[ -s "${LICENSE_SRC}" ]]     || { echo "FATAL: LICENSE.txt missing at repo root — a bundle without the EULA is not distributable" >&2; exit 1; }
SHARED_PARTS=("${LICENSES_DIR}"/[0-9][0-9]-*.txt)
WIN_PARTS=("${LICENSES_WIN_DIR}"/[0-9][0-9]-*.txt)
{ [[ ${#SHARED_PARTS[@]} -ge 3 && -f "${SHARED_PARTS[0]}" ]]; } \
  || { echo "FATAL: shared third-party notices manifest missing/incomplete at ${LICENSES_DIR}" >&2; exit 1; }
{ [[ ${#WIN_PARTS[@]} -ge 3 && -f "${WIN_PARTS[0]}" ]]; } \
  || { echo "FATAL: Windows-only notices manifest missing/incomplete at ${LICENSES_WIN_DIR} (need >=3 NN-*.txt: OpenBLAS/GCC-runtime/winpthreads)" >&2; exit 1; }
[[ -s "${SAMPLE_SRC}" ]] || { echo "FATAL: sample dataset not found at ${SAMPLE_SRC}" >&2; exit 1; }

rm -rf "${OUTPUT_DIR}"
RES_DIR="${OUTPUT_DIR}/resources"
mkdir -p "${RES_DIR}/frontend/css" "${RES_DIR}/frontend/js" "${RES_DIR}/sample"

cp "${PIET_NATIVE_EXE}" "${OUTPUT_DIR}/PIET.exe"
cp "${COBRA_EXE}" "${RES_DIR}/cobra.exe"

# ── Windows resource-layer assert (piet/native/resources/windows/) ────────
# The application manifest is embedded in the PE's resource section VERBATIM,
# as the UTF-8 XML bytes, so a plain byte grep distinguishes a
# resource-carrying build from one made before that layer landed (or from a
# non-WIN32 CMake branch). Two reasons this is FATAL rather than a warning:
#   1. activeCodePage=UTF-8 in that manifest is THE gate for Korean/CP949
#      usernames and paths — an exe without it mojibakes or fails to open
#      files under a non-ASCII profile directory, on the machines this port
#      exists to serve.
#   2. piet_setup.iss reads its AppVersion out of THIS file's ProductVersion
#      string resource; without the resource layer that fails much later
#      with a far less obvious message.
#
# The manifest is stored in .rsrc as the verbatim UTF-8 XML bytes, so a plain
# byte grep for the ASCII token is sound.
grep -aq 'activeCodePage' "${OUTPUT_DIR}/PIET.exe" \
  || { echo "FATAL: staged PIET.exe carries no embedded application manifest (no activeCodePage/UTF-8) — it was built without piet/native/resources/windows/. Rebuild with docker/Dockerfile.piet-windows against a piet/native/CMakeLists.txt that configures piet.rc, and check that build's configure output for the 'building piet_native.exe WITHOUT an application icon' warning too." >&2; exit 1; }

# Separate assert for reason 2 above: the manifest and the VERSIONINFO block
# are distinct resources, so the activeCodePage grep does NOT actually prove
# the version resource is there — and it is the version resource that
# piet_setup.iss's AppVersion read (GetStringFileInfo ... "ProductVersion")
# depends on. VERSIONINFO string KEYS are stored UTF-16LE, not UTF-8, so the
# ASCII token never appears literally; '.' matches the interleaved NUL bytes.
# LC_ALL=C keeps '.' byte-oriented so arbitrary binary can't upset the match.
LC_ALL=C grep -aq 'P.r.o.d.u.c.t.V.e.r.s.i.o.n' "${OUTPUT_DIR}/PIET.exe" \
  || { echo "FATAL: staged PIET.exe carries no VERSIONINFO ProductVersion string — piet_setup.iss derives its AppVersion from it and will fail the compile with '#error ... carries no ProductVersion resource'. Rebuild with docker/Dockerfile.piet-windows so piet.rc.in is configured from PIET_VERSION_VALUE." >&2; exit 1; }

cp "${FRONTEND_SRC}/index.html" "${RES_DIR}/frontend/index.html"
rsync -a --exclude '__pycache__' --exclude '.DS_Store' "${FRONTEND_SRC}/css/" "${RES_DIR}/frontend/css/"
rsync -a --exclude '__pycache__' --exclude '.DS_Store' "${FRONTEND_SRC}/js/"  "${RES_DIR}/frontend/js/"

cp "${SAMPLE_SRC}" "${RES_DIR}/sample/fatalities.csv"
cp "${LICENSE_SRC}" "${OUTPUT_DIR}/LICENSE.txt"

# Demo kit (User 2026-07-09): the teaching kit ships user-visible in the
# install dir. The tour loads data via sysuse (bundle-first), so it runs
# from ANY directory — the csv here is the Stata-side companion copy.
mkdir -p "${OUTPUT_DIR}/demo"
cp "${DEMO_KIT_SRC}/fatalities_tour.do" "${OUTPUT_DIR}/demo/"
cp "${DEMO_KIT_SRC}/HANDBOOK.md"        "${OUTPUT_DIR}/demo/"
cp "${DEMO_KIT_SRC}/fatalities.csv"     "${OUTPUT_DIR}/demo/"
# The pangram + its UI/UX manual. pangram_tour.do deliberately opens the .csv
# with `use` — the one line that is an error in Stata — and carries the
# `import delimited` fallback commented directly beneath it, so the kit
# demonstrates the content-sniffed open rather than only describing it. It
# therefore reads fatalities.csv from the WORKING DIRECTORY (unlike
# fatalities_tour.do, which goes through bundle-resolved `sysuse`), which is
# why the manual tells the reader to copy this folder somewhere writable.
cp "${DEMO_KIT_SRC}/pangram_tour.do"    "${OUTPUT_DIR}/demo/"
# Inno Setup runs under Wine in the cross-build lane. Its file enumerator can
# mojibake a UTF-8 source filename and abort with "Invalid name", so retain the
# Korean guide bytes under an ASCII packaging name. The public source tree does
# not carry this guide, so a clone staging without it is not an error.
if [[ -f "${DEMO_KIT_SRC}/사용법.md" ]]; then
  cp "${DEMO_KIT_SRC}/사용법.md"         "${OUTPUT_DIR}/demo/PANGRAM_GUIDE_KO.md"
fi

# ── Third-party notices — shared parts + Windows-only activated parts ─────
# (see piet/packaging/licenses/windows/README.md for why these two lists are
# concatenated separately from package_native_app.sh's mac-only pass)
NOTICES_OUT="${RES_DIR}/THIRD_PARTY_NOTICES.txt"
{
  echo "PIET — THIRD-PARTY NOTICES (Windows build)"
  echo "(generated at package time from piet/packaging/licenses/{,windows/} — do not edit in place)"
  for part in "${SHARED_PARTS[@]}" "${WIN_PARTS[@]}"; do
    echo
    echo "────────────────────────────────────────────────────────────────────"
    echo
    cat "${part}"
  done
} > "${NOTICES_OUT}"
# Root copy for the .iss Start-menu shortcut ({app}\THIRD_PARTY_NOTICES.txt)
# and Program Files browsing; same generated bytes, same run.
cp "${NOTICES_OUT}" "${OUTPUT_DIR}/THIRD_PARTY_NOTICES.txt"

[[ -s "${NOTICES_OUT}" ]] \
  || { echo "FATAL: THIRD_PARTY_NOTICES.txt missing/empty in the assembled bundle at ${NOTICES_OUT}" >&2; exit 1; }
grep -q "StataCorp LLC" "${NOTICES_OUT}" \
  || { echo "FATAL: StataCorp non-affiliation disclaimer absent from ${NOTICES_OUT}" >&2; exit 1; }
grep -q "OpenBLAS" "${NOTICES_OUT}" && grep -q "GCC RUNTIME LIBRARY EXCEPTION" "${NOTICES_OUT}" && grep -q "winpthreads" "${NOTICES_OUT}" && grep -q "GNU libiconv" "${NOTICES_OUT}" \
  || { echo "FATAL: expected Windows-only component notices (OpenBLAS/GCC-runtime/winpthreads/libiconv) absent from ${NOTICES_OUT}" >&2; exit 1; }

# ── Runtime-layout assert — the bundle_paths.cpp WP-W5 contract ───────────
# Everything piet_native resolves through bundleResourcesDir() must exist
# under resources\ or the app silently degrades to dev-tree fallback paths
# (the exact failure the 2026-07-08 canary caught). Fail the stage, not the
# user.
for must in "resources/frontend/index.html" "resources/cobra.exe" \
            "resources/THIRD_PARTY_NOTICES.txt" "resources/sample/fatalities.csv" \
            "demo/fatalities_tour.do" "demo/HANDBOOK.md" "demo/fatalities.csv" \
            "LICENSE.txt" "THIRD_PARTY_NOTICES.txt" "PIET.exe"; do
  [[ -e "${OUTPUT_DIR}/${must}" ]] \
    || { echo "FATAL: staged bundle violates the runtime layout contract — missing ${must} (see bundle_paths.cpp bundleResourcesDir, Windows branch)" >&2; exit 1; }
done

echo "[stage] third-party notices: $(wc -c < "${NOTICES_OUT}" | tr -d ' ') bytes from $(( ${#SHARED_PARTS[@]} + ${#WIN_PARTS[@]} )) manifest parts — asserts OK"
echo "[stage] built: ${OUTPUT_DIR}"
du -sh "${OUTPUT_DIR}"
echo "[stage] next: iscc /DDistDir=\"${OUTPUT_DIR}\" piet/packaging/piet_setup.iss  (on Windows or under Wine — Inno Setup itself does not build on macOS/Linux)"
