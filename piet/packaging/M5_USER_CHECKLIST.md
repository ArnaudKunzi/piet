# PIET 2.0.0 GA — User visual-smoke checklist

**Purpose:** manual launched-app verification of the packaged `PIET.app` (no-Python
native core). Per the standing rule, PIET GUI testing is never automated (no
computer-use driving the screen) — this is a checklist for the User to run by hand.
Everything automatable (headless gates: `--selftest`, M1 diff, full corpus,
`debug_all.do` replay) has already been run against the packaged binary itself and
is reported separately (ROADMAP §2.0.0 M5 row).

**Build under test:** `PIET.app` produced by `piet/packaging/package_native_app.sh`
(default output `/tmp/piet-native-app-build/PIET.app`; pass a different `OUTPUT_DIR`
arg to build elsewhere). 4-tuple: `{piet 2.0.0, cobra 0.18.2, contract_A 1.8.0,
contract_B 2.0.0}` — visible in `Contents/Info.plist`'s `PIETReleaseTuple` and (once
the app is open) wherever the frontend surfaces `GET`-equivalent version info.

Since this is an ad-hoc-signed build (no Developer ID cert), the FIRST launch will
likely need a right-click → Open (or a one-time Gatekeeper "are you sure" dialog)
rather than a plain double-click — that's expected, not a bug.

## Checklist

- [ ] **Launch.** Right-click `PIET.app` → Open (first launch) or double-click
      (subsequent launches). A window titled "PIET 2.0" opens. No crash, no
      bounce-and-quit in the Dock.
- [ ] **App icon.** The Dock/Finder icon is the CoBrA mark (not a generic
      placeholder) — confirms `Resources/CoBrA.icns` + `Info.plist`'s
      `CFBundleIconFile` wired correctly.
- [ ] **Cold-start feel.** Window appears quickly (native binary, no interpreter
      boot) — compare subjectively against the old 1.16.0 `.app` if it's still
      installed; the native build should feel noticeably snappier to first paint.
- [ ] **Load a dataset.** Use the toolbar's Open (or the console: `sysuse auto`
      or `use` a `.dta`/`.hbin` you have) — the data grid populates, variable list
      shows in the sidebar.
- [ ] **Run a do-file.** `do`/`run` a `.do` file from the console (or paste a few
      lines and hit Run) — it executes, output blocks render in the console pane,
      and (per B2, carried over from 1.16.0) the do-file opens in the editor tab.
- [ ] **Run a regression** (e.g. `regress price mpg weight` or any estimator you
      normally use) — coefficient table renders, numbers look sane. If you have
      the 1.16.0 `.app` side by side, spot-check a few coefficients match.
- [ ] **Progress strip + Cancel.** Kick off a longer-running do-file (a loop over
      many iterations, or a big script) — the progress strip shows
      current line/command and updates live (not just on a slow poll); clicking
      Cancel stops the run cleanly (no hang, no crash) and reports a
      `cancelled` status.
- [ ] **Graphs render (SVG).** Run any `graph`/`twoway`/`histogram`/`scatter`/
      `xtline` command reachable without the two known gaps below — the SVG
      renders inline. `graph export foo.svg` produces a real file you can open.
      **Known gap:** `graph export foo.png` will show an explicit error
      ("native core does not bundle matplotlib... use .svg instead") — this is
      expected, not a bug (documented `piet_gap`, SVG is the supported path).
- [ ] **Editor tab.** Open/edit a `.do` file in the in-app editor, make a small
      edit, save it, re-run it — the edit takes effect.
- [ ] **Data editor.** Double-click a cell in the data grid, edit a value, confirm
      it sticks (single-cell mutate, per 1.x parity).
- [ ] **Console history / scrollback.** Scroll back through prior console output
      after running several commands — text stays legible, nothing corrupts.
- [ ] **Clean close.** Close the window (red traffic-light or Cmd-Q). The process
      exits — check Activity Monitor / `ps aux | grep piet_native` shows nothing
      lingering a few seconds after close (proves the owned-thread-join shutdown
      path works, not just that the window disappeared).
- [ ] **Relaunch.** Quit and relaunch once more — same clean behavior, no stale
      lock files or corrupted state from the first session.

## If something looks wrong

Note exactly which checklist item failed, what you did, and what you saw (a
screenshot helps) — hand it to the PIET Conductor for triage same as any other
`f-killer` report. Since every headless gate (selftest/M1/corpus/debug_all) is
already GREEN against this exact binary, a checklist failure here most likely
points at something GUI-specific (webview rendering, window chrome, file-dialog
behavior) that the headless gates structurally can't see — genuinely useful signal,
not redundant with what's already been verified.
