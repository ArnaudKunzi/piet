> **⚠ ARCHIVED (2026-07-07, governance overhaul).** Pre-1.0 design document for the retired
> Python-kernel architecture — superseded by the 2.0 native core ([ROADMAP.md](ROADMAP.md) ·
> [SPEC.md](SPEC.md) · `piet/native/`). Kept for history — do NOT plan or implement from this file.

# PIET Backend Plan — Python kernel & command router

High-level context and locked decisions live in [MASTER.md](MASTER.md).
Frontend spec (UI, CSS, JS engineering) lives in [FRONTEND.md](FRONTEND.md).
This document is the authority for the Python kernel implementer.

---

## Engine facts the plan rests on (verified against source)

- **All estimators output JSON; always pass `--tidy`** → single `coeftable:[{name,b,se,t,p,ci_lo,ci_hi}]`.
  Multi-equation (reg3/heckman/feols_multi) nests under `outcomes[]`/`equations[]`, each with its own
  `coeftable`. (`apps/cecon_cli.cpp:93–139` confirmed)
- **`i.var` = engine token `i(col)`** (K−1 dummies, min-level base). `c.`/`##` interactions: **not in
  engine** → reject at router with a helpful message ("create with `gen xz = x*z` for now").
- **`tab`/`tabulate`/`describe`/`browse`/`list`/`use` subcommands: absent** → Python handles these
  directly from hbin. `cobra info` emits plain text; skip it, read the header directly.
- **`summarize` = `cobra eval --summarize`**; gen/egen/drop/keep = `cobra eval`.
- **`--keepif "EXPR"` hardcodes `/tmp/cecon_keepif_<pid>.hbin`** → broken on Windows (no `/tmp`). PIET
  workaround: `cobra eval --keepif --out <workdir/filtered.hbin>` and pass that as `--data`. Details in
  Windows section below.
- **`xtreg` is RE-only** (Swamy-Arora, `estimator:"xtreg_re"`). **`xtreg y x, fe` must route to
  `feols --fe <panelvar>`.** Misrouting here produces wrong numbers silently.
- **`cobra convert` is CSV-only** (not dta). dta → hbin: use `tools/to_hbin.py` (pandas;
  value-label→factor, `%td`→epoch-days) in-process until native readstat support lands in the engine.
- **`.hbin` layout:** magic `CECONHB1` · u64 header_len · JSON header `{n_rows, columns:[{name, dtype:
  "f64"|"factor", offset, nbytes, levels?}]}` · 64-byte-aligned column blobs. `f64` = LE double (NaN =
  missing). `factor` = i32 codes (−1 = missing). **Only two dtypes exist. No string dtype.**
  Reader reference: `tools/hbin_read.py` (37 lines, numpy `frombuffer`). For large files use
  `np.memmap` instead of loading the entire file.
- **Dev sample data:** `tests/validate/deryugina.{dta,hbin}`. Homebrew Python lacks numpy/pandas →
  create a PIET-specific venv.

---

## Directory layout — `piet/`

