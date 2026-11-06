# cobra 0.15.2 — Definitive Engine Capability Manual

> Generated 2026-06-24 by a 10-agent deep audit: each estimator family was read from source **and**
> every command was run on real data (`/tmp/ks.hbin` 960×15 panel; `panel_synth.hbin` 800; `auto.hbin` 74).
> Dispatch authority: `apps/cecon_cli.cpp:4421-4474` — **52 subcommand arms** (all covered in §4).
> Legend: ✅ ran clean · ⚠️ ran with a real caveat · ❌ errored / absent.

---

## 1. Capability matrix by category

### 1.1 Linear & HDFE
- **ols** ✅ — pooled OLS (QR); emits classic + HC1 SE (default inference HC1). Factor ops `i./c./ib#./i(numlist)./o.`, `#`/`##`. `regress y x, robust` parity. *(n=941 r2=0.169 x1=0.744)*
- **feols** ✅ — **HDFE-OLS** via FWL within-transform (alternating projections + Anderson accel; iterative singleton drop; multi-outcome shares absorption). vcov iid/hc1/cluster/twoway. Emits `r2_within/between/overall, F, fe_exact, nested_fe`. `reghdfe`(2-FE exact dof)/`areg`(1-FE); 3+FE dof conservative (`fe_exact=false`). *(--fe id,year → fe_exact=true)*
- **predict** ✅ — within-fitted (`xb`) + within-residuals under `--fe` (FWL; not full y-hat); writes a new .hbin.

### 1.2 IV / weak-ID / systems
- **iv** ✅ — 2SLS + optional two-step `--gmm`; FE absorption; weak-ID = KP rk Wald F, MOP effective F, KP LM, Hansen J. `ivregress`/`ivreg2`/`ivreghdfe` parity. **Does NOT drop singleton FE.**
- **liml** ✅ — LIML + Fuller k-class (κ = min eigenvalue). **No FE, no robust/cluster** (i.i.d. only).
- **iv-diag** ✅ — i.i.d. IV post-est: Durbin/Wu-Hausman endogeneity, Sargan/Basmann overid, first-stage F + partial R².
- **reg3** ✅ — SUR or 3SLS systems (`--method sure|3sls`, `--eq` repeated). Σ÷n; no robust/cluster/FE.

### 1.3 GLM / limited-dependent
- **logit / probit** ✅ — binary MLE (IRLS); iid + robust(HC0/score); McFadden pseudo-R². ⚠️ `--tidy` emits **null p/CI** (use native `coefs[]` t).
- **tobit** ✅ — censored-normal MLE (`--ll/--ul`, ≥1 required); observed-info VCE. No robust/cluster.
- **nbreg** ⚠️ — NB2 (β, lnα); iid/robust(HC1)/cluster. Can hit `no_convergence` on degenerate fits.
- **fepois** ✅ — Poisson **PPML + HDFE** (IRLS + demean); separation drop; `--scale x2` quasi-Poisson; multi-outcome. `ppmlhdfe`/`fepois` parity.
- **poisson** ❌ — **not a command** → use `fepois` without `--fe`.
- **margins** ✅ — AME (logit/probit/poisson, delta method) OR predictive `--at`. ⚠️ AME has **no p/CI** (t only); no tobit/nbreg AME.

### 1.4 Panel
- **xtreg** ✅ — RE GLS (Swamy-Arora variance components); `sigma_e/sigma_u/rho`. **No robust/cluster.** ⚠️ `--tidy` null p/CI.
- **xtpois** ✅ — conditional FE Poisson (HHG 1984); iid / Wooldridge-1999 robust.
- **xtlogit** ⚠️ — Chamberlain conditional FE logit (exact LL). Drops constant-y panels; **aborts if all panels drop** (e.g. time-invariant y). No `--keepif/--offset`.
- **hausman** ✅ — FE-vs-RE (1978) on common slopes; PSD-clip fallback. ⚠️ `--tidy` null fields.

