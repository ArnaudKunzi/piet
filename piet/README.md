# PIET — Parser, Interpreter & Estimation Tool

A thin, offline, Stata-like desktop front-end over the **CoBrA** C++ econometrics engine. Goal: run a professor's
`.do` file and get the same numbers without Stata.

## Architecture — 3 layers, 2 boundaries, NO Python at runtime
```
cobra C++ binary  ──A: --tidy coeftable[] (frozen)──  piet_native (C++ core + webview)  ──B: bridge routes──  HTML/JS SPA
```
- **Boundary A** (engine ↔ core): `../apps/cecon_cli.cpp`, anchored on trailing `--tidy`. cobra stays a subprocess
  (`piet/native/src/cobra_runner.cpp`), launched via the pinned Art 7 reference binary.
- **Boundary B** (core ↔ frontend): **`CONTRACT.md` (internal) — v2.0.0, ratified.** `window.pietNative.request(path,
  body)` (native `webview::bind()`) replaced `fetch(baseUrl + path)` at the 2.0 GA transport swap; every
  request/response JSON shape is unchanged from the retired HTTP kernel.

`piet/native/` is a single process: it embeds a webview (WKWebView/WebView2/WebKitGTK), serves the frontend from
`piet/frontend/` over `file://`, and drives cobra as a subprocess — no server, no venv, no installed interpreter
on the user's machine (Doctrine ③).

## Layout
```
piet/
  native/           # C++ core (CMakeLists.txt is its own standalone subproject — see there for build/test commands)
    include/piet/    src/            # ported command surface (each file's header comment cites the retired
                                     #   Python module it replaces, kept as historical porting provenance)
    tests/          # native regression scripts (internal parity gates are not in the public tree)
    vendor/         # webview.h, nlohmann/json
  frontend/         # index.html · css/piet.css · js/{api,app,console,grid,panels,util}.js — vanilla ES modules,
                    #   no framework, no build step
  fixtures/
    golden/         # real `cobra … --tidy` JSON — frontend/native dev+test reference, no kernel needed
    api/            # normalised mock bridge-route responses (kernel-shape), dom/ DOM-render fixtures
    contract_test.py
  packaging/        # package_native_app.sh (macOS .app) · stage_native_windows.sh + piet_setup.iss (Windows
                    #   installer, via Docker cross-compiled binaries) · licenses/ · sample/
```

## Dev setup
```sh
cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release
cmake --build piet/native/build -j
./piet/native/build/piet_native --selftest
./piet/native/build/piet_native            # interactive window, loads piet/frontend/ over file://
```
Engine: `piet_native` finds `cobra` via `$COBRA_BIN` > the pinned reference engine build
> bare `cobra` on PATH. Dev sample: `../tests/validate/auto.hbin` (public sysuse auto, 74 rows, 12 cols).

## Status
**2.0.x, native-only.** The original Python kernel (FastAPI/uvicorn/pywebview) that built out D1–D7 and shipped
through the 1.x line was deleted in 2.0.3 once its role as the frozen parity oracle was fully captured into
an internal frozen corpus (Eval-owned, MI-139) — the C++ core was diffed against that corpus, not against live Python.
Packaging, ports, and feature work now land directly in `piet/native/`.

## Known issues
- **System-wide text-injection tools can freeze the UI (Grammarly Desktop confirmed).** A macOS tool that
  injects into arbitrary text fields (Grammarly Desktop is the confirmed case) can mutate PIET's embedded
  WKWebView editor DOM and silently break interactivity — menus (`File▾`/`Graphics▾`), result tabs, and the
  dark/style toggles stop firing until the app is relaunched. **Workaround:** quit the injecting tool (or
  exempt PIET from it), then relaunch PIET. This is an OS-level accessibility injection, not a PIET bug; a
  webview focus/injection guard is tracked as a native-hardening follow-up.
