# PIET + CoBrA — a free, offline, Stata-syntax-compatible econometrics tool

**PIET** is a desktop application for learning and doing econometrics with the
command syntax many researchers already know. It reads `.do` scripts and `.dta`
datasets, runs them through its own independent computation engine, and renders
tables, regression output, and graphs — fully offline, no license server, no
telemetry.

**CoBrA** (C-Oriented Bootstrap & Regression Accelerator) is the engine: a
dependency-light C++17 library and CLI for high-dimensional fixed-effects
econometrics — OLS/IV/2SLS/LIML, FE-Poisson (PPML), panel models, tobit,
Heckman, quantile regression, Callaway–Sant'Anna staggered DiD, HAC/Newey–West,
robust/cluster/wild-bootstrap inference, and a Stata-style data-preparation
layer (`generate`/`egen`/`merge`/`joinby`/`reshape`/`collapse` …). Heavy linear
algebra goes through the platform BLAS (Apple **Accelerate** on macOS — the AMX
path on Apple Silicon); the engine never reimplements BLAS.

PIET is aimed at students, instructors, and researchers who want familiar
syntax without a paid license. It is **not** a copy, port, or clone of Stata,
and contains no Stata source code, manual text, or other StataCorp-authored
material (see *Relationship to Stata* below).

## What works today

- **160+ commands** across the data-management, estimation, post-estimation,
  graphics, and programming surfaces (loops, locals/globals, programs,
  `eclass`/`sclass` returns, matrices, frames, `preserve`/`restore`).
- **Resampling inference**: `bootstrap`, `jackknife`, `permute`, wild cluster
  bootstrap; deterministic seeded streams.
- **Parallel execution**: `parallel, workers(N)` fan-out with byte-identical
  output across worker counts.
- **File formats**: native `.dta` reader (legacy formats through 118, strL
  included) and 118 writer, CSV/XLSX import-export, and `.hbin`, a
  memory-mapped columnar format for fast repeated loads.
- **Graphs**: twoway families, event-study/coefficient plots, histograms,
  binscatter, `graph export` to PNG/SVG.

## Accuracy

Numbers are the product. Every estimator is validated in three rings before it
ships:

1. **Synthetic exact-answer unit tests** (ctest).
2. **Cross-engine golden fixtures** captured from real Stata runs and pinned
   byte-for-byte; the release gate re-runs the full fixture corpus on every
   stamp.
3. **Cell-level parity** against a live Stata oracle on realistic workloads,
   held to a documented tolerance ladder (baseline relative tolerance 1e-9,
   i.e. the displayed digits plus two guard digits; see
   `tests/eval/TOLERANCE.md`).

Where PIET cannot honestly reproduce a quantity, it refuses with an error
rather than returning a plausible number — "honest reject over silent wrong
answer" is the project's first law, and known numerical limitations are named
in the release notes rather than hidden.

## Build

Requires CMake ≥ 3.20 and a C++17 compiler. Currently macOS-first (Accelerate);
Windows and Linux ports are in progress (`docker/` holds the cross-build recipes).

The one-command path, which is also what the CI workflow runs:

```sh
sh scripts/build-macos.sh      # builds both executables, runs ctest and the public smoke test
```

Step by step:

```sh
# the engine (repo root)
cmake --preset release
cmake --build --preset release -j4
ctest --preset release

# the PIET app — a standalone CMake project with its own presets
cd piet/native && cmake --preset release && cmake --build --preset release -j4
```

Without presets the equivalent is `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
and `cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release`.

**Tests from a fresh clone.** `ctest` (the engine's unit tests) and
`tests/public_smoke.py` (import → regress → save → reload through the app, checked
against an independent OLS) run self-contained; `scripts/build-macos.sh` runs both.
`piet_native --selftest` is the maintainers' suite: most of its arms read private
oracle fixtures (Stata captures and datasets the repo deliberately does not ship),
so in a clone it reports those arms as failed or skipped rather than passing.

The app finds its engine on its own when both are built in place — it looks
beside itself, then at the repo-root `build/cobra`. Set `COBRA_BIN` to override
that (test harnesses do, to pin an exact engine); packaged builds bundle their
own. See `piet/README.md` for app-side details and the UI test harness.

## Layout

```
src/ apps/        CoBrA engine (io, core, estimators, vcov, inference, CLI)
piet/native/      PIET application (C++ core, WKWebView UI, dispatcher)
piet/frontend/    UI (HTML/CSS/JS)
tests/            unit tests, golden fixtures, parity harnesses
tools/            converters and audit tooling
vendor/           vendored single-header dependencies
```

## Relationship to Stata

PIET is not affiliated with, endorsed by, or sponsored by StataCorp LLC.
"Stata" is a registered trademark of StataCorp LLC. PIET is an independently
developed tool that implements a compatible command syntax and reads/writes
compatible file formats; command languages and file formats are not subject to
copyright. See `NOTICE.txt` for the full non-affiliation statement, trademark
rationale, and per-author credits for community-package command names.

## License

**GPL-3.0-or-later.** See `LICENSE.txt`. Third-party components are listed
with their licenses under `piet/packaging/licenses/`, including the
adopted-code register (`40-ADOPTED-CODE.txt`), which records the provenance of
any code adopted from GPL-compatible sources.

## Citing

If you use PIET's Callaway–Sant'Anna staggered difference-in-differences
(`csdid`) estimator in published or for-credit academic work, please cite:

> Callaway, Brantly, and Pedro H. C. Sant'Anna. 2021. "Difference-in-Differences
> with Multiple Time Periods." *Journal of Econometrics* 225(2): 200-230.
>
> Sant'Anna, Pedro H. C., and Jun Zhao. 2020. "Doubly Robust
> Difference-in-Differences Estimators." *Journal of Econometrics* 219(1):
> 101-122.

If you use PIET more generally in published work, please also cite the tool
itself; the citation form will accompany the first citable public release.

<!-- hook live-fire 2026-09-02: docs-only commit on main -->
