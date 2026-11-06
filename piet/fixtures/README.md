# PIET fixtures (planner-owned)

- **golden/** — PUBLIC-data `cobra <est> … --tidy` JSON (raw engine shape: `coef{}`/`ci_lo{}`/`ci_hi{}` dicts +
  `coeftable[]`). Data: Stata `sysuse auto` (`price`/`mpg`/`weight`/`length`/`rep78`/`foreign`), 74 obs.
  The frontend builds `formatEstimation()` against these numbers here, **no kernel needed**:
  - `feols_plain` — iid, `price ~ mpg + weight`
  - `feols_robust` — HC1, `price ~ mpg + weight`
  - `feols_cluster` — cluster by `rep78` (5 clusters), `price ~ mpg + weight`
  - `feols_factor` — iid, `price ~ weight + i(rep78)` → dummies `rep78==2…5`, base dropped
  - `feols_fe` — FE absorbed `rep78` (5 levels), cluster by `rep78`, `price ~ mpg + weight`
  - `feols_multi` — two outcomes `price,mpg`, `x = weight,length` (`estimator:"feols_multi"`, `outcomes[]`)
- **api/** — illustrative, SHAPE-accurate mock `/api/*` responses in the kernel's **normalised** `ResultBlock` shape
  (per the internal `CONTRACT.md`). The frontend's `api.js` develops against these (serve `api/` as static JSON
  or via a ~20-line mock server). **Numbers are illustrative; for parity use `golden/`.**
- **contract_test.py** — validates these fixtures (and, with `--live URL`, a running kernel) against the CONTRACT
  union. Run at each sync point: `python3 piet/fixtures/contract_test.py`.

Regenerate goldens from public data (Stata `sysuse auto`, 74 obs):
```sh
AUTO=tests/validate/auto.hbin  # path from repo root

cobra feols --data $AUTO --y price --x mpg,weight --tidy > golden/feols_plain.json
cobra feols --data $AUTO --y price --x mpg,weight --vcov hc1 --tidy > golden/feols_robust.json
cobra feols --data $AUTO --y price --x mpg,weight --vcov cluster --cluster rep78 --tidy > golden/feols_cluster.json
cobra feols --data $AUTO --y price --x "weight,i(rep78)" --tidy > golden/feols_factor.json
cobra feols --data $AUTO --y price --x mpg,weight --fe rep78 --vcov cluster --cluster rep78 --tidy > golden/feols_fe.json
cobra feols --data $AUTO --y price,mpg --x weight,length --tidy > golden/feols_multi.json
```
**Engine facts to remember** (verified): `--tidy` is trailing; intercept is named `(intercept)`; `i(col)` needs an
f64 column (factor-dtype → `factor_misuse`); cluster `vcov_type` embeds the var name; quote `i(col)` in shell to
prevent glob expansion.
