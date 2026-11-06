#!/usr/bin/env python3
"""Dual-run parity leg: `cobra serve` (persistent session) vs one-shot CLI.

The gate-(1) instrument for MAXPERF Stage 1 (`cobra serve`), commissioned by
MSG-MI-237 item 5 / MI-242 (`<private-coordination-tree>/messenger/eval-harness/inbox.md`) and
designed in `internal note GATE23-MAXPERF-design-20260717.md` SS4 (Stage 1)
+ SS5.2 (this leg). Golden-first law: this harness lands BEFORE the engine's
`serve` subcommand exists. Against an engine that lacks it, every case must
SKIP gracefully (exit 0) -- see "Serve availability" below.

Byte-equality guarantee under test (pinned by the maintainer, quoted verbatim from the
commission): "serve stdout/stderr/exit == one-shot `cobra <argv...>` for
identical argv." This tool runs every case's cobra invocation(s) TWICE -- once
as a fresh one-shot subprocess per invocation (exactly what run_eval.py's gate
already does), once as a request against ONE persistent `cobra serve` session
covering the whole corpus (that persistence, in corpus order, is the point --
it is what would surface request-to-request state leakage that a one-shot
comparison could never see) -- and compares stdout/stderr/exit code for exact
string/int equality.

Case sourcing (do NOT invent a parallel corpus -- see README "Case sourcing"
for the full rationale): this file imports `run_eval.py` as a module and
calls its OWN functions directly --
  * `load_manifest`      -- the exact 101-case corpus the gate runs.
  * `resolve_args`        -- the exact {placeholder}->path substitution rules.
  * `run_prep`             -- executed for real (one-shot) to materialize a
                              case's fixture; its internal `subprocess.run`
                              call is tapped (not reimplemented) to recover
                              the exact argv it ran, so the SAME argv can be
                              replayed through `cobra serve`.
  * `run_cobra`            -- executed for real (one-shot) for every case's
                              main invocation, so the CLI half of the
                              comparison is byte-identical to what the 101-
                              case gate itself produces (same --format tidy
                              suffix, same arg resolution).
No case-construction logic is reimplemented; the only "replication" is a
transparent subprocess.run tap (see `_one_shot_prep`), which observes rather
than reinvents run_eval's prep-kind branching (convert/recode/gen/collapse).

Usage:
    python3 run_serve_parity.py --cobra PATH [--limit N] [--shuffle SEED]
                                 [--only cmd1,cmd2] [--group g1,g2]
                                 [--allow-no-serve] [--json OUT.json]

Exit codes: 0 = no RED, and (serve was available OR --allow-no-serve was
passed) * 1 = at least one RED, OR serve is unavailable and
--allow-no-serve was NOT passed (the default -- serve-absence is a hard
RED unless explicitly waived).
"""
import argparse
import json
import random
import re
import select
import subprocess
import sys
import time
from pathlib import Path

SERVE_PARITY = Path(__file__).resolve().parent
EVAL = SERVE_PARITY.parent
sys.path.insert(0, str(EVAL))
import run_eval as RE  # noqa: E402  -- see module docstring "Case sourcing"


# ───────────────────────── one-shot half (reuses run_eval directly) ─────────────────────────
def _one_shot_prep(prep, data_dir):
    """Run a case's fixture-prep step via the REAL `run_eval.run_prep` (one-shot
    subprocess execution), tapping the `subprocess.run` call it makes internally
    so the exact argv can be recovered and replayed through `cobra serve` later.

    Returns (overrides, record) where `overrides` is run_prep's own return value
    ({placeholder_name: materialized_path}, fed straight into resolve_args for
    the case's main command) and `record` is {"cmd","rc","out","err"} for the
    prep invocation itself, in the same shape run_eval.run_cobra() returns.
    """
    captured = {}
    real_run = RE.subprocess.run

    def _tap(cmd, *a, **kw):
        p = real_run(cmd, *a, **kw)
        captured["cmd"] = list(cmd)
        captured["rc"] = p.returncode
        captured["out"] = p.stdout
        captured["err"] = p.stderr
        return p

    RE.subprocess.run = _tap
    try:
        overrides = RE.run_prep(prep, data_dir)
    finally:
        RE.subprocess.run = real_run
    if "cmd" not in captured:
        # Defensive: every known prep kind calls subprocess.run exactly once.
        # If a future prep kind doesn't, this harness needs updating (Art 10,
        # Eval's job) rather than silently comparing nothing.
        raise RuntimeError("run_prep did not invoke subprocess.run as expected "
                            "(harness assumption broken -- update _one_shot_prep)")
    return overrides, {"cmd": captured["cmd"], "rc": captured["rc"],
                        "out": captured["out"], "err": captured["err"]}


