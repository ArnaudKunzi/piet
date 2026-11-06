# perfsweep — estimator stress bench (PERF-CLASS sweep, 2026-09-01)

The instrument behind `internal note PERFSWEEP-RUN-20260901.md`. It exists because the
repo's other timing tools do not cover this ground: `floor_gate.py` times commands
on a small fixture with a control-diff (right for regression floors, wrong for
measuring an O(np·n·k) inner loop), and the acceptance corpus exercises 5 of the 69
estimator rows. This one drives estimators hard enough that their inner loops, not
process startup, decide the wall.

## Run it

```
python3.13 gen_fixtures.py <outdir>          # seed 42, deterministic
python3.13 ../../../tools/to_hbin.py <outdir>/panel_stress.pkl <outdir>/panel_stress.hbin
#   ... same for panel_big, xsec_stress   (surv_stress: see the RUN-LOG)
python3.13 bench.py --a ./cobra_A [--b ./cobra_B] [--reps N] [--only n1,n2] [--json out]
```

`bench.py` alternates A/B/A/B so machine drift cancels and reports the median of N
per arm, ratio only.

## The one rule that is not optional

**Measure the null band first.** `cp cobra cobra_null` and run `--a ./cobra
--b ./cobra_null`: whatever spread that produces is this machine's noise floor for
this command, and nothing inside it is an effect. On 2026-09-01 that band was
+/-2.6%, and a 3-rep run without it read an untouched-file 7% "win" that did not
exist. A control command is not a substitute — short commands (<1 s) are dominated
by process start and swing far wider than the effects being hunted (xtpois read 1.39
on a file nobody had touched).

Two more things that measurement taught here, both in the RUN-LOG:
- Give the mechanism dynamic range. The hoisted buffers scale as np or k^2, so the
  cross-section fixture carries 40 regressors; at k=3 there was no signal to find.
- Do not trust an arm that ran immediately after a heavy multithreaded command. A
  control read 0.839 that way and 0.976 alone in a quiet window.

## Files

`gen_fixtures.py` builds panel_stress (20k panels x T=40), panel_big (T=100, drives
xtlogit's (T+1)^2 tables), and xsec_stress (150k x 40 regressors + endog/instruments
/groups). `bench.py` carries the command table. The `*.json` files are the
2026-09-01 measurements the RUN-LOG cites.
