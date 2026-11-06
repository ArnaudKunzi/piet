#!/usr/bin/env python3
"""BUG-124 standing soak gate -- resident-session storm + disk-exhaustion honesty.

Why this gate exists (BUG-124, 2026-07-18): the B.5.0 fair-run SIGSEGV'd in
Datastore::buildVarInfo after a string-gen. Root cause was NOT a timing race:
the local disk filled mid-run, cobra's convert died with an honest ENOSPC
error AFTER creating its --out with a complete header and short column blobs,
PIET discarded the result (runCobra() stopped throwing when the serve router
replaced the one-shot; the surrounding catch was dead), the torn file passed
the exists() check, got mmap'd, and the next varlist scan walked off the end
of the mapping (KERN_INVALID_ADDRESS at exactly region end -- .ips + the torn
27,594,752-byte hbin both preserved under
internal capture).

The selftest/freeze/DOM/pangram legs are single-shot or short-session and run
with disk space available, so this class was invisible to every existing
gate. This soak closes the two blind spots:

  Leg "storm"  -- long resident session with mutation churn (numeric gen /
                  string gen / preserve / restore / keep-drop / tempfile
                  save+use cycles), run TWICE under PIET_SCRATCH_BUDGET_MB=0
                  (eager reclaim -- maximum unlink pressure on the scratch
                  lifecycle) and once under the default budget.
                  PASS = every run exits 0 with zero error blocks, no signal
                  death, and the two eager transcripts are byte-identical
                  after volatile-field masking (byte-stability).

  Leg "enospc" -- the same storm inside a deliberately tiny APFS sparseimage
                  (hdiutil), sized so the scratch churn exhausts it mid-run.
                  PASS = the process NEVER dies on a signal (SIGSEGV/SIGBUS =
                  FAIL -- that is BUG-124), the run surfaces an honest
                  disk-full/data_io engine error (in the transcript when the
                  transcript survives; the fixed binary's stderr FATAL line
                  otherwise), and every *.hbin left in the scratch workdir
                  parses cleanly (header column extents <= file size), i.e.
                  no torn file survived where a reader could later find it.
                  SKIP (honest, like serve_session day-1) if hdiutil is
                  unavailable.

  Leg "torn"   -- deterministic reader-honesty pin: truncate a valid .hbin at
                  1/6 of its size (the exact BUG-124 shape: full header,
                  short blobs) and `use` it. PASS = honest data_io error
                  block, exit without signal death. This is the permanent
                  regression trap for the .ips crash even if the writer-side
                  holes ever regress.

Usage:
  run_piet_soak.py --piet /path/to/piet_native --cobra /path/to/cobra \
                   [--cycles 12] [--leg all|storm|enospc|torn] [--keep]

Exit 0 = all requested legs PASS (SKIP counts as pass with a note).
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def _pin_gate_settings(env, _parents=3):  # BUG-388 (2026-09-02): never the user's live settings.json
    """Point the spawned piet_native at a GATE-OWNED settings file (tools/engine_arm.py::pin_settings)."""
    import sys as _s, tempfile as _t
    from pathlib import Path as _P
    _tools = str(_P(__file__).resolve().parents[_parents] / "tools")
    if _tools not in _s.path:
        _s.path.insert(0, _tools)
    import engine_arm
    engine_arm.pin_settings(env, _t.mkdtemp(prefix="piet_gate_settings_"))

# ---------------------------------------------------------------------------


def make_fixture_csv(path: str, rows: int = 4000) -> None:
    """Small mixed numeric/string dataset the storm mutates."""
    with open(path, "w") as f:
        f.write("id,grp,x,y,name\n")
        for i in range(rows):
            f.write(f"{i},{i % 7},{(i * 37) % 1000 / 10.0},{(i * 91) % 500 / 5.0},n{i % 13}\n")


def make_fixture_hbin(cobra: str, workdir: str, rows: int = 4000) -> str:
    csv = os.path.join(workdir, "soak_fixture.csv")
    hbin = os.path.join(workdir, "soak_fixture.hbin")
    make_fixture_csv(csv, rows)
    r = subprocess.run(
        [cobra, "convert", "--data", csv, "--out", hbin, "--delim", ",", "--factor", "name"],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        raise RuntimeError(f"fixture convert failed: {r.stderr.strip()[:300]}")
    return hbin


def storm_do(fixture_hbin: str, cycles: int) -> str:
    """The storm .do source: every BUG-124-adjacent lifecycle per cycle.

    Each cycle exercises: resident/virtual numeric gen + replace, a string gen
    (strmutate stateless convert -- the crash site), preserve/restore (stash
    plane + clone fallback), egen, a keep/drop structure verb (resident plane,
    Stage 2), and a tempfile save/use round-trip (save_virtual plane).
    """
    lines = [
        "set more off",
        f'use "{fixture_hbin}"',
    ]
    for c in range(cycles):
        lines += [
            f"gen zn_{c} = x * {c + 1} + y",
            f'gen str8 zs_{c} = "c{c}" + name',
            f"replace zn_{c} = zn_{c} + 1 if grp == {c % 7}",
            f"egen zm_{c} = mean(zn_{c}), by(grp)",
            "preserve",
            f"drop if grp == {c % 7}",
            f"gen zdrop_{c} = zn_{c} * 2",
            "restore",
            "tempfile snap",
            'save "`snap\'"',
            f"drop zs_{c}",
            'use "`snap\'", clear',
            "count",
            # BUG-125 coverage: a DISK reader (append using) of a tempfile that
            # save_virtual may hold only in worker RAM — bounded by the
            # preserve/restore bracket (self-append doubles rows transiently).
            "preserve",
            "tempfile ap",
            'save "`ap\'"',
            'append using "`ap\'"',
            "count",
            "restore",
        ]
    lines += ["summarize", "count"]
    return "\n".join(lines) + "\n"


VOLATILE_RE = re.compile(
    r'(?<=")/[^"]+(?=")'        # any double-quoted absolute path (workdir/tempfile/resolved_path)
    # absolute scratch paths EMBEDDED in message text ("file /…/snap.tmp saved"):
    # eat to the closing quote — both runs' text ends there, so masking stays
    # deterministic even though trailing words are swallowed with the path.
    r'|(?:/private)?(?:/var/folders|/tmp|/Volumes)/[^"]*'
    r"|piet_native_[A-Za-z0-9]+"
    r"|_1[0-9]{12}_[0-9]+"      # nextPath time*1000 + seq scratch suffixes
    r"|tmp_1[0-9]{12}"          # runTempfile's minted per-run tempdir name
)


def masked(transcript: str) -> str:
    return VOLATILE_RE.sub("<VOLATILE>", transcript)


def run_do(piet: str, cobra: str, do_src: str, workdir: str, env_extra=None):
    """Run piet_native --do on do_src; return (exit, signal_death, stdout, stderr)."""
    do_path = os.path.join(workdir, "soak.do")
    with open(do_path, "w") as f:
        f.write(do_src)
    env = dict(os.environ)
    env["COBRA_BIN"] = cobra
    _pin_gate_settings(env)     # BUG-388 — before env_extra, so a leg's PIET_WORKDIR/TMPDIR override still wins
    env.update(env_extra or {})
    r = subprocess.run(
        [piet, "--do", do_path],
        capture_output=True,
        text=True,
        env=env,
        cwd=workdir,
    )
    signal_death = r.returncode < 0  # subprocess reports -SIGNUM on signal death
    return r.returncode, signal_death, r.stdout, r.stderr


def transcript_orphans(stdout: str):
    """FIX:INSTRUMENT-STORM-BOUND (2026-09-02): the exit census's ORPHAN count, or None when
    the transcript carries no census. Runs are launched with PIET_SCRATCH_CENSUS=1, so a
    missing census is itself a finding (the env plumbing broke), reported by the caller."""
    try:
        doc = json.loads(stdout)
    except (json.JSONDecodeError, ValueError):
        return None
    census = doc.get("scratch_census")
    if not isinstance(census, dict):
        return None
    return int(census.get("by_class", {}).get("ORPHAN", {}).get("n", 0))


def strip_census(stdout: str) -> str:
    """The byte-stability pair compares transcripts; the census carries the run's own
    workdir and byte totals, so it is removed before masking."""
    try:
        doc = json.loads(stdout)
    except (json.JSONDecodeError, ValueError):
        return stdout
    if isinstance(doc, dict) and "scratch_census" in doc:
        doc = dict(doc)
        del doc["scratch_census"]
        return json.dumps(doc)
    return stdout


def transcript_error_blocks(stdout: str):
    try:
        doc = json.loads(stdout)
    except (json.JSONDecodeError, ValueError):
        return None  # no parseable transcript
    return [b for b in doc.get("blocks", []) if b.get("type") == "error"]


def hbin_parses_clean(path: str) -> bool:
    """The reader-validation invariant: header column extents fit the file."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            if f.read(8) != b"CECONHB1":
                return False
            (hlen,) = struct.unpack("<Q", f.read(8))
            if 16 + hlen > size:
                return False
            h = json.loads(f.read(hlen))
        data_start = (16 + hlen + 63) // 64 * 64
        for c in h.get("columns", []):
            elem = 4 if c.get("dtype") == "factor" else 8
            if c["nbytes"] != h["n_rows"] * elem:
                return False
            if data_start + c["offset"] + c["nbytes"] > size:
                return False
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------------