class OneShotRecord:
    __slots__ = ("case_id", "prep", "main", "skip_reason")

    def __init__(self, case_id, prep=None, main=None, skip_reason=None):
        self.case_id = case_id
        self.prep = prep          # {"cmd","rc","out","err"} or None
        self.main = main          # {"cmd","rc","out","err"} or None
        self.skip_reason = skip_reason


def run_one_shot_phase(cases, data_dir):
    """Execute every case's prep (if any) + main invocation as fresh one-shot
    subprocesses, exactly mirroring what run_eval.py's 101-case gate does.
    Returns {case_id: OneShotRecord}, in manifest order (order here is always
    the natural manifest order -- one-shot output cannot depend on invocation
    order since each call is an independent process, so --shuffle only
    reorders the SERVE side; see README).
    """
    out = {}
    for case in cases:
        cid = case["id"]
        prep_rec = None
        overrides = None
        try:
            if case.get("prep"):
                overrides, prep_rec = _one_shot_prep(case["prep"], data_dir)
        except Exception as ex:  # noqa: BLE001
            out[cid] = OneShotRecord(cid, skip_reason=f"prep failed in one-shot mode: {ex}")
            continue
        main = RE.run_cobra(case["command"], case.get("args", []), data_dir, overrides=overrides)
        main_rec = {"cmd": main["cmd"], "rc": main["rc"], "out": main["out"], "err": main["err"]}
        out[cid] = OneShotRecord(cid, prep=prep_rec, main=main_rec)
    return out


# ───────────────────────── serve half ─────────────────────────
class ServeUnavailable(Exception):
    pass


