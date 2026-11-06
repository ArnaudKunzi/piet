# PIET 1.8.0 — Definitive Capability Map (cobra 0.15.2, contract 1.5.1)

> Generated 2026-06-24 by a 7-agent code audit (dispatch table · translator · native handlers ·
> descriptives/post-est · control-flow · graphics+engine), reconciled against the runtime gate ledger
> (internal ledger 0.15.2, 147-command corpus, 120 GREEN / 0 RED). Where static code-read
> conflicted with runtime truth, the ledger + live verification win.

**Key reconciliation:** the ledger's lines 154–156 ("cobra 0.14.0+ gap") are **stale pre-0.14.0
annotations**. Live-verified working on 0.15.2: `i.region` factor dummies, `c.x1##i.region` interactions,
`L.x2` TS lags. Genuine remaining gaps: `encode`/`destring`/`tostring`/`decode`, and `margins, dydx` after
plain OLS (works after logit/probit). `winsor2`/`coefplot` are SSC packages (Stata itself errors rc=199).

---

## ✅ SUPPORTED (solid core)

**Data load/management:** use (.dta/.hbin, quoted paths), sysuse auto, clear, xtset, tsset, cd/pwd,
merge (1:1/m:1/1:m), append, collapse, reshape (wide/long), duplicates (report/drop/tag/list),
preserve/restore, drop/keep [if] (wildcard+range), sort/gsort/order, recode, clonevar, gen/replace/egen.

**Descriptives:** summarize (+detail), tabulate/tab/tab1/tab2 (+chi2, +sum()), table, tabstat (+by stats),
correlate/pwcorr, ttest, count [if], describe/codebook, browse/list (if/in), levelsof.

**Estimation:** regress (+vce robust/cluster), xtreg fe, logit/logistic/probit, poisson, ivregress 2sls
(+ivreg2/ivreghdfe), csdid/drdid. (xtreg re, nbreg, tobit, qreg, heckman, reg3 = supported with caveats — see ⚠️.)

**Factor & time-series:** `i.var` dummies, `ib#.`/`i(numlist).`/`o.`, `i.A#i.B`, `c.x`/`c.x#c.y`/`c.x##i.f`
interactions, `L./F./D./S.` lags/leads/diffs (need prior xtset/tsset). ← live-verified working.

**Post-estimation:** predict (xb/resid/fe), test (joint + constraint), lincom, nlcom, margins at()
(predictive), margins dydx() **after logit/probit/poisson**, estat hettest/ovtest/vif/dwatson/bgodfrey/archlm,
estat event/pretrend/calendar (post-csdid).

**Scalars/macros/control flow:** local/global (+expansion), scalar, display (%fmt, scalar/_b[]/_se[]/_N,
e() via display), extended macro fns (word count/length/subinstr/list uniq…), r() (from summarize/count),
foreach (in/of varlist/of numlist), forvalues, while, if/else, quietly/capture/noisily, by/bysort, comments.

**Graphics:** scatter, histogram, kdensity, twoway line, tsline, xtline, marginsplot (after logit),
graph export (.png/.pdf/.svg).

---

## ❌ NOT SUPPORTED — by reason

**Engine-gap (cobra lacks it):**
- `margins, dydx` after **plain OLS/feols/xtreg/qreg/tobit** (works after logit/probit/poisson). PIET shows cobra's honest error.
- `predict` stats beyond xb/resid/fe (no cooksd/leverage/hat/stdp/rstandard/dfbeta/…).
- `xtreg, be/gls/mle/pa` (only fe + re); `xtreg` sigma_u/sigma_e/rho variance components not emitted.

**Not wired in PIET (no Stata route):**
- `encode`/`decode`/`destring`/`tostring` (type conversion).
- `save`; `import excel`/`sasxport`/`infile`/`insheet`/`infix`.
- `merge m:m`; `joinby`/`cross`/`stack`/`contract`/`expand`/`fillin`.
- `do FILENAME` as a console command (do-files run via the do-editor / API only).
- `program…end`, `tempvar`/`tempname`/`tempfile`, `tokenize`/`gettoken`, `continue`, `#delimit`.
- `testparm`; `estat ic`/`gof`/`summarize`; `foreach … of local/global`.

**SSC-not-bundled (Stata itself errors rc=199):** `winsor2`, `coefplot`.

---

## ⚠️ PARTIAL — works, but watch out

- **sysuse/webuse** — only bundled `auto`; `webuse` is not a real network fetch.
- **import delimited** — `delimiter()`/`varnames()` silently ignored.
- **rename** — two-name form only (no wildcard/pattern).
- **xtreg, re** — no vce()/robust/cluster; sigma/rho missing (coefficients + R² correct).
- **nbreg/tobit/qreg/heckman** — vce() rejected; need their required opts (ll()/ul(), select(), q()→tau).
- **newey/prais/reg3/sureg/dfuller/pperron/hausman/wildboot** — generic routing; estimator-specific options (lag/rhotype/reps/two-equation) often unparsed. reg3/sureg multi-equation syntax unverified.
- **r() results** — only `summarize`+`count` populate r(); ttest/corr/tabstat/levelsof do NOT expose r().
- **e() scalars** — only via `display`, not inside arithmetic expressions.
- **bysort/by:** — only gen/replace get true group `_n`/`_N`; others just run on pre-sorted data.
- **console loops** — a one-line loop header without an inline `{…}` block errors; full loops via the do-editor.
- **graphics** — `in` range = hard error on every graph; no multi-series overlays `(…)(…)`; kdensity uses
  Gaussian (Stata=Epanechnikov) + the `normal` overlay isn't drawn; coefplot/xtline/tsline/marginsplot
  **export** falls back to a scatter SVG (wrong geometry on disk).

---

## 🔴 Real bugs the audit surfaced (being routed)

1. **`ivregress liml` / `ivregress gmm` silently run 2SLS** — the `liml`/`gmm` token is stripped but
   `--liml`/`--gmm` is never appended, so PIET returns a 2SLS fit *without telling you*. This is the **one
   place PIET can be silently wrong** (everything else errors honestly). → backend wiring fix.
2. **`xtreg` sigma_u/sigma_e/rho not emitted** (coefficients/R² correct) → engine (cobra-op).
3. **Gate stale exclusions** (corpus 154–156) → Eval should re-promote i./##/L. from EXCLUDED to tested.
4. **Native-vs-engine divergence** — PIET's `tabulate`/`describe`/`codebook` use Python handlers, not the
   richer `cobra tabulate`/`codebook` (so Cramér's V, gamma, codebook freq-tables aren't surfaced).

---

**Bottom line:** PIET 1.8.0 covers the full applied-econometrics core — OLS/FE/IV/panel/count/DiD,
factor & TS operators, descriptives, data wrangling, macros/scalars/loops, post-estimation. With **one
exception** (`ivregress liml/gmm`), it never silently returns a wrong number — it raises an explicit
`unsupported_command`/`engine_error`. The honest hard gaps are the specific list above.