def leg_storm(piet, cobra, cycles, keep):
    root = tempfile.mkdtemp(prefix="piet_soak_storm_")
    try:
        fixture = make_fixture_hbin(cobra, root)
        src = storm_do(fixture, cycles)
        results = []
        # two eager runs (byte-stability pair) + one default-budget run
        for tag, env in (
            ("eager-1", {"PIET_SCRATCH_BUDGET_MB": "0"}),
            ("eager-2", {"PIET_SCRATCH_BUDGET_MB": "0"}),
            ("default", {}),
        ):
            wd = os.path.join(root, tag)
            os.makedirs(wd, exist_ok=True)
            env = dict(env, PIET_SCRATCH_CENSUS="1")  # BUG-389 gate: ORPHAN must read 0 at exit
            code, sig, out, err = run_do(piet, cobra, src, wd, env)
            errs = transcript_error_blocks(out)
            results.append((tag, code, sig, out, errs))
            if sig:
                return False, f"storm[{tag}]: SIGNAL DEATH (exit {code}) -- BUG-124 class"
            if code != 0:
                first = (errs or [{}])[0].get("message", "")[:200] if errs else err[-300:]
                return False, f"storm[{tag}]: exit {code}; first error: {first}"
            if errs:
                return False, f"storm[{tag}]: {len(errs)} error block(s); first: {errs[0].get('message','')[:200]}"
            orphans = transcript_orphans(out)
            if orphans is None:
                return False, f"storm[{tag}]: no scratch census in the transcript (PIET_SCRATCH_CENSUS plumbing broken)"
            if orphans != 0:
                return False, f"storm[{tag}]: exit census ORPHAN={orphans} -- a minted file no ledger owns survived to exit (BUG-389 class)"
        if masked(strip_census(results[0][3])) != masked(strip_census(results[1][3])):
            return False, "storm: eager-1 vs eager-2 transcripts DIFFER after masking (byte-stability broken)"
        return True, f"storm: 3 runs x {cycles} cycles clean; eager pair byte-stable; exit census ORPHAN 0 on all three"
    finally:
        if not keep:
            shutil.rmtree(root, ignore_errors=True)


