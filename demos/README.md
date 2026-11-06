# PIET demo kits

Self-contained teaching demos that double as a **durability gate**: each kit is
a real dataset plus a commented "pangram" do-file that exercises the core
PIET / cobra surface, and every block must run GREEN through the shipped app.
If a change to the engine or the do-executor breaks a demo, the release stops
until the demo is fixed.

## The demo law

A kit lives in `demos/<name>/` and MUST contain exactly these parts:

1. **Data** — the dataset the kit runs on (e.g. `fatalities.csv`), committed so
   the kit is self-contained, with a one-line **provenance note** (source +
   licence) at the top of the handbook. Repo-root `.gitignore` blanket-ignores
   `*.csv`; re-include a kit's data file with a negation line in
   `demos/.gitignore`.
2. **A commented pangram `.do`** — `<name>_tour.do`. One brief comment per
   command saying WHAT it does. Only command syntax the **shipped** do-executor
   supports; where a capability is a roadmap gap, substitute a supported
   analogue and flag it in-line. No `capture` masking — the tour runs clean.
3. **A handbook** — `HANDBOOK.md`. The econometrics/teaching prose: the model,
   the estimator, the standard-error choice and why, the identification
   assumptions and their credibility, and the interpretation of the **actual
   numbers** the verified run produced.
4. **All blocks GREEN** — the `.do` runs end-to-end through a shipped
   `piet_native` with **zero error blocks** (verified by `run_demos.sh`).

Generated graph artifacts (`*.svg`, `*.png`, `*.pdf`) are ignored, not
committed — they are reproducible by re-running the kit.

## The gate

```sh
# auto-discovers the newest releases/product/*/PIET.app binary:
demos/run_demos.sh

# or pin a specific binary:
demos/run_demos.sh /path/to/PIET.app/Contents/MacOS/piet_native
```

Runs every `demos/*/*.do` headless (each with cwd = its own kit dir so relative
data paths resolve), prints a per-kit GREEN/RED table, and exits non-zero if any
block errors. Dependencies: `bash` + `python3` only. This is **release-checklist
step 5b** (VERSIONING.md).

## Ownership — stop-the-line

The **PIET Conductor** is custodian of the demo suite. But any operator whose
change turns a demo RED **owns the fix before their merge lands** — a red demo
is a stop-the-line event, not a follow-up ticket. Substitutions for roadmap gaps
are allowed and must be flagged in the do-file and the handbook; silently
dropping a broken block is not.

## Kits

| Kit | Data | Topics |
|-----|------|--------|
| [`fatalities/`](fatalities/) | US traffic fatalities panel (48 states × 1982–1988) | data prep · pooled OLS · panel FE (one/two-way) · SE hierarchy (iid/HC1/cluster) · IV by hand (ILS + weak-IV) · log-linear count analogue · Callaway–Sant'Anna staggered DiD · graphics (SVG) · ts operators |