```
docs/piet/
  MASTER.md  BACKEND.md  FRONTEND.md    ← plans (separate from implementation)
piet/
  pyproject.toml                         ← fastapi, uvicorn, numpy, pandas, pywebview, pyinstaller
  backend/piet/
    config.py          # cobra binary resolution ($COBRA_BIN > bundled > build/cobra), workdir
    app.py             # FastAPI app + uvicorn on 127.0.0.1:<ephemeral>, mounts static frontend
    session.py         # SessionState: current_hbin, varlist cache, history[], workdir, last_model
    cobra_runner.py    # subprocess wrapper: run(args) → dict; absolute paths, --tidy forced,
                       #   JSON parse, stderr capture, exit-code → CobraError
    hbin.py            # hbin reader (np.memmap for large files) + page window; adapts hbin_read.py
    datastore.py       # header→varlist, dtype→VarInfo, factor-code→label resolution
    router/
      tokenizer.py     # "cmd varlist [if][in][weight][, opts]" → {cmd,varlist,if_expr,opts{}}
                       #   bracket-aware comma split; ///+/* */ continuations; reject macro refs first
      commands.py      # command table: name → cobra-argv-builder | python-native handler
      translate.py     # i.var→i(col), vce()→--vcov/--cluster, if→--keepif, varlist→--y/--x
      errors.py        # UnsupportedCommand / UnsupportedOption → structured error payload
    mutate.py          # gen/egen/replace/drop/keep → cobra eval --out, hbin swap, varlist refresh
    tabulate.py        # tab/tabulate: np.unique freq (oneway), np.add.at contingency + chi2 (twoway)
    describe.py        # describe: VarInfo from hbin header
    summarize.py       # summarize: numpy min/max/mean/std/N over hbin columns
    doexec.py          # .do reader: strip comments, join ///, line-split, replay via router,
                       #   halt + emit error at first rejection, include line_number+context
    format/
      tables.py        # coeftable → Stata-style monospace text (%9.0g/%9.3f, right-aligned)
      summ_fmt.py      # summarize output layout (Obs/Mean/Std.Dev./Min/Max)
      numfmt.py        # %9.0g Stata numeric formatting helper
    api/
      routes_data.py   # GET /api/varlist · GET /api/browse · GET /api/history · POST /api/open
      routes_run.py    # POST /api/command · POST /api/do · POST /api/tabulate · POST /api/describe
      ws_console.py    # WS /ws/console  ← deferred; synchronous POST /api/do in v0
  frontend/            # static SPA; no Python imports, no file access; talks only to /api + /ws
    index.html  css/piet.css
    js/{api,app,grid,console,panels,util}.js
  packaging/
    entry.py           # uvicorn daemon thread + webview.create_window
    piet.spec          # PyInstaller spec
    build_mac.sh  build_win.ps1
    bin/{macos,windows}/cobra   ← bundled binary (version-pinned per release)
```

Frontend is a swappable artifact — the entire `frontend/` tree can be replaced without touching
the backend as long as the `/api/*` contract is honored.

---

## The command router — tokenize → classify → translate → dispatch

### Tokenizer (`router/tokenizer.py`)

**Order of operations (run before anything else):**
1. **Macro/control-flow detect** (reject whole line immediately if found):
   - Local macro refs: `` `name' `` (grave + apostrophe), global `$name` / `${name}`, inline
     `` `=exp' ``, `` `:macro fcn' ``, positional `` `0'…`9' ``
   - Macro statements: `local`, `global`, `tempvar`, `tempname`, `tempfile`, `macro`, `tokenize`,
     `gettoken`
   - Control flow: `foreach`, `forvalues`, `while`, `if` (program-level block), `else`, `continue`
   - Programs / preprocessor: `program [define]`, `#delimit` (and any `#…`)
   - **Why this must be first:** undefined macros expand to empty string — NOT an error in Stata.
     If PIET strips a macro ref and runs the remainder, it silently changes the command's meaning.
     Detect-and-reject before command classification is the only safe path.
2. Strip `*`-line comments, inline `//` (leading-blank required), `/* */` blocks.
3. Join `///` and `/* */` line continuations.
4. Split on **first unparenthesised, unquoted comma** to get `[cmd varlist [if][in][weight]]` vs
   `opts`. Handle compound double quotes `` `"…"' `` (nest: +1 on `` `" ``, −1 on `"'`) and
   `vce(…)`-style parentheses.
5. Extract `if expr` / `in range` / `[weight]` from the pre-comma portion.
6. Result: `{cmd, varlist:[], if_expr, in_range, weight, opts:{name: val|True}}`.

### Translate (`router/translate.py`)

Common rewrites applied before cobra argv construction:

| Stata input | cobra flag |
|---|---|
| varlist[0] | `--y` |
| varlist[1:] | `--x` (comma-joined) |
| `i.VAR` | `i(VAR)` |
| `if EXPR` | `--keepif "EXPR"` |
| `robust` / `vce(robust)` | `--vcov hc1` (GLM: `--vce robust`) |
| `vce(cluster V)` | `--vcov cluster --cluster V` |
| `vce(twoway A B)` | `--vcov twoway --cluster A,B` |
| `ib#.` / `c.` / `##` | **`UnsupportedOption`** — named error, helpful message |

**`last_model` state** — after each successful estimation, cache `{cmd, y, x, fe, vcov, cluster}`.
`test` / `margins` / `predict` / `lincom` read it to reconstruct `--y/--x/--fe` (minimal `e()`).

