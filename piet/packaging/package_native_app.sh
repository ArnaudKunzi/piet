#!/usr/bin/env bash
# M5 GA packaging script — assembles PIET.app from the built piet_native
# binary. Owner: PIET Conductor (Constitution §11.C builder role).
#
# Unlike piet.spec (PyInstaller, SUPERSEDED but kept for the 1.16.x PATCH
# line per the Master Plan — the Python backend isn't deleted until the
# FIRST 2.0.x after GA GREEN), this script does NOT freeze a Python
# interpreter: it copies the already-compiled piet_native executable, the
# real piet/frontend/ SPA tree, and the pinned cobra binary into a standard
# macOS .app layout, generates Info.plist from the template, and ad-hoc
# codesigns the result. There is no PyInstaller step, no venv, no
# site-packages — that is the entire point of M5 (Doctrine ③: no Python at
# runtime).
#
# Usage (from the repo root, after building piet_native):
#   cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build piet/native/build -j
#   piet/packaging/package_native_app.sh [OUTPUT_DIR]
#
# UI journey gate (2.0.2 / MI-138 WP-1 — run it on the artifact this script
# just built, REQUIRED for any release touching UI/packaging/transport,
# Constitution Part 1 "renders != works"):
#   OUTPUT_DIR/PIET.app/Contents/MacOS/piet_native --ui-smoke
# Opens the real packaged WKWebView, drives boot -> bridge -> kernel-dot
# GREEN -> open dataset -> varlist -> regress -> rendered results block;
# exits 0 (GREEN) / 1 (RED) and writes /tmp/piet_ui_smoke_verdict.json
# (override: $PIET_UI_SMOKE_OUT; fixture dataset: $PIET_UI_SMOKE_DATA,
# default = the bundle's own Resources/sample/fatalities.csv — the gate is
# self-contained on the packaged artifact). See main.cpp runUiSmoke().
#
# OUTPUT_DIR defaults to a LOCAL (not external-drive) scratch dir —
# /tmp/piet-native-app-build — since codesign can be finicky on some
# external volumes (per the ROADMAP M5 briefing; the old PyInstaller
# external-drive bug itself doesn't apply here, there's no PyInstaller, but
# we build local anyway to sidestep any volume-permission surprises).
#
# Produces: OUTPUT_DIR/PIET.app
#
# Layout:
#   PIET.app/Contents/
#     Info.plist
#     MacOS/piet_native
#     Resources/
#       frontend/          <- piet/frontend/ (index.html, css/, js/) — VERBATIM,
#                             no build step, same tree main.cpp loads via file://
#       cobra              <- the pinned Art 7 cobra binary (engine/current;
#                             version read from the binary itself, never typed)
#       CoBrA.icns
#       sample/fatalities.csv      <- bundled sample dataset (WI-4: US traffic
#                             fatalities panel, public-domain US gov statistics;
#                             provenance in THIRD_PARTY_NOTICES.txt)
#       THIRD_PARTY_NOTICES.txt    <- generated from piet/packaging/licenses/
#                             (WI-5; presence preflight-asserted below)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# HERE = <worktree>/piet/packaging -> walk up to <worktree>
WORKTREE="$(cd "${HERE}/../.." && pwd)"

