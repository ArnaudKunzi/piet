# cecon benchmarks

Machine: Apple M1, 8 GB, macOS. Panel: `deryugina_panel` (620,279 rows).
Model: `y_all ~ pm25 + tmaxb + tminb + precd + wsd | sigungu5 + year`,
cluster by `sigungu5`. cecon demean tol = 1e-12.

## Phase 1 — HDFE-OLS, single regression

| stage | cecon | pyfixest 0.50.1 |
|---|---|---|
| import / JIT warmup | none | ~seconds (numba) |
| data load | **2.3 ms** (mmap .hbin) | ~186 ms (read_parquet) |
| build (factors, copy) | **17 ms** | — |
| fit (demean + OLS + cluster vcov) | **~238 ms** | ~251 ms (warm, in-memory) |
| **full process, warm** | **~0.39 s** | **~8.55 s** (cold import + load + fit) |

Takeaways:
- **Per-invocation wall clock: ~22× faster** (0.39 s vs 8.55 s). This is the win
  that matters for the real workflow — dozens of standalone scripts that each
  re-pay Python import + parquet load. cecon's mmap'd `.hbin` loads in ~2 ms.
- **Pure compute (warm fit) is at rough parity** with pyfixest now that the
  demean is parallelized over the 4 P-cores; both ultimately lean on the same
  BLAS. The decisive compute win is the resampling inference in Phase 3
  (bootstrap/permutation), where cecon demeans once and parallelizes replicates.

## Correctness & reproducibility

- cecon matches a high-precision numpy within-estimator oracle to **~1e-13**
  (coefficients) and **~1e-14** (cluster/iid SEs).
- cecon is **bit-reproducible** across runs and across `--threads 1` vs `4`.
- pyfixest is **non-deterministic across process launches**: its parallel
  demeaner at default `fixef_tol=1e-8` yields SEs that wobble up to ~7e-6 between
  runs (confirmed: pm25 SE = 0.001020049 on two runs, 0.001020057 on a third).
  This is why the validation harness gates on the numpy oracle, not pyfixest.

## Phase 3 — wild cluster bootstrap (9,999 reps, 620k rows, cluster sigungu5)

| model | cecon time | result |
|---|---|---|
| FE-OLS, pm25 | **0.54 s** | t0=5.98, p<0.0001 |
| 2SLS/IV, pm25~wq | **0.92 s** | t0=−0.47, p=0.64 |

- Naive baseline (re-running the regression 9,999×): FE-OLS ≈ 9,999×1.72s ≈ **4.8h**;
  2SLS ≈ 9,999×4.64s ≈ **12.9h**. cecon's fast WCR algorithm (demean + per-cluster
  pieces once; each replicate O(G²)) collapses this to <1s.
- Stata `boottest` (the fair fast-algorithm comparison) is **not installed** in
  this StataBE, so no side-by-side Stata bootstrap time is available.
- **Correctness**: the fast t* equals a brute-force "form y*, refit, recompute
  cluster SE" t* for every replicate to <1e-9 (unit test, OLS and IV); beta/SE
  match the point estimators exactly; bit-reproducible across runs/threads.
