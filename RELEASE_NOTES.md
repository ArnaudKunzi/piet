# PIET A.0.2 / CoBrA 1.0.2 — first Windows build; do-files open in their own folder

The app reports PIET `A.0.2`, the engine CoBrA `1.0.2`; contracts unchanged at
1.26.0 / 2.10.0. Corresponding source: this repository at tag `A.0.2`.

**New on every platform: a do-file's folder becomes the working directory when you
open it.** Choosing a do-file with Run .do or Open do-file, dropping one on the
window, or reopening one from Recent now moves the working directory to that file's
folder, and the Results pane says so. A relative `use "mydata.dta"` in the do-file
finds the data beside it, where before it looked in Documents. A `cd` you type
afterwards stands, because the move happens only when a file is opened. A `do FILE`
you type or call from a script leaves the working directory alone, as in Stata.

**New: Windows 10/11 x64.** `PIET-A.0.2-setup.exe` installs the app and, if the machine
needs it, Microsoft's WebView2 Runtime. `PIET-A.0.2-windows-x64-portable.zip` runs
from any folder on a machine that already has WebView2, as Windows 11 does. Neither
is code-signed, so SmartScreen warns on first launch: see the README's "Download
for Windows".

Known differences from the macOS build, both named rather than hidden:

- `ustrregexra()` and `ustrregexm()` refuse a character above U+FFFF, such as an emoji,
  with r(199). Windows matches text in 16-bit units, so `.` would otherwise match half
  of such a character. Hangul and all other text through U+FFFF behave as on macOS.
- `sdid, vce(bootstrap)` standard errors can differ from the macOS build's in about the
  fourth significant digit for the same seed; the point estimate does not. A few
  resampled panels are near-degenerate, which makes those replicates sensitive to
  whether the CPU fuses `a*b + c` into one rounding step. The macOS build does and the
  Windows build, which targets CPUs without that instruction, does not.

Also fixed: a built tree on Windows now finds its own engine, and the engine's
x86-64 baseline is now applied on native Windows builds.

**Checks on this tree:** engine unit tests and `tests/public_smoke.py` (now including
Hangul folder and file names) on both macOS and Windows in GitHub Actions, where the
same 45 of 112 engine tests are skipped on both for lack of private fixtures; the
app's real-window `--ui-smoke` journey on both, now covering the do-file folder
behaviour through the drop path.

**Build provenance:** `.github/workflows/release.yml` built every download from the
commit at this tag: `scripts/build-macos.sh` with `scripts/package-macos-dmg.sh`, and
`scripts/build-windows.sh` with `scripts/package-windows.sh`. Verify downloads with the
attached `.sha256` files.

# PIET A.0.1 / CoBrA 1.0.1 — patch stamp of 2026-11-06 (tag `A.0.1`)

Patch cut of the official line: the same tree as the 2026-11-06 macOS build
below, now carrying the version strings that identify it (the app reports
PIET `A.0.1`, the engine CoBrA `1.0.1`; contracts unchanged at 1.26.0 / 2.10.0).
What changed since A.0.0 / 1.0.0: BUG-1939 (dev-tree paths resolve from the
executable; `sysuse` and the frontend were lost under a `-ffile-prefix-map`
build), BUG-1940 (`hausman` when V_b-V_B is not positive definite answered
chi2 ~ 1e16 or refused; now Stata's generalized-inverse rule with its note),
adoption declarations and SPDX headers across the tree, the demo tour
corrections, and the public packaging scripts. The `.dmg` on the release page
is rebuilt from this tag. Corresponding source: this repository at tag `A.0.1`.

# PIET A.0.0 / CoBrA 1.0.0 — macOS build of 2026-11-06 (tag `A.0.0-public-20261106`)

First downloadable build from the public tree: `PIET-A.0.0-20261106-macos-arm64.dmg`
on the release page, built with `scripts/build-macos.sh` + `scripts/package-macos-dmg.sh`
from the tagged commit on an Apple Silicon Mac (macOS 26.6, Xcode command-line
tools), ad-hoc signed, not notarized (see README, "Download for macOS").
The app reports PIET `A.0.0` and CoBrA `1.0.0`; the tree it was built from
includes the fixes landed after those version stamps (BUG-1939 dev-tree path
resolution, BUG-1940 `hausman` on a difference matrix that is not positive
definite, demo tour corrections), so the tag and commit on the release page,
not the version string alone, identify the build. Corresponding source: this
repository at the tag.

# PIET A.0.0 / CoBrA 1.0.0 — public source cut of 2026-11-06

This cut replaces the experimental 2026-10 source beta with the full current
source tree of PIET A.0.0 and CoBrA 1.0.0 (the compiled version strings), as
prepared for public distribution on 2026-11-06. The whole tree is replaced in
one commit on top of the beta's own public history; no private development
history, private oracle logs, licensed reference datasets, or internal
automation is included.

- Every statistical routine ships as the project's own implementation. Code
  adapted from GPL-compatible open-source projects is declared in
  `THIRD_PARTY_NOTICES.txt` and in a header comment of the file that carries it.
- Tests and self-test arms that depend on private oracle fixtures report
  themselves as declared skips in a public clone; the fixtures they name are
  not required to build or use the software.
- Corresponding source for any binary published on the release page is this
  repository at the release tag.

# PIET 1.0 Beta 1 — v1.0.0-beta.1 (historical)

The first public source snapshot (2026-10-12) with an Apple Silicon DMG. It
used a different version naming scheme and withheld several functions pending
provenance review; those functions are present in the current tree as the
project's own implementations.