class ServeSession:
    """One persistent `cobra serve` subprocess for the whole corpus.

    Protocol (pinned by the maintainer): boots, prints one handshake line
    {"serve":1,"cobra_version":...,"contract_a":...}, then answers each
    request line {"id":N,"argv":[...]} with one response line
    {"id":N,"exit":rc,"stdout":s,"stderr":s}. EOF or {"argv":["shutdown"]}
    -> exit 0.
    """

    def __init__(self, cobra_bin, handshake_timeout=10.0):
        self.cobra_bin = str(cobra_bin)
        self._next_id = 0
        self.handshake = None
        try:
            self.proc = subprocess.Popen(
                [self.cobra_bin, "serve"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1,
            )
        except OSError as ex:
            raise ServeUnavailable(f"serve mode unavailable (could not spawn: {ex})") from ex

        line = self._readline(self.proc.stdout, handshake_timeout)
        if line is None:
            rc = self.proc.poll()
            stderr_txt = ""
            try:
                stderr_txt = self.proc.stderr.read(500) or ""
            except Exception:  # noqa: BLE001
                pass
            self._cleanup()
            if rc is None:
                raise ServeUnavailable(
                    f"serve mode unavailable (handshake timeout after {handshake_timeout:.1f}s, "
                    f"no line received, process still alive -- killed)")
            raise ServeUnavailable(
                f"serve mode unavailable (exit={rc}, no handshake"
                + (f" -- stderr: {stderr_txt.strip()[:200]}" if stderr_txt.strip() else "") + ")")
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            self._cleanup()
            raise ServeUnavailable(f"serve mode unavailable (handshake not valid JSON: {line[:200]!r})")
        if not isinstance(obj, dict) or obj.get("serve") != 1:
            self._cleanup()
            raise ServeUnavailable(f"serve mode unavailable (handshake missing serve:1 key: {line[:200]!r})")
        self.handshake = obj

    @staticmethod
    def _readline(stream, timeout):
        r, _, _ = select.select([stream], [], [], timeout)
        if not r:
            return None  # timeout, nothing readable
        line = stream.readline()
        if line == "":
            return None  # EOF
        return line.rstrip("\n")

    def request(self, argv, timeout=180.0):
        self._next_id += 1
        rid = self._next_id
        try:
            self.proc.stdin.write(json.dumps({"id": rid, "argv": argv}) + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as ex:
            raise RuntimeError(f"serve session pipe broke sending id={rid} argv={argv}: {ex}") from ex
        line = self._readline(self.proc.stdout, timeout)
        if line is None:
            rc = self.proc.poll()
            raise RuntimeError(f"serve session died/EOF answering id={rid} argv={argv} (exit={rc})")
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as ex:
            raise RuntimeError(f"serve response id={rid} not valid JSON: {line[:200]!r}") from ex
        if obj.get("id") != rid:
            raise RuntimeError(f"serve id mismatch: sent {rid}, got {obj.get('id')!r} ({line[:200]!r})")
        return {"cmd": [self.cobra_bin] + list(argv), "rc": obj.get("exit"),
                "out": obj.get("stdout", ""), "err": obj.get("stderr", "")}

    def shutdown(self, timeout=10.0):
        try:
            self.proc.stdin.close()
        except Exception:  # noqa: BLE001
            pass
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass
        # Fallback: explicit shutdown request form, per protocol.
        try:
            self.proc.stdin.write(json.dumps({"argv": ["shutdown"]}) + "\n")
            self.proc.stdin.flush()
        except Exception:  # noqa: BLE001
            pass
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._cleanup()
            return self.proc.returncode

    def _cleanup(self):
        try:
            self.proc.kill()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            pass


class ServeRecord:
    __slots__ = ("case_id", "prep", "main", "skip_reason")

    def __init__(self, case_id, prep=None, main=None, skip_reason=None):
        self.case_id = case_id
        self.prep = prep
        self.main = main
        self.skip_reason = skip_reason


def run_serve_phase(session, cases, one_shot, order):
    """Replay every case's recorded argv(s) through the ONE persistent serve
    session, in `order` (a possibly-shuffled permutation of `cases`, prep/main
    pairs always kept adjacent -- see README "Order-sensitivity guard").
    Cases whose one-shot half already SKIPped (prep failed) are not replayed
    (there is no known-good argv to replay -- resolve_args needs the prep's
    overrides, which don't exist).
    """
    out = {}
    by_id = {c["id"]: c for c in cases}
    for cid in order:
        rec = one_shot[cid]
        if rec.skip_reason is not None:
            out[cid] = ServeRecord(cid, skip_reason=rec.skip_reason)
            continue
        case = by_id[cid]
        prep_result = None
        if rec.prep is not None:
            prep_argv = rec.prep["cmd"][1:]
            prep_result = session.request(prep_argv)
        main_argv = rec.main["cmd"][1:]
        main_result = session.request(main_argv)
        out[cid] = ServeRecord(cid, prep=prep_result, main=main_result)
    return out


# ───────────────────────── comparison ─────────────────────────
# cobra's own JSON schema embeds real wall-clock timing that is non-
# deterministic BY CONSTRUCTION -- confirmed at the source (op-cobra,
# `git log` head at the time of writing): apps/cmds/common.cpp:811/914/
# 1048/1213/1298 emit `"timing_ms":{"load":...,"build":...,"fit":...}`;
# apps/cmds/cmds_linear.cpp:255 and cmds_panel.cpp:161/263/378/467/553 and
# common.cpp:1122 emit a top-level `"time_ms":<float>`. These are the ONLY
# two field names used for this anywhere in the engine (verified by grep).
# Two back-to-back invocations of the IDENTICAL one-shot binary on the
# IDENTICAL argv already differ in these fields -- so literal byte-equality
# would RED on ~every estimator case (most of the corpus) regardless of
# whether serve is behaviorally correct. Masked by default; --strict-bytes
# disables masking for a literal byte-for-byte run (expect mass RED against
# a real engine -- useful for manual inspection, not as a standing gate).
_TIME_MS_RE = re.compile(r'"time_ms"\s*:\s*[0-9eE+\-.]+')
_TIMING_MS_RE = re.compile(r'"timing_ms"\s*:\s*\{[^{}]*\}')


def mask_timing(s):
    if not s:
        return s
    s = _TIME_MS_RE.sub('"time_ms":"<masked>"', s)
    s = _TIMING_MS_RE.sub('"timing_ms":"<masked>"', s)
    return s


def first_diff_excerpt(a, b, context=32):
    """A bounded hexdump-style excerpt of the first byte where a != b."""
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    lo = max(0, i - 8)

    def seg(s):
        chunk = s[lo:i + context]
        raw = chunk.encode("utf-8", errors="replace")
        hexed = " ".join(f"{byte:02x}" for byte in raw)
        return f"{hexed}  {chunk!r}"

    return (f"first diff at char offset {i} (lengths {len(a)} vs {len(b)}):\n"
            f"      one-shot: {seg(a)}\n"
            f"      serve   : {seg(b)}")


def compare_stream(label, one, serve, diffs, mask=True):
    if one["rc"] != serve["rc"]:
        diffs.append(f"{label}:exit  one-shot={one['rc']!r} serve={serve['rc']!r}")
    for stream_key, stream_name in (("out", "stdout"), ("err", "stderr")):
        a, b = one.get(stream_key) or "", serve.get(stream_key) or ""
        a_cmp, b_cmp = (mask_timing(a), mask_timing(b)) if mask else (a, b)
        if a_cmp != b_cmp:
            diffs.append(f"{label}:{stream_name} diverged -- {first_diff_excerpt(a_cmp, b_cmp)}")


def grade_case(one_rec, serve_rec, mask=True):
    """Returns (verdict, diffs) where verdict in {"PASS","RED","SKIP"}."""
    if one_rec.skip_reason is not None:
        return "SKIP", [one_rec.skip_reason]
    if serve_rec is None or serve_rec.skip_reason is not None:
        reason = serve_rec.skip_reason if serve_rec else "serve mode unavailable"
        return "SKIP", [reason]
    diffs = []
    if one_rec.prep is not None:
        compare_stream("prep", one_rec.prep, serve_rec.prep, diffs, mask=mask)
    compare_stream("main", one_rec.main, serve_rec.main, diffs, mask=mask)
    return ("RED" if diffs else "PASS"), diffs


# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=str(RE.EVAL / "manifest.json"))
    ap.add_argument("--data-dir", default=str(RE.DATA_DIR))
    ap.add_argument("--cobra", default=str(RE.COBRA), help="path to cobra binary")
    ap.add_argument("--only", default="", help="comma list of commands (mirrors run_eval.py)")
    ap.add_argument("--group", default="", help="comma list of groups (mirrors run_eval.py)")
    ap.add_argument("--limit", type=int, default=0, help="cap the corpus to the first N cases (smoke)")
    ap.add_argument("--shuffle", type=int, default=None, metavar="SEED",
                     help="reorder the SERVE request stream by case (prep/main stay adjacent); "
                          "default off = natural manifest order")
    ap.add_argument("--allow-no-serve", action="store_true",
                     help="opt-in: exit 0 (graceful SKIP) when serve mode is unavailable, "
                          "instead of the default hard RED. Use only for a deliberate "
                          "golden-first landing window before `cobra serve` exists on the "
                          "target engine.")
    ap.add_argument("--strict-bytes", action="store_true",
                     help="disable time_ms/timing_ms masking -- literal byte comparison "
                          "(expect mass RED against a real engine: these fields are "
                          "non-deterministic wall-clock, not engine output -- see README "
                          "'Timing-field masking')")
    ap.add_argument("--handshake-timeout", type=float, default=10.0)
    ap.add_argument("--request-timeout", type=float, default=180.0)
    ap.add_argument("--json", default="", help="optional path to write a full JSON verdict dump")
    args = ap.parse_args()

    cobra_bin = Path(args.cobra)
    if not cobra_bin.exists():
        print(f"ERROR: cobra binary not found: {cobra_bin}", file=sys.stderr)
        sys.exit(2)
    RE._COBRA_BIN = cobra_bin  # module global run_eval's run_prep/run_cobra dispatch through

    cases = RE.load_manifest(args.manifest)
    if args.only:
        want = set(args.only.split(","))
        cases = [c for c in cases if c["command"] in want]
    if args.group:
        want = set(args.group.split(","))
        cases = [c for c in cases if c.get("group") in want]
    if args.limit:
        cases = cases[:args.limit]
    if not cases:
        print("ERROR: no cases selected (check --manifest/--only/--group/--limit)", file=sys.stderr)
        sys.exit(2)

    mask = not args.strict_bytes
    print(f"serve-parity: {len(cases)} case(s) from {args.manifest}")
    print(f"  one-shot/serve binary: {cobra_bin}")
    print(f"  data dir: {args.data_dir}")
    print("  comparison: time_ms/timing_ms masked before compare (default)" if mask
          else "  comparison: --strict-bytes -- literal comparison, mass RED expected on timed commands")

    t0 = time.perf_counter()
    one_shot = run_one_shot_phase(cases, args.data_dir)
    t_one = time.perf_counter() - t0
    n_prep_fail = sum(1 for r in one_shot.values() if r.skip_reason is not None)
    print(f"  one-shot phase: {len(cases)} case(s) in {t_one:.1f}s"
          + (f" ({n_prep_fail} prep failure(s) -- will SKIP)" if n_prep_fail else ""))

    order = [c["id"] for c in cases]
    if args.shuffle is not None:
        random.Random(args.shuffle).shuffle(order)
        print(f"  serve request order: SHUFFLED (seed={args.shuffle})")
    else:
        print("  serve request order: manifest (natural) order")

    serve_reason = None
    serve = {}
    session = None
    try:
        session = ServeSession(cobra_bin, handshake_timeout=args.handshake_timeout)
        hs = session.handshake
        print(f"  serve handshake: {hs}")
        t1 = time.perf_counter()
        serve = run_serve_phase(session, cases, one_shot, order)
        t_serve = time.perf_counter() - t1
        print(f"  serve phase: {len(order)} case(s) in {t_serve:.1f}s")
        rc = session.shutdown()
        if rc != 0:
            print(f"  NOTE: serve session exited rc={rc} on shutdown (expected 0)", file=sys.stderr)
    except ServeUnavailable as ex:
        serve_reason = str(ex)
        print(f"  {serve_reason}")
    except RuntimeError as ex:
        # Session died mid-run (not the "never existed" case) -- still a graceful
        # SKIP-everything outcome, just with a different one-line reason.
        serve_reason = f"serve mode unavailable (session failed mid-run: {ex})"
        print(f"  {serve_reason}")
        if session is not None:
            session._cleanup()

    results = []
    for cid in order:
        one_rec = one_shot[cid]
        serve_rec = serve.get(cid) if serve_reason is None else None
        if serve_reason is not None and one_rec.skip_reason is None:
            serve_rec = ServeRecord(cid, skip_reason=serve_reason)
        verdict, diffs = grade_case(one_rec, serve_rec, mask=mask)
        results.append({"id": cid, "verdict": verdict, "diffs": diffs})
        label = f"{verdict:4s}"
        print(f"  {label}  {cid}")
        for d in diffs:
            for ln in str(d).splitlines():
                print(f"        {ln}")

    n_pass = sum(1 for r in results if r["verdict"] == "PASS")
    n_red = sum(1 for r in results if r["verdict"] == "RED")
    n_skip = sum(1 for r in results if r["verdict"] == "SKIP")
    n_compared = n_pass + n_red
    print(f"\nserve-parity: {n_pass}/{n_compared} identical ({n_skip} skipped)")

    if args.json:
        out = {
            "n": len(results), "identical": n_pass, "red": n_red, "skipped": n_skip,
            "serve_available": serve_reason is None,
            "serve_reason": serve_reason,
            "shuffle_seed": args.shuffle,
            "timing_masked": mask,
            "cases": results,
        }
        Path(args.json).write_text(json.dumps(out, indent=2))
        print(f"  wrote {args.json}")

    if n_red > 0:
        sys.exit(1)
    if serve_reason is not None and not args.allow_no_serve:
        print("  serve unavailable -> exit 1 (default; pass --allow-no-serve for the old "
              "graceful-SKIP behavior)", file=sys.stderr)
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
