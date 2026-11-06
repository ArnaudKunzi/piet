> **⚠ ARCHIVED (2026-07-07, governance overhaul).** Pre-1.0 design document for the retired
> Python-kernel architecture — superseded by the 2.0 native core ([ROADMAP.md](ROADMAP.md) ·
> [SPEC.md](SPEC.md) · `piet/native/`). Kept for history — do NOT plan or implement from this file.

# PIET — Master Plan

**CoBrA engine + Stata-like desktop GUI = PIET.** CoBrA has ~45
subcommands, all JSON-out, `.hbin` columnar data — everything except a UI. PIET is the thin shell
that makes it behave like Stata.

**Target:** macOS MVP by **Wed 2026-06-24**. No Stata license required.

Sub-plans: [BACKEND.md](BACKEND.md) (Python kernel, router, API) · [FRONTEND.md](FRONTEND.md) (UI design, JS engineering)

---

## Architecture — 3 layers, 2 boundaries

```
cobra C++ binary          ← complete engine, ~45 subcommands
       ↕  A: --tidy coeftable[] (frozen contract)
Python kernel  (HTTP)     ← BACKEND.md
       ↕  B: /api/*  ResultBlock union
HTML/JS frontend  (SPA)   ← FRONTEND.md
```

**Boundary A** (cobra ↔ kernel): subcommand names, flags, JSON schema, exit codes.
Anchor = `--tidy` → single `coeftable:[{name,b,se,t,p,ci_lo,ci_hi}]`. Frozen.

**Boundary B** (kernel ↔ frontend): `/api/*` REST + `ResultBlock` union.
Swappable — rewrite frontend in React/Tauri later without touching the kernel.

**Patch safety:** shipped `.app/.exe` bundles a pinned cobra triple (engine+kernel+frontend tested
together). During development: version handshake (`cobra version --json`), additive-only semver on
both boundaries, boundary contract-tests, defensive JSON parsing. Details → BACKEND.md §patch /
FRONTEND.md §6.2.

---

## Locked decisions

1. **Stack = web UI, frontend/backend fully separated.** Backend = Python "PIET kernel" (local HTTP).
   Frontend = static HTML/JS SPA talking only to `127.0.0.1`. Frontend is an independently swappable
   artifact.
2. **Offline standalone desktop, macOS + Windows only.** No Linux/cloud (no UI needed there).
   MVP = pywebview native window; PyInstaller packaging is post-Wed.
3. **Hardware baseline:** Mac = Apple Silicon M1 / 8 GB. Windows = ~13-yr-old GTX 660 — **no AVX2**
   → Windows cobra must use `-march=x86-64` (v3 = SIGILL on that CPU). Mac MVP is unblocked.
4. **Wed cut = "core + linear .do".** No loops / macros / programs / RNG / graphics. Unsupported =
   **explicit named rejection**, never silently wrong.
5. **Kernel language = Python now → C++ in Tier A.** Kernel is glue (not compute) — Python gives dev
   speed + free `.dta` parsing (pandas) + fast error-report→command-add loop. When the subset
   stabilizes, the parser absorbs into the engine (lean single binary, Tier-A endgame). Frontend is
   unchanged by that migration (same `/api/*` boundary).

---

## 7-day sequence (Mac MVP — target Wed 2026-06-24)

| Day | Who | Milestone |
|---|---|---|
| **D1** | backend | `piet/` scaffold + venv, `hbin.py` / `cobra_runner.py` / `config.py`. Script: `deryugina.hbin` → `cobra feols --tidy` → `coeftable` parsed. **Proof of life.** |
| **D2** | backend + FE | Router + formatter: tokenizer / commands / translate / tables, `POST /api/command`. curl: regress/feols/logit/summarize. **FE: `formatEstimation()` golden-first** (≥4 real `--tidy` JSON outputs). |
| **D3** | frontend | UI shell: `index.html` + app / console / panels / api.js. Command input → console block. Browser tab. *Clickable 3-pane.* |
| **D4** | frontend | Data browser + tabulate: `grid.js` virtualization, `GET /api/browse`, `tab a b, chi2`, `describe`. |
| **D5** | both | Mutate + .do + reject: `mutate.py` (eval → hbin swap), `doexec.py`, `errors.py`, `lastModel`. *Linear .do end-to-end.* |
| **D6** | both | dta load + hardening: toolbar Open .dta, estimator long-tail (iv / xtreg-fe / nbreg / tobit / heckman / qreg / ts), number-format polish. |
| **D7** | frontend | pywebview native window (`entry.py`) + buffer. *Wed demo.* |

