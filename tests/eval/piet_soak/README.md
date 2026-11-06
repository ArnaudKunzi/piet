# piet_soak — BUG-124 standing soak gate

Resident-session storm + disk-exhaustion honesty for the FULL PIET client
(`piet_native --do`), sibling of `../serve_session` (which soaks the engine
protocol; this soaks the client lifecycle above it).

## Provenance

BUG-124 (2026-07-18, P0): the B.5.0 exit-gate fair-run SIGSEGV'd in
`Datastore::buildVarInfo` after a string-gen, and a second run died silently
(exit 1, empty transcript). Root cause — **not** a reclaim/mmap timing race:
the local disk filled mid-run; cobra's `convert` failed honestly (ENOSPC,
BUG-109 write-check) but had already created its `--out` with a complete
header and short column blobs; PIET **discarded the result** (`runCobra()`
stopped throwing when the Stage-1 serve router replaced the one-shot, so the
surrounding `catch (CobraError)` was dead), the torn file passed the
`native_exists()` check, was `load()`ed + mmap'd, and the next varlist scan
walked off the mapping end — `KERN_INVALID_ADDRESS` at exactly region end.

Primary evidence (`internal capture`): the
original `.ips`, and the torn `strmut_…_10.hbin` (27,594,752 bytes; header
claims 165,827,584 — its mmap rounds to 27,607,040 bytes, matching the .ips
fault region to the byte). Mechanism reproduced on demand pre-fix (fill disk
to ~1.4 GB free → same stack, same 10th-file torn write at 84.6 s) and
verified fixed post-fix under identical conditions.

## Why existing gates missed it

selftest / freeze / DOM / pangram / serve-parity legs are single-shot or
short-session and always run with disk space available. The class needs (a) a
long mutation-churn session and (b) an exhausted filesystem — neither existed
in any standing gate.

## Legs

| Leg      | What it does | PASS |
|----------|--------------|------|
| `storm`  | N cycles of numeric gen / string gen (strmutate) / replace-if / egen / preserve→drop→restore / tempfile save+use, run twice under `PIET_SCRATCH_BUDGET_MB=0` (eager reclaim) + once under the default budget | all runs exit 0, zero error blocks, no signal death; the eager pair's transcripts byte-identical after volatile masking |
| `enospc` | the same storm inside a 192 MB APFS sparseimage sized to fill mid-run | **no signal death** (SIGSEGV/SIGBUS = the BUG-124 class), an honest disk-full/`data_io` error surfaced (transcript, or the stderr FATAL line when the transcript itself cannot be written), and **no torn .hbin survives** (every remaining .hbin's header extents fit its file size); SKIP if `hdiutil` unavailable |
| `torn`   | truncate a valid .hbin at 1/6 (the exact BUG-124 shape) and `use` it | honest `data_io` error block, no crash — the permanent regression pin for the .ips crash |

## Run

```sh
python3 run_piet_soak.py --piet <path/to/piet_native> --cobra <path/to/cobra> \
    [--cycles 12] [--leg all|storm|enospc|torn] [--keep]
```

Exit 0 = all requested legs PASS (SKIP is honest and does not fail the gate).