# BUG-209: the workspace-root variable used to be a FIXED "${WORKTREE}/.."
# hop, which assumed the checkout sits exactly one level under the workspace
# directory. The worktree pool is one level deeper, so that fixed hop
# silently resolved to a directory that does not exist and packaging could
# not find the pinned engine there (a stint had to route around it with
# $COBRA_BIN_PIN). Fixed the same way as the C++ site
# (piet/native/include/piet/coord_root.h): search upward for a directory
# that actually LOOKS like the private coordination tree (has releases/ and
# claims/ subdirectories) instead of counting hops. Same semantics, no
# shared code with the C++ helper (bash has no header to #include) — see
# find_coord_root() below.
find_coord_root() {
  # A directory qualifies as the private coordination tree only by STRUCTURE:
  # it carries a regular file releases/CURRENT and a claims/ directory. No
  # name is assumed; every child of every ancestor is probed and more than
  # one qualifying directory is an error, never a guess.
  local dir="$1" max_depth=10 depth=0 parent d searched=() found=""
  dir="$(cd "${dir}" && pwd)"
  while [[ "${depth}" -le "${max_depth}" ]]; do
    [[ "${dir}" == "/" ]] && break  # already at the filesystem root, nothing further up
    parent="$(cd "${dir}/.." && pwd)"
    [[ "${parent}" == "${dir}" ]] && break  # reached filesystem root
    for d in "${parent%/}"/*/; do
      d="${d%/}"
      [[ -d "${d}" ]] || continue          # unmatched glob stays literal
      [[ "${d}" == "${dir}" ]] && continue  # the checkout / ancestor we came from
      searched+=("${d}")
      if [[ -f "${d}/releases/CURRENT" && -d "${d}/claims" ]]; then
        if [[ -n "${found}" && "${found}" != "${d}" ]]; then
          echo "FATAL: ambiguous coordination trees (releases/CURRENT + claims/) found while walking upward from ${1} — refusing to guess: ${found} AND ${d}" >&2
          exit 1
        fi
        found="${d}"
      fi
    done
    dir="${parent}"
    depth=$((depth + 1))
  done
  if [[ -z "${found}" ]]; then
    echo "FATAL: no coordination tree (releases/CURRENT + claims/) found within ${max_depth} level(s) above ${1} — probed ${#searched[@]} directories" >&2
    exit 1
  fi
  echo "${found}"
}
COORD_ROOT="$(find_coord_root "${WORKTREE}")"

NATIVE_BUILD="${WORKTREE}/piet/native/build/piet_native"
FRONTEND_SRC="${WORKTREE}/piet/frontend"
ICON_SRC="${WORKTREE}/branding/CoBrA.icns"
# Art 7 pin — same reference cobra_runner.cpp's kPinnedReference and
# piet.spec's COBRA_PIN both point at. Override with $COBRA_BIN_PIN for a
# pre-merge gate build against a different pinned reference (mirrors
# piet.spec's $PIET_SRC override pattern).
COBRA_PIN="${COBRA_BIN_PIN:-${COORD_ROOT}/releases/engine/current/cobra}"

OUTPUT_DIR="${1:-/tmp/piet-native-app-build}"
APP="${OUTPUT_DIR}/PIET.app"
CONTENTS="${APP}/Contents"

# WI-5 (legal remediation 2026-07-08): the third-party notices manifest.
# THIRD_PARTY_NOTICES.txt is GENERATED at package time from the checked-in
# licenses/ dir (single source — see licenses/README.md) and must travel
# with every distributed bundle (MIT notice obligation).
LICENSES_DIR="${HERE}/licenses"

# ── Preflight ────────────────────────────────────────────────────────────
[[ -x "${NATIVE_BUILD}" ]] || { echo "FATAL: piet_native not built at ${NATIVE_BUILD} — build it first" >&2; exit 1; }
[[ -d "${FRONTEND_SRC}" ]] || { echo "FATAL: frontend source not found at ${FRONTEND_SRC}" >&2; exit 1; }
[[ -f "${ICON_SRC}" ]]     || { echo "FATAL: icon not found at ${ICON_SRC}" >&2; exit 1; }
[[ -x "${COBRA_PIN}" ]]    || { echo "FATAL: pinned cobra not found/executable at ${COBRA_PIN}" >&2; exit 1; }
# WI-5 preflight: the notices manifest must exist with the disclaimer + the
# two active MIT notices (webview, nlohmann/json) at minimum.
NOTICE_PARTS=("${LICENSES_DIR}"/[0-9][0-9]-*.txt)
{ [[ -d "${LICENSES_DIR}" ]] && [[ ${#NOTICE_PARTS[@]} -ge 3 && -f "${NOTICE_PARTS[0]}" ]]; } \
  || { echo "FATAL: third-party notices manifest missing/incomplete at ${LICENSES_DIR} (need >=3 NN-*.txt parts; see licenses/README.md) — a bundle without notices is not distributable" >&2; exit 1; }
# WI-4 preflight: the bundled sample dataset.
SAMPLE_SRC="${HERE}/sample/fatalities.csv"
[[ -s "${SAMPLE_SRC}" ]] || { echo "FATAL: sample dataset not found at ${SAMPLE_SRC}" >&2; exit 1; }

# ── 4-tuple ──────────────────────────────────────────────────────────────
# SINGLE VERSION SOURCE (2.0.2 / MI-138 WP-2): piet_version/contract_a/
# contract_b have exactly ONE definition site — piet/native/CMakeLists.txt's
# target_compile_definitions, compiled into the binary. This script reads
# them back OUT of the actual binary being bundled (`piet_native --version`)
# instead of keeping a hand-synced copy, so Info.plist can never drift from
# the compiled tuple (the 2.0.1 contract_a drift bug class; postmortem RC6).
# cobra_version is likewise read from the ACTUAL pinned binary (never
# hand-typed) so Info.plist can never drift from what's bundled.
# `piet_native --selftest`'s version-single-source leg re-asserts this
# equality from INSIDE the packaged bundle.
NATIVE_VERSION_JSON="$("${NATIVE_BUILD}" --version)" \
  || { echo "FATAL: ${NATIVE_BUILD} --version failed to run" >&2; exit 1; }
read -r PIET_VERSION CONTRACT_A CONTRACT_B < <(printf '%s' "${NATIVE_VERSION_JSON}" \
  | python3 -c 'import json,sys; t=json.load(sys.stdin); print(t["piet_version"], t["contract_a"], t["contract_b"])') \
  || { echo "FATAL: could not parse the version tuple JSON from ${NATIVE_BUILD} --version: ${NATIVE_VERSION_JSON}" >&2; exit 1; }
[[ -n "${PIET_VERSION}" && -n "${CONTRACT_A}" && -n "${CONTRACT_B}" ]] \
  || { echo "FATAL: could not read the compiled version tuple from ${NATIVE_BUILD} --version" >&2; exit 1; }
COBRA_VERSION_JSON="$("${COBRA_PIN}" version --json)" \
  || { echo "FATAL: ${COBRA_PIN} version --json failed to run" >&2; exit 1; }
COBRA_VERSION="$(printf '%s' "${COBRA_VERSION_JSON}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["cobra_version"])')" \
  || { echo "FATAL: could not parse cobra_version JSON from ${COBRA_PIN} version --json: ${COBRA_VERSION_JSON}" >&2; exit 1; }
[[ -n "${COBRA_VERSION}" ]] \
  || { echo "FATAL: could not read cobra_version from ${COBRA_PIN} version --json" >&2; exit 1; }

echo "[package] piet=${PIET_VERSION} cobra=${COBRA_VERSION} contract_A=${CONTRACT_A} contract_B=${CONTRACT_B}"

# ── Assemble ─────────────────────────────────────────────────────────────
rm -rf "${APP}"
mkdir -p "${CONTENTS}/MacOS" "${CONTENTS}/Resources/frontend"

cp "${NATIVE_BUILD}" "${CONTENTS}/MacOS/piet_native"
chmod 755 "${CONTENTS}/MacOS/piet_native"

# Strip the bundled binary (symbol table only; execution byte-unchanged) — the
# SAME size discipline the engine cobra already gets (VERSIONING checklist
# step 4, gate ③). Adopted at the B.0.0 stamp: the C→H feature body pushed the
# unstripped .app past gate ③'s size ceiling; stripping is the honest infra fix
# (never a baseline fudge). Runs BEFORE codesign so the signature covers the
# stripped binary. `-` keeps going if strip is a no-op on an already-lean build.
strip "${CONTENTS}/MacOS/piet_native" || echo "[package] strip: skipped/no-op"

# Frontend: verbatim copy, no build/minify step — this is the SAME tree
# repoRelative()/resolveFrontendIndexPath() loads in dev, just relocated.
# Only index.html/css/js are ever loaded by the webview (the file:// entry
# point is index.html, whose relative ES-module imports reach only into
# css/ and js/) — copy exactly that, nothing else. Explicitly EXCLUDE the
# test/ subtree (Node-only harness, never loaded by the webview) and
# README.md/__pycache__/.DS_Store cruft. (The 1.x Python launcher entry.py
# this used to exclude is deleted from the tree entirely — WS-D1 2026-07-08.)
mkdir -p "${CONTENTS}/Resources/frontend/css" "${CONTENTS}/Resources/frontend/js"
cp "${FRONTEND_SRC}/index.html" "${CONTENTS}/Resources/frontend/index.html"
rsync -a --exclude '__pycache__' --exclude '.DS_Store' "${FRONTEND_SRC}/css/" "${CONTENTS}/Resources/frontend/css/"
rsync -a --exclude '__pycache__' --exclude '.DS_Store' "${FRONTEND_SRC}/js/"  "${CONTENTS}/Resources/frontend/js/"

cp "${COBRA_PIN}" "${CONTENTS}/Resources/cobra"
chmod 755 "${CONTENTS}/Resources/cobra"

cp "${ICON_SRC}" "${CONTENTS}/Resources/CoBrA.icns"

# ── Sample dataset (WI-4) ────────────────────────────────────────────────
# The shipped demo panel ("Fatalities": US traffic fatalities, 48 states ×
# 1982–1988 — public-domain US government statistics; provenance note rides
# in THIRD_PARTY_NOTICES.txt via licenses/30-DATA-SOURCES.txt). CSV as
# fetched, verbatim — human-readable; the app's open flow converts CSV
# in-session via the engine. --ui-smoke defaults to THIS file, so the
# journey gate exercises the shipped sample.
mkdir -p "${CONTENTS}/Resources/sample"
cp "${SAMPLE_SRC}" "${CONTENTS}/Resources/sample/fatalities.csv"
[[ -s "${CONTENTS}/Resources/sample/fatalities.csv" ]] \
  || { echo "FATAL: sample dataset missing from the assembled bundle" >&2; exit 1; }

# ── License + notices (WI-9; GPL amendment 2026-08-06) ───────────────────
# The product is GPL-3.0-or-later (LEGAL-DISTRIBUTION §6). BOTH files travel
# in the bundle: LICENSE.txt is the canonical GPL text (removing it from a
# distributed binary is a license violation, not just a policy miss), and
# NOTICE.txt carries the non-affiliation / numerical-output / community-name
# notices that §2.7 makes a release-gate item.
LICENSE_SRC="${WORKTREE}/LICENSE.txt"
[[ -s "${LICENSE_SRC}" ]] || { echo "FATAL: LICENSE.txt missing at repo root — a bundle without its license is not distributable" >&2; exit 1; }
grep -q "GNU GENERAL PUBLIC LICENSE" "${LICENSE_SRC}" \
  || { echo "FATAL: LICENSE.txt does not contain the GNU GPL text (LEGAL-DISTRIBUTION §6.1)" >&2; exit 1; }
cp "${LICENSE_SRC}" "${CONTENTS}/Resources/LICENSE.txt"
[[ -s "${CONTENTS}/Resources/LICENSE.txt" ]] \
  || { echo "FATAL: LICENSE.txt missing from the assembled bundle" >&2; exit 1; }

NOTICE_SRC="${WORKTREE}/NOTICE.txt"
[[ -s "${NOTICE_SRC}" ]] || { echo "FATAL: NOTICE.txt missing at repo root (LEGAL-DISTRIBUTION §2.7(b)/§6.1)" >&2; exit 1; }
cp "${NOTICE_SRC}" "${CONTENTS}/Resources/NOTICE.txt"
[[ -s "${CONTENTS}/Resources/NOTICE.txt" ]] \
  || { echo "FATAL: NOTICE.txt missing from the assembled bundle" >&2; exit 1; }

# Adopted-code register consistency (LEGAL-DISTRIBUTION §1.f-bis): every
# "Portions derived from <project>" header in our own source must name a
# project that appears in the 40-ADOPTED-CODE register, and vice versa.
# A derivation that ships undeclared is the violation this asserts against.
ADOPTED_REG="${LICENSES_DIR}/40-ADOPTED-CODE.txt"
[[ -s "${ADOPTED_REG}" ]] || { echo "FATAL: adopted-code register missing at ${ADOPTED_REG}" >&2; exit 1; }
# NOTE: grep exits 1 on zero matches, and zero adopted code is the LEGAL state
# today — guard the substitution or set -e kills the package on the honest case
# (the standing "set -e capture checkpoint" shell trap).
_hdr_projects="$(grep -rho "Portions derived from [^ ]*" "${WORKTREE}/src" "${WORKTREE}/apps" "${WORKTREE}/piet/native/src" 2>/dev/null \
  | sed 's/Portions derived from //' | sort -u || true)"
while IFS= read -r _p; do
  [[ -z "${_p}" ]] && continue
  grep -q -- "${_p}" "${ADOPTED_REG}" \
    || { echo "FATAL: source declares 'Portions derived from ${_p}' but the adopted-code register has no row for it (LEGAL-DISTRIBUTION §3.3)" >&2; exit 1; }
done <<< "${_hdr_projects}"

# ── Third-party notices (WI-5) ───────────────────────────────────────────
# Generate Resources/THIRD_PARTY_NOTICES.txt from the checked-in manifest
# (licenses/NN-*.txt in lexical order: 00 = disclaimer header, then one part
# per bundled component — staged/ placeholders are deliberately excluded
# until the static Linux/Windows builds activate them). The in-app
# "About / Legal" panel reads this exact file via the legal.notices bridge
# route; dev (non-bundled) runs concatenate the same manifest directly.
NOTICES_OUT="${CONTENTS}/Resources/THIRD_PARTY_NOTICES.txt"
{
  echo "PIET ${PIET_VERSION} — THIRD-PARTY NOTICES"
  echo "(generated at package time from piet/packaging/licenses/ — do not edit in place)"
  for part in "${NOTICE_PARTS[@]}"; do
    echo
    echo "────────────────────────────────────────────────────────────────────"
    echo
    cat "${part}"
  done
} > "${NOTICES_OUT}"

# PREFLIGHT ASSERT (WI-5): a bundle without the notices file (or without the
# StataCorp disclaimer, WI-6) must never leave this script — hard build FAIL,
# not a warning. Runs BEFORE codesign so a failing bundle is never signed.
[[ -s "${NOTICES_OUT}" ]] \
  || { echo "FATAL: THIRD_PARTY_NOTICES.txt missing/empty in the assembled bundle at ${NOTICES_OUT}" >&2; exit 1; }
grep -q "StataCorp LLC" "${NOTICES_OUT}" \
  || { echo "FATAL: StataCorp non-affiliation disclaimer absent from ${NOTICES_OUT} (WI-6)" >&2; exit 1; }
grep -q "webview" "${NOTICES_OUT}" && grep -q "Niels Lohmann" "${NOTICES_OUT}" \
  || { echo "FATAL: expected component notices (webview / nlohmann json) absent from ${NOTICES_OUT}" >&2; exit 1; }
echo "[package] third-party notices: $(wc -c < "${NOTICES_OUT}" | tr -d ' ') bytes from ${#NOTICE_PARTS[@]} manifest parts — asserts OK"

sed \
  -e "s/@PIET_BUNDLE_VERSION@/${PIET_VERSION}/g" \
  -e "s/@PIET_BUNDLE_COBRA_VERSION@/${COBRA_VERSION}/g" \
  -e "s/@PIET_BUNDLE_CONTRACT_A@/${CONTRACT_A}/g" \
  -e "s/@PIET_BUNDLE_CONTRACT_B@/${CONTRACT_B}/g" \
  "${HERE}/native_bundle/Info.plist.in" > "${CONTENTS}/Info.plist"

# ── Codesign ─────────────────────────────────────────────────────────────
# Ad-hoc signature (`-s -`): no Developer ID cert available in this
# environment. This satisfies Gatekeeper's "is it signed at all" check for
# local/internal runs on the SAME machine that built it, but a truly
# distributable build (another user's machine, first-launch Gatekeeper
# assessment) needs a real "Developer ID Application: ..." identity +
# notarization — flagged here, not blocking M5 GA per the briefing.
CODESIGN_IDENTITY="${PIET_CODESIGN_IDENTITY:--}"
if [[ "${CODESIGN_IDENTITY}" == "-" ]]; then
  echo "[package] codesigning ad-hoc (-s -) — no Developer ID identity provided"
  codesign --force --deep -s - "${APP}"
else
  echo "[package] codesigning with identity: ${CODESIGN_IDENTITY} (hardened runtime + entitlements)"
  codesign --force --deep --options runtime \
    --entitlements "${HERE}/native_bundle/piet_native.entitlements" \
    -s "${CODESIGN_IDENTITY}" "${APP}"
fi

codesign --verify --deep --strict "${APP}" \
  || { echo "FATAL: codesign --verify failed on ${APP} — the shipped artifact would not pass Gatekeeper's own check" >&2; exit 1; }
echo "[package] codesign --verify: OK"

echo "[package] built: ${APP}"
du -sh "${APP}"
echo "[package] next — UI journey gate on THIS artifact (required for UI/packaging/transport releases):"
echo "[package]   ${APP}/Contents/MacOS/piet_native --ui-smoke"