def leg_enospc(piet, cobra, cycles, keep):
    if shutil.which("hdiutil") is None:
        return None, "enospc: SKIP (hdiutil unavailable on this platform)"
    img = tempfile.mktemp(suffix=".sparseimage", prefix="piet_soak_disk_")
    mount = None
    try:
        r = subprocess.run(
            ["hdiutil", "create", "-megabytes", "192", "-fs", "APFS",
             "-type", "SPARSE", "-volname", "PIETSOAK", img],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            return None, f"enospc: SKIP (hdiutil create failed: {r.stderr.strip()[:120]})"
        r = subprocess.run(["hdiutil", "attach", img, "-nobrowse"], capture_output=True, text=True)
        if r.returncode != 0:
            return None, f"enospc: SKIP (hdiutil attach failed: {r.stderr.strip()[:120]})"
        mount = next((tok for tok in r.stdout.split() if tok.startswith("/Volumes/")), None)
        if mount is None:
            return None, "enospc: SKIP (mount point not found in hdiutil output)"

        wd = os.path.join(mount, "work")
        os.makedirs(wd, exist_ok=True)
        # Fixture big enough that the storm's scratch churn (~fixture-size per
        # mutation) exhausts the 192MB volume partway through the cycles.
        fixture = make_fixture_hbin(cobra, wd, rows=120_000)
        src = storm_do(fixture, cycles)
        code, sig, out, err = run_do(
            piet, cobra, src, wd,
            {"PIET_WORKDIR": wd, "TMPDIR": wd},
        )
        if sig:
            return False, f"enospc: SIGNAL DEATH (exit {code}) -- BUG-124 crash class reproduced"
        # Honesty contract: the failure must SURFACE — an error block in the
        # transcript, or the stderr FATAL line when the transcript itself could
        # not be written. (The message wording varies by which write fails
        # first: "disk full or I/O error", "could not materialize", data_io
        # "cannot open/write" — all are truthful; silence or a crash is not.)
        errs = transcript_error_blocks(out)
        honest = bool(errs)
        if "transcript write to stdout failed" in err or "disk full" in err.lower():
            honest = True  # transcript itself unwritable; the stderr FATAL line is the honest signal
        if code == 0 and not errs:
            # The storm fit inside the volume -- undersized churn, not a pass
            # on the property under test.
            return False, "enospc: run completed clean (volume never filled) -- raise cycles/rows"
        if not honest:
            return False, f"enospc: failed (exit {code}) WITHOUT an honest disk-full/data_io error"
        torn = []
        for dirpath, _dirs, files in os.walk(mount):
            for fn in files:
                if fn.endswith(".hbin"):
                    p = os.path.join(dirpath, fn)
                    if not hbin_parses_clean(p):
                        torn.append(p)
        if torn:
            return False, f"enospc: {len(torn)} torn .hbin file(s) survived (first: {torn[0]})"
        return True, f"enospc: honest failure (exit {code}), no signal death, no torn hbin survived"
    finally:
        if mount:
            subprocess.run(["hdiutil", "detach", mount, "-force"], capture_output=True)
        if os.path.exists(img) and not keep:
            os.remove(img)


def leg_torn(piet, cobra, keep):
    root = tempfile.mkdtemp(prefix="piet_soak_torn_")
    try:
        fixture = make_fixture_hbin(cobra, root, rows=50_000)
        torn = os.path.join(root, "torn.hbin")
        size = os.path.getsize(fixture)
        with open(fixture, "rb") as fin, open(torn, "wb") as fout:
            fout.write(fin.read(size // 6))  # the BUG-124 shape: full header, ~1/6 of the blobs
        src = f'use "{torn}"\ncount\n'
        code, sig, out, err = run_do(piet, cobra, src, root)
        if sig:
            return False, f"torn: SIGNAL DEATH (exit {code}) -- reader validation missing (BUG-124)"
        errs = transcript_error_blocks(out)
        if errs is None:
            return False, f"torn: no parseable transcript (exit {code}; stderr: {err[-200:]})"
        if not errs:
            return False, "torn: truncated hbin was accepted WITHOUT an error block"
        msg = errs[0].get("message", "")
        return True, f"torn: honest error, no crash ({msg[:120]})"
    finally:
        if not keep:
            shutil.rmtree(root, ignore_errors=True)


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description="BUG-124 resident-session soak gate")
    ap.add_argument("--piet", required=True, help="path to piet_native")
    ap.add_argument("--cobra", required=True, help="path to cobra")
    ap.add_argument("--cycles", type=int, default=12)
    ap.add_argument("--leg", choices=["all", "storm", "enospc", "torn"], default="all")
    ap.add_argument("--keep", action="store_true", help="keep scratch dirs for forensics")
    args = ap.parse_args()

    legs = []
    if args.leg in ("all", "storm"):
        legs.append(("storm", lambda: leg_storm(args.piet, args.cobra, args.cycles, args.keep)))
    if args.leg in ("all", "enospc"):
        legs.append(("enospc", lambda: leg_enospc(args.piet, args.cobra, args.cycles, args.keep)))
    if args.leg in ("all", "torn"):
        legs.append(("torn", lambda: leg_torn(args.piet, args.cobra, args.keep)))

    failed = 0
    for name, fn in legs:
        ok, note = fn()
        status = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
        print(f"[{status}] {note}")
        if ok is False:
            failed += 1
    print(f"piet_soak: {len(legs)} leg(s), {failed} FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