### Command table (`router/commands.py`)

Core mapping (kernel adds `--data <hbin> --tidy` automatically):

| Stata command | cobra dispatch |
|---|---|
| `regress y x` | `ols --y y --x x` |
| `regress y x, robust` | `feols --y y --x x --vcov hc1` |
| `regress y i.sex x, vce(cluster id)` | `feols --y y --x "i(sex),x" --vcov cluster --cluster id` |
| `reghdfe y x, absorb(f1 f2) vce(cluster id)` | `feols --y y --x x --fe f1,f2 --vcov cluster --cluster id` |
| **`xtreg y x, fe`** | **`feols --y y --x x --fe <panelvar>`** (xtreg is RE-only) |
| `xtreg y x, re` | `xtreg --y y --x x --ivar <id>` |
| `ivregress 2sls y x (d = z1 z2)` | `iv --y y --x x --endog d --instruments z1,z2` |
| `logit / probit / poisson / nbreg / tobit / heckman / qreg` | same-name subcommand (poisson → `fepois`) |
| `test a=b` / `margins, dydx(*)` / `predict yh, xb` / `lincom` | last_model spec → cobra |
| `summarize` | `cobra eval --summarize --data <hbin>` |
| `generate z=expr` | `cobra eval --gen "z=expr" --data <hbin> --out <new.hbin>` |
| `replace z=expr if E` | `cobra eval --gen "z=cond((E),(rhs),z)" --out` |
| `egen m=mean(x), by(g)` | `cobra eval --egen "m=mean(x)" --by g --out` |
| `drop / keep / drop if / keep if` | `cobra eval --drop/--keepif --out` |
| `tabulate / describe / browse (br)` | **python-native** (hbin direct) |
| `xtset id time` | store `panelvar`/`timevar` in session; no cobra call |
| `use file.dta` | `to_hbin.py` → hbin swap; return `DataMutationResult` |

**Explicit rejection gate:** unknown command → `UnsupportedCommand`. Loops/macros/programs/graphics
(named above) → named rejection code + "PIET v0 not supported (next milestone)" message. `c.`/`##`
→ `UnsupportedOption` with "create manually: `gen xz = x*z` for now; `##`/`c.` is the #1 engine
roadmap gap (DID `i.treat##i.post` is the painful case)."

---

## Python-native handlers

### browse (`GET /api/browse?offset=&limit=&vars=`)

`np.memmap`; slice only the requested row window and columns. Factor codes → level labels. NaN/−1 →
`null` (renders as `.`). Default `limit=200`; frontend virtual grid requests windows on scroll.
Response: `{columns[], rows[][], n_rows_total}`.

### tabulate (`tabulate.py`)

- **Oneway:** `np.unique(counts=True)` → freq, cumulative %.
- **Twoway:** `np.add.at` contingency table + row/column margins.
- **Chi²:** `Σ(O−E)²/E`, dof = (r−1)(c−1). P-value via self-contained `chi2_sf` (normalised upper
  incomplete gamma) — no scipy dependency (keeps bundle lean).

### describe / summarize

`describe`: VarInfo from hbin header (n_missing = count of NaN/−1 codes).
`summarize`: numpy min/max/mean/std/N over hbin column arrays.

### .do executor (`doexec.py`)

1. Strip comments (per tokenizer rules); join continuations.
2. Split into lines (`#delimit` → reject immediately).
3. Replay each line through the **same router** as interactive.
4. On first `UnsupportedX` or engine error: emit `error` block with `line_number + context`, halt.
   Never execute past an error.
5. Return `{blocks[], stopped_at_line?, stopped_at_reason?}`.

---

## API contract (server side)

| Endpoint | Method | Returns |
|---|---|---|
| `POST /api/command` | POST | `{blocks: ResultBlock[], varlist_after?: VarInfo[]}` |
| `POST /api/do` | POST | `{blocks: ResultBlock[], stopped_at_line?: int, stopped_at_reason?: str}` |
| `POST /api/open` | POST | `DataMutationResult` (action:"open") |
| `GET /api/varlist` | GET | `{varlist: VarInfo[], n_rows, n_cols}` |
| `GET /api/browse` | GET | `{columns[], rows[][], n_rows_total}` |
| `POST /api/tabulate` | POST | `DataTableResult` with optional chi2 footer |
| `POST /api/describe` | POST | `DescribeResult` |
| `POST /api/summarize` | POST | `SummarizeResult` |
| `GET /api/history` | GET | `[{cmd, ts, success}]` |
| `WS /ws/console` | WS | **deferred**; v0 uses sync `POST /api/do` |

