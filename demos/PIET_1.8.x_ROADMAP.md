# PIET 1.8.x → 1.9 — capability roadmap

> Source: the 2026-06-24 full-code capability audit (7-agent) + the test+debug campaign. Companion to
> [PIET_1.8.0_CAPABILITIES.md](PIET_1.8.0_CAPABILITIES.md). Priorities are the maintainer's; owner lanes noted.

---

## P0 — silent-wrong (fix first; violates "never silently wrong")

- **`ivregress liml` / `ivregress gmm` silently run 2SLS.** translate.py strips the `liml`/`gmm` token but
  never appends `--liml`/`--gmm` to the cobra `iv` call → user gets a 2SLS fit labeled as LIML/GMM, no error.
  This is the *only* place PIET returns a wrong number without surfacing it.
  → **Owner: PIET backend** (translate.py `build_iv_argv`) + confirm the engine `iv` flags exist (cobra-op).

## P1 — high value, engine already does the work (low effort)

- **Wild-cluster bootstrap — surface it properly.** The engine has a complete `cobra wildboot` subcommand
  (`--param`, `--cluster`, reps, seed; `inference/wildboot.h`). PIET only passes `wildboot` through generically
  with the options unwired. Wire the Stata route(s):
  - `regress y x, vce(bootstrap)` and `vce(boottest)` / a `bootstrap:`-style path → map to `cobra wildboot`.
  - `wildboot y x, param(x1) cluster(id) reps(999) seed(…)` → translate the options.
  Few-cluster wild bootstrap is exactly what applied/teaching audiences want; the engine is ready.
  → **Owner: PIET backend** (new translate path) + verify engine arg surface (cobra-op).
- **`xtreg` variance components.** fe/re coefficients + R² are correct, but `sigma_u`/`sigma_e`/`rho` are not
  emitted → the panel header is incomplete vs Stata. → **Owner: cobra-op** (engine emits the scalars) → PIET passthrough.
- **Re-promote the stale gate exclusions.** Corpus lines 154–156 (`i.region`, `c.x1##i.region`, `L.x2`) are
  marked EXCLUDED ("cobra 0.14.0+") but live-verified working on 0.15.2. Move them from EXCLUDED to tested in
  `corpus_spec.py` so the gate actually covers factors/interactions/TS-lags. → **Owner: Eval.**

## P2 — genuine feature gaps (not wired in PIET)

- **Type conversion:** `encode` / `decode` / `destring` / `tostring` (needs engine string-dtype support).
- **`save`** (write .dta/.hbin) — no handler today.
- **`predict` extras:** cooksd / leverage / hat / stdp / rstandard / dfbeta … (engine: only xb/resid/fe).
- **`margins, dydx` after linear models** (OLS/feols/xtreg/qreg/tobit) — engine has no linear-dydx path
  (works after logit/probit/poisson). → engine feature (cobra-op).
- **`xtreg be/mle`**, **`ologit`/`mlogit`/`clogit`**, **zero-inflated count** — additional estimators.

## P3 — surface richer engine features PIET hides

- PIET's `tabulate` / `describe` / `codebook` use Python handlers, not the richer `cobra tabulate` / `codebook`
  (Cramér's V, gamma, tau-b, LR-χ²; codebook percentile/freq tables). Route to the engine subcommands.
- **`reg3` / `sureg`** multi-equation `(…)(…)` syntax — tokenizer likely doesn't parse it; verify + wire.
- **`newey` / `prais` / `dfuller` / `pperron` / `hausman`** — estimator-specific options (lag/rhotype/two-est)
  hit the unknown-option guard; wire them.
- **`r()` coverage** — only summarize/count populate r(); extend to ttest/correlate/tabstat/levelsof.
- **vce() on `nbreg`/`tobit`/`qreg`/`heckman`** — currently rejected; wire robust/cluster where the engine supports it.

## P4 — graphics polish

- `in` range qualifier is a hard error on every graph command (only `if` works).
- No multi-series overlays `twoway (…)(…)`.
- `kdensity` uses Gaussian (Stata default = Epanechnikov); the `normal` overlay is computed but not drawn.
- **Export fidelity:** `graph export` of xtline/tsline/marginsplot/coefplot falls through to a *scatter* SVG
  (on-screen render is fine; the saved file has wrong geometry).
- `coefplot` has no frontend renderer (also an SSC package).

---

**Notes.** Nothing here blocks normal use — PIET 1.8.0 covers the applied-econometrics core and (except the P0
`ivregress` case) raises an honest `unsupported_command`/`engine_error` rather than returning a wrong number.
The highest-leverage near-term work is **P0 (the silent ivregress bug)** and **P1 (bootstrap wiring + xtreg
variance components)** — small surface changes over capabilities the engine already has.