### 1.5 DiD / causal
- **csdid** ✅ — **Callaway-Sant'Anna staggered DiD** via DR-IPW (Sant'Anna-Zhao); aggregations simple/event-study/calendar/pretrend; `--comparison never|notyet`, `--pscoretrim`, `--window`. Validated ~1e-11 vs Stata `drdid`. ⚠️ only `dripw` method; SE = cluster-by-ivar; no bootstrap. *(simple_att=1.644 se=0.181 pretrend p=0.926, 11 event bins)*
- **TWFE / "normal" DiD** ✅ — recipe via `feols --x i.treat --fe id,year` (or `i(treat)##i(post)`), HC1/cluster/twoway SE. *(treat==1 b=1.567 se=0.152 p=3.5e-18)*. ⚠️ not heterogeneity-robust for staggered adoption — use csdid.

### 1.6 Quantile / selection / mediation / break
- **qreg** ✅ — quantile regression (Frisch-Newton interior point); iid fitted-sparsity SE (Hall-Sheather). No cluster/HC/bootstrap/weights.
- **heckman** ⚠️ — two-step Heckit (1979); probit selection + IMR + sandwich VCE. **No MLE** (`--twostep` decorative); singular VCE → SE=0 on near-collinear data.
- **mediate** ✅ — Baron-Kenny product-of-coefficients; Sobel/Aroian/Goodman + Philox bootstrap (pct + BC CI). OLS-only.
- **chow** ✅ — Chow stability test by `--by`: classic F + robust Wald. ⚠️ `--vcov cluster` **silently downgrades to HC1**; numeric group only.