Full `ResultBlock` union schema (canonical): see FRONTEND.md §6. Backend emits these; frontend renders them.

---

## Patch compatibility (summary; full detail: FRONTEND.md §6.2)

- **Boundary A (cobra ↔ kernel):** `--tidy coeftable[]` is frozen. All other cobra fields are
  additive-only (minor); rename/remove/semantic-change = major → lockstep kernel update.
- **Boundary B (kernel ↔ frontend):** `/api/*` fields additive-only (minor).
- **Boot handshake:** kernel checks `cobra version --json` → `{cobra_version, contract_version}` at
  startup; mismatch → explicit warning + status-bar red dot (open question: §11.7 FRONTEND.md —
  does current cobra expose this?).
- **Contract tests (CI):** (a) kernel argv builder → real cobra binary → exit 0 + `coeftable` parse
  + required fields present; (b) `/api/*` responses validated against `ResultBlock` pydantic schema.

---

## Verification checkpoints

| When | Test | Pass condition |
|---|---|---|
| D1 | `python -c` script: `deryugina.hbin` → `cobra feols --tidy` → `coeftable` | Length > 0, numeric b/se values |
| D2 | `test_translate.py` unit tests | `"regress y x, robust"` → `['feols','--y','y','--x','x','--vcov','hc1']`; `"xtreg y x, fe"` → `feols --fe`; macro ref → UnsupportedMacro |
| D3 | curl `POST /api/command` `{"cmd":"regress y x"}` | `blocks[0].type == "estimation"`, coeftable rendered |
| D4 | `tab sex happy, chi2` chi2/p vs Stata `tabulate, chi2` | Match to 4 decimal places |
| D5 | Linear `.do` (use/gen/summarize/regress/predict) → end-to-end replay | No errors; `foreach` .do → halt at that line with line_number |
| D6 | Open `.dta` → `br` → `regress` | Correct obs count, factor labels display |

---

## Reused engine assets (do not reinvent)

| Source | Reuse in PIET |
|---|---|
| `tools/hbin_read.py` | Base for `backend/piet/hbin.py` + `datastore.py` |
| `tools/to_hbin.py` | dta → hbin in-process (value-label→factor, `%td`→epoch-days) |
| `apps/cecon_cli.cpp` | Authority for JSON schema, flag names, `--tidy` coeftable, `i(col)` syntax |
| `src/io/columnar.h` | On-disk hbin layout contract (Python reader must match) |
| `src/core/model.cpp` | Confirms `i(col)` syntax and absence of `c.`/`##` (rejection rule source) |

---

## Post-Wed: packaging & Windows

**Native window:** `entry.py` starts uvicorn on a daemon thread, then calls
`webview.create_window(url="http://127.0.0.1:<port>")`. Uses WKWebView (Mac) / WebView2 (Windows)
— no Chromium bundle.

**PyInstaller (onedir):** bundles Python + backend + `frontend/` + `bin/<os>/cobra`.
Mac cobra links Apple Accelerate (system framework, no bundling needed).
Windows cobra needs OpenBLAS DLLs alongside.

**Windows cobra build (when targeting that machine):**
- Compile with `-march=x86-64` (SSE2 / v2 baseline). `-march=x86-64-v3` is forbidden — that CPU
  has no AVX2; the binary will SIGILL at runtime.
- OpenBLAS: build with `TARGET=NEHALEM` or `DYNAMIC_ARCH=1` to avoid AVX2 kernel selection.
- `/tmp` workaround: before any `--keepif` usage, PIET runs
  `cobra eval --keepif "EXPR" --data <in.hbin> --out <workdir/filtered.hbin>` first, then passes
  `filtered.hbin` as `--data` to the estimator. The kernel owns this routing; the frontend is
  unaffected.

**dta native (future):** when `cobra convert x.dta x.hbin` lands (readstat-based), swap
`to_hbin.py` in-process call for a `cobra_runner` call — drop pandas from the bundle.