Post-Wed: PyInstaller `.app` / `.exe`, Windows cobra baseline build (x86-64 / non-AVX2), `/tmp`-keepif workaround.

---

## Build status (live)

**Consolidated onto `main` 2026-06-18** — the parallel-build worktrees (`cecon-planner`, branch
`piet-frontend`) are retired; everything now lives in the single repo on `main`,
linear history. Only side worktree left = `cecon-patch` (engine hardening, branch `auto-patch/2026-06-17-1343`,
still separate).

- **Backend D1–D2 ✓** (`b9ba3ef`) — 28 kernel modules, all 9 `/api/*` endpoints, `contract_test.py` green on all
  10 fixtures. Three planner flags dispositioned (see `CONTRACT.md` *Engine facts* + `ROADMAP.md` *PIET 요청*):
  `regress`→`feols --vcov iid` routing (ols `--tidy` se/t null), version handshake best-effort, pyproject
  `[build-system]`.
- **Frontend D3 ✓** (`6ad8f09`) — vanilla SPA shell (`index.html`/`css`/`js`), Mondrian/Simple modes,
  `formatEstimation()` golden-first. D4+ (data browser, .do replay, dta) still ahead.
- **Engine (landed with PIET work)** — factor-var `i./c./#/##` interactions (`2a73ff5`) + collinear-column
  omission for empty-cell DID designs (`7c284f4`, closes 2a73ff5's documented limitation); both green vs Stata
  goldens (`fvint`, `fvcoll` ctests, ~1e-9).
- **Planner** — now works directly in the repo on `main`; owns `CONTRACT.md` + `piet/fixtures/`,
  runs the contract-test, integrates at D5 / D7.
- **Pending:** live `--live` contract-test (needs a running kernel) · fold `auto-patch` into `main` when that
  engine line is declared done.

---

## Cross-cutting risks

1. **Coeftable formatter = critical path.** All three design judges flagged this. Build `formatEstimation()`
   against goldens on D2 *before* any layout polish. Degrade: flatten multi-equation to first equation
   rather than ship misaligned nested tables.
2. **dta = biggest pre-Wed risk.** `cobra convert` is CSV-only; MVP uses `to_hbin.py` in-process
   (validated path). Post-Wed: native `cobra convert x.dta` (readstat-based) → drop pandas from bundle.
3. **`xtreg ,fe` must route to `feols --fe`.** `xtreg` is RE-only (`xtreg_re`); silent misroute = wrong
   numbers. Confirmed by engine inspection.
4. **Never silently wrong.** Unsupported constructs (loops, macros, `c.`/`##`, unknown commands) return a
   named `error` block and **halt `.do` replay** at that line. The lexer must detect macro references
   *before* command parsing — an undetected macro expands to empty string and silently changes the command.

**Cut order when behind schedule:**
① WS streaming (sync POST only)
② dta load (demo with CSV / `tests/validate/*.hbin`)
③ twoway tab + chi2 (oneway only)
④ estimator long-tail (heckman / reg3 / qreg / ts)
⑤ native window (browser tab is fine for demo)

---

## v0 NOT building (hard cut list)

Graphics · loops / macros / `program define` · RNG / simulate / bootstrap · `c.` / `ib#.` / `##`
interactions (engine gap — workaround: `gen xz = x*z`) · `matrix` / `scalar` · grid cell copy /
find / filter · dark-mode toggle · WS streaming · PyInstaller · Windows baseline build · resizable panes.

All rejections surface as a named `error` block. Never silently wrong.

---

## Post-Wed roadmap (brief)

- **Packaging:** `entry.py` → uvicorn daemon thread + `webview.create_window`; PyInstaller onedir
  bundles python + backend + `frontend/` + `bin/<os>/cobra` (+ OpenBLAS DLL on Windows).
- **Windows cobra build:** `-march=x86-64` baseline (SSE2 / v2); OpenBLAS non-AVX2 kernel
  (`TARGET=NEHALEM` or `DYNAMIC_ARCH=1`).
- **Windows `/tmp` workaround:** `--keepif` hardcodes `/tmp` → PIET pre-runs
  `cobra eval --keepif --out <workdir>` to materialise the filtered hbin before passing it.
- **Tier A:** parser migrates into cobra C++ → lean single binary; `/api/*` boundary unchanged.
- **Engine gap #1 (DID / interaction):** `c.x##c.z`, `i.treat##i.post` — the most painful missing
  feature. Workaround: `gen xz = x*z` manually. Highest-priority engine roadmap item post-Wed.