### 1.7 Inference / variance (on feols)
- **iid** ✅ σ²(X'X)⁻¹ · **hc1** ✅ (validated ~1e-12 vs ivreg2) · **cluster** ✅ CR1, t(G−1), nested-FE dof · **twoway** ✅ Cameron-Gelbach-Miller, PSD-clip — **hard-capped at exactly 2 dims** (3+ multiway unimplemented). No HC2/HC3 anywhere.
- **wildboot** ✅ — wild-cluster bootstrap-t (null-imposed); Rademacher/Webb, auto-Webb if G<12; Philox RNG; `boottest` (Roodman) parity. One cluster dim only. *(--param x1 --cluster cl → t0=10.03 p_sym=0.034, G=4→Webb)*

### 1.8 Time-series (single-series; subset per unit with `--keepif 'id==1'`)
- **newey** ✅ HAC (Bartlett; lag0→HC1) · **prais** ✅ Prais-Winsten/Cochrane-Orcutt AR(1) FGLS · **dfuller** ✅ ADF (Fuller CV, MacKinnon p, 4 cases) · **pperron** ✅ Phillips-Perron · **tsops** ✅ L/F/D/S operators (panel-keyed, gap-safe; writes .hbin) · **tsfill** ✅ balance panel · **dwatson** ✅ (no p) · **durbinalt** ✅ · **bgodfrey** ✅ · **archlm** ✅ · **wntestq** ✅ Ljung-Box Q.

### 1.9 Data-prep / descriptive / post-est
- **eval** ✅ — gen/egen(max/min/mean/median/sum/count/sd/rank/std/iqr/pctile/tag/group/seq)/rowegen/mkspline(RCS)/bygen/winsor/xtile/ipolate/xtset/tsma/drop/keepif/summarize.
- **collapse** ✅ (mean/sum/sd/count/max/min/median/p#/first/last) · **convert** ✅ CSV→.hbin · **corr/pwcorr** ✅ (+`--sig`) · **ttest** ✅ (1/2-sample/Welch/paired) · **sdtest** ✅ · **tabstat** ✅ · **tabulate/tab** ✅ (+chi2/LR/Cramér's V/gamma/tau-b) · **codebook** ✅ · **vif** ✅.
- **test** ✅ (Wald `--joint`/`--constraint`) · **lincom** ✅ · **nlcom** ✅ (delta method) · **hettest** ✅ (BP/CW/Koenker/`--white`) · **ovtest** ✅ (Ramsey RESET).
- **imtest** ❌ — **not a command** → White's test via `hettest --white` (normality sub-test genuinely absent).

### 1.10 Engine / orchestration
- **version** ✅ · **info** ✅ (schema dump) · **run** ✅ (single-model JSON spec → feols/iv/fepois) · **grid** ✅ (batch; same-design models auto-merge to one multi-outcome batch).

---

## 2. "How to do X" — literal command lines confirmed live

**TWFE / "normal" DiD** (ATT on treatment, unit+time FE, cluster by unit):
```
cobra feols --data DATA --y y --x i.treat --fe id,year --vcov cluster --cluster id --tidy
```
**Staggered / heterogeneity-robust DiD (Callaway-Sant'Anna):**
```
cobra csdid --data DATA --y y --ivar id --time year --gvar cohort --x x1,x2
```
(`--comparison notyet`, `--window -3,3` optional.)

**reghdfe / multi-way HDFE** (2 FE absorbed, two-way cluster):
```
cobra feols --data DATA --y y --x x1,x2 --fe id,region --vcov twoway --cluster id,year --tidy
```
**IV + weak-ID diagnostics (2SLS, cluster-robust):**
```
cobra iv --data DATA --y y --endog x1 --instruments z1,z2 --x x2 --vcov cluster --cluster cl --tidy
```
(`--gmm` for two-step GMM; `cobra iv-diag …` for classic Durbin/Sargan/first-stage. Pre-drop singletons yourself.)

**Panel FE / RE / Hausman:**
```
cobra feols  --data DATA --y y --x x1,x2 --fe id --vcov cluster --cluster id   # FE within
cobra xtreg  --data DATA --y y --x x1,x2 --ivar id                              # RE Swamy-Arora
cobra hausman --data DATA --y y --x x1,x2 --ivar id                             # FE vs RE
```
**Wild-cluster bootstrap (few clusters):**
```
cobra wildboot --data DATA --y y --x x1,x2 --param x1 --cluster cl --reps 9999 --seed 42
```
**Pooled Poisson:** `cobra fepois --y y --x x1,x2` (omit `--fe`). **White test:** `cobra hettest --y y --x x1,x2 --white`.

---

## 3. ❌ NOT in the engine (confirmed absent — all return `unknown command`)

| Method | Stata equiv | What adding it would take |
|---|---|---|
| **PSM / NN matching** | `psmatch2`, `teffects psmatch/nnmatch` | propensity step (logit exists) + matching algo (NN/caliper/kernel/Mahalanobis) + Abadie-Imbens bias-corrected ATT + SE |
| **Regression discontinuity** | `rdrobust`, `rdbwselect`, `rdplot` | running-var/cutoff + local-poly each side + CCT optimal bandwidth + robust bias-corrected CI + fuzzy-RD + McCrary density |
| **Synthetic control** | `synth` (ADH) | convex QP donor weights + placebo/permutation inference + predictor-balance table |
| **`poisson`, `imtest`** | — | covered by `fepois` (no `--fe`) and `hettest --white`; imtest normality sub-test genuinely absent |

**Per-command limits worth knowing:**
- **No HC2/HC3** (only HC1); **no panel HAC** (newey is single-series); **two-way cluster capped at 2 dims**.
- **iv/liml/iv-diag/reg3** don't drop singleton FE (iv) / have no FE at all (liml, iv-diag); liml/iv-diag i.i.d.-only.
- **logit/probit/xtreg/hausman `--tidy` emit null p/CI** — use the native `coefs[]` t-stats. *(This is why PIET should read native coefs, not the tidy table, for these.)*
- **chow `--vcov cluster` → silently HC1**; **heckman** two-step-only, singular-VCE → SE=0; **nbreg** can `no_convergence`; **xtlogit** aborts if all panels drop, no `--keepif/--offset`.
- **predict under `--fe`** = within-fitted (no FE intercepts added back), always IID.
- **margins AME** only logit/probit/poisson, no p/CI.
- **`L.x` + `--fe`** needs the lag materialized via `tsops` first in some configs (else `missing_column`).

---

## 4. Coverage — all 52 dispatch arms
Every arm in `apps/cecon_cli.cpp:4421-4474` is in §1: version·info·convert·eval·ols·feols·iv·fepois·wildboot·csdid·xtpois·xtreg·nbreg·xtlogit·test·nlcom·lincom·corr/pwcorr·vif·sdtest·ttest·tabstat·tabulate/tab·codebook·qreg·mediate·chow·logit·probit·tobit·heckman·reg3·margins·predict·hausman·hettest·ovtest·dwatson·durbinalt·bgodfrey·archlm·wntestq·newey·prais·iv-diag·liml·dfuller·pperron·tsops·collapse·tsfill·run·grid. `poisson`/`imtest` correctly characterized as non-commands.
