#!/usr/bin/env python3
"""CoBrA evaluation harness.

Runs every cobra command (from manifest.json) on PUBLIC data, judges each against a Stata
golden (correctness — a hard gate), runs property/metamorphic + smoke checks, optionally times
cobra vs pyfixest/Stata (--bench), and writes report.json + report.md.

Correctness sets the exit code (0 iff every gating check passes). Speed is reported only.
Doctrine: ~/.claude/skills/cobra-eval/SKILL.md.

Design — uniform check addressing:
  * A check's `q` is a JSON path into cobra's (--format tidy) output:
        "kp_wald_F"                 -> result["kp_wald_F"]
        "coeftable[educ].b"         -> coeftable element with name=="educ", field b
        "events[0].att"             -> list index
  * The golden CSV stores the Stata truth under the SAME key string:
        coeftable[educ].b,9.030943361204917e+00
  So got = jget(result, q); want = golden[q]; compare at the check's rtol/atol.
"""
import argparse
import json
import math
import re
import subprocess
import sys
import time
import os
from pathlib import Path

EVAL = Path(__file__).resolve().parent          # tests/eval/ wherever this file lives
# Repo root is two levels up (tests/eval -> tests -> root). Env override for odd layouts.
ROOT = Path(os.environ.get("COBRA_ROOT", EVAL.parent.parent))
GOLDEN_DIR = EVAL / "golden"


def _resolve_data_dir():
    """Locate the eval data fixtures, portable across worktrees (PC-024).

    The fixtures physically live in `tests/validate/` (present on this Mac;
    gitignored — datasets are never committed). Inside op-eval that dir is exposed
    as `tests/eval/data` (a relative symlink); on a bare `main` checkout the symlink
    may be absent, so fall back to `../validate`, then to an explicit env override.
    Pick the first candidate that actually holds the fixtures (has `auto.hbin`).
    """
    cands = []
    env = os.environ.get("COBRA_EVAL_DATA")
    if env:
        cands.append(Path(env))
    cands += [EVAL / "data", EVAL.parent / "validate", ROOT / "tests" / "validate"]
    for c in cands:
        try:
            if (c / "auto.hbin").exists():
                return c
        except OSError:
            continue
    return EVAL / "data"          # last resort (lets a clear error surface downstream)


DATA_DIR = _resolve_data_dir()
COBRA = Path(os.environ.get("COBRA_BIN", ROOT / "build" / "cobra"))  # overridden by --cobra

HOT = {"ols", "feols", "iv", "fepois", "csdid", "wildboot", "xtreg"}  # speed-benchmarked


# ───────────────────────── manifest / data ─────────────────────────
def load_manifest(path):
    obj = json.loads(Path(path).read_text())
    cases = obj["cases"] if isinstance(obj, dict) else obj
    return cases


PLACEHOLDER = re.compile(r"\{([A-Za-z0-9_:./\-]+)\}")


def resolve_args(args, data_dir, overrides=None):
    """Replace {name} tokens with data_dir/name.hbin (or overrides[name] if given).

    `overrides` maps a placeholder name to an explicit path — used by a case's
    `prep` step to point at a runtime-materialized fixture instead of a file in
    the shared data dir.

    A `{repo:relpath}` token resolves to a repo-committed file under tests/eval/
    (e.g. `{repo:golden/f16_tw_fixture.dta}`) — the portable way to hand a
    command a committed NON-hbin fixture (a binary `.dta`, a raw `.csv`) by its
    real extension, which the plain `{name}`→`<data_dir>/name.hbin` rule cannot.
    """
    overrides = overrides or {}

    def sub(m):
        name = m.group(1)
        if name.startswith("repo:"):
            return str(EVAL / name[len("repo:"):])
        if name in overrides:
            return str(overrides[name])
        return str(Path(data_dir) / (name + ".hbin"))

    out = []
    for a in args:
        out.append(PLACEHOLDER.sub(sub, a))
    return out


def run_prep(prep, data_dir):
    """Materialize a derived fixture and return {name: path}.

    Two forms, selected by `prep.get("kind", "gen")`:
      * "gen" (default/original form) — `cobra eval --gen ...` derived columns:
            "prep": {"source": "csdid_input", "as": "nbreg_count",
                     "gen": ["treated=(gvar>0)*(wv>=gvar)", "y_int=round(visits_w)"]}
      * "recode" (COUNCIL-20260710 E4/BUG-059 goldens) — `cobra recode --var --rules`,
        with an optional `gen` (new-var name, a single string here — NOT a list like
        the "gen" kind's `gen`) for the copy-out form; omitting it recodes in place:
            "prep": {"kind": "recode", "source": "e4_recode", "as": "e4_recode_gen_out",
                     "var": "x", "rules": "(1=10)", "gen": "xr"}
      * "collapse" (Step-1 D3 fweight golden, 2026-07-11) — `cobra collapse --stat --by
        [--fweight]`; collapse always requires --out and never prints JSON on stdout
        (data-transform command, like tsops), so its own numeric output can only be
        gated by materializing the collapsed rows here and reading them back with a
        follow-on `eval --summarize` (see manifest case collapse_fweight_auto):
            "prep": {"kind": "collapse", "source": "auto", "as": "fweight_collapse_auto_out",
                     "stat": "(mean) mp=price (sum) sp=price (count) cp=price (sd) sdp=price",
                     "by": "foreign", "fweight": "mpg"}
    All forms keep the eval reproducible and self-contained without committing a
    large binary .hbin — the fixture is rebuilt on every run from data already in-tree.
    """
    kind = prep.get("kind", "gen")
    # The "convert" kind reads a committed CSV under tests/eval/ (NOT a data-dir
    # .hbin) so a bespoke `prep["source"]` .hbin lookup would be wrong for it.
    src = (str(EVAL / prep["csv"]) if kind == "convert"
           else str(Path(data_dir) / (prep["source"] + ".hbin")))
    out_path = Path("/tmp") / f"eval_prep_{prep['as']}.hbin"
    if kind == "convert":
        # Materialize the fixture through the engine's own CSV reader
        # (Frame::from_csv) at grade time — the ONLY way a from_csv bug (e.g.
        # a UTF-8 BOM left on the first header, C1-F20c) manifests live and
        # auto-flips green once the reader is fixed; a pre-frozen .hbin would
        # bake the bug in permanently. Source CSV is committed under tests/eval/.
        cmd = [str(_COBRA_BIN), "convert", src, str(out_path)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if p.returncode != 0:
            raise RuntimeError(f"prep(convert) failed for {prep['as']}: {p.stderr.strip()[:200]}")
        return {prep["as"]: out_path}
    if kind == "recode":
        cmd = [str(_COBRA_BIN), "recode", "--data", src, "--var", prep["var"],
               "--rules", prep["rules"]]
        if prep.get("gen"):
            cmd += ["--gen", prep["gen"]]
        cmd += ["--out", str(out_path)]
    elif kind == "gen":
        cmd = [str(_COBRA_BIN), "eval", "--data", src]
        for g in prep.get("gen", []):
            cmd += ["--gen", g]
        cmd += ["--out", str(out_path)]
    elif kind == "collapse":
        cmd = [str(_COBRA_BIN), "collapse", "--data", src,
               "--stat", prep["stat"], "--by", prep["by"]]
        if prep.get("fweight"):
            cmd += ["--fweight", prep["fweight"]]
        cmd += ["--out", str(out_path)]
    else:
        raise ValueError(f"unknown prep.kind {kind!r} for {prep.get('as')}")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if p.returncode != 0:
        raise RuntimeError(f"prep failed for {prep['as']}: {p.stderr.strip()[:200]}")
    return {prep["as"]: out_path}


# ───────────────────────── cobra ─────────────────────────
_COBRA_BIN = COBRA  # set to Path by main() from --cobra arg


def run_cobra(command, args, data_dir, tidy=True, timeout=180, overrides=None):
    cmd = [str(_COBRA_BIN), command] + resolve_args(args, data_dir, overrides)
    if tidy:
        cmd += ["--format", "tidy"]
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    parsed = None
    if p.stdout.strip().startswith(("{", "[")):
        try:
            parsed = json.loads(p.stdout)
        except json.JSONDecodeError:
            parsed = None
    return {"cmd": cmd, "rc": p.returncode, "out": p.stdout, "err": p.stderr,
            "json": parsed, "wall_ms": wall_ms}


# ───────────────────────── golden ─────────────────────────
def read_golden(path):
    """Read a golden CSV into a flat {key: float}.

    Native format (new eval goldens): two columns `key,value`, value at %21.15e.
    Tolerates a header row (a row whose 2nd field is not a float) and the legacy
    `name,b,se` / `name,att,se` triples (expanded to b_<name>/se_<name>/att_<name>).
    """
    out = {}
    rows = [ln for ln in Path(path).read_text().splitlines() if ln.strip()]
    for i, ln in enumerate(rows):
        # Use rsplit so keys that contain commas (e.g. cell[g2,t2].att) parse correctly.
        parts = ln.rsplit(",", 1)
        if len(parts) == 2:
            key, val_str = parts[0].strip(), parts[1].strip()
            try:
                out[key] = float(val_str)
                continue
            except ValueError:
                if i == 0:
                    continue  # header row
        # Fallback: 3-column legacy name,b,se (key has no internal commas)
        parts3 = [c.strip() for c in ln.split(",")]
        if len(parts3) == 3:
            name, a, b = parts3
            try:
                fa, fb = float(a), float(b)
            except ValueError:
                continue
            out[f"b_{name}"] = fa
            out[f"se_{name}"] = fb
            out[f"att_{name}"] = fa  # csdid name,att,se reuse
    return out


# ───────────────────────── JSON path resolver ─────────────────────────
_TOK = re.compile(r"[^.\[\]]+|\[[^\]]*\]")


def jget(obj, q):
    """Resolve a path like coeftable[educ].se / events[0].att / kp_wald_F. Raises KeyError."""
    cur = obj
    for tok in _TOK.findall(q):
        if tok.startswith("["):
            key = tok[1:-1]
            if isinstance(cur, list):
                if re.fullmatch(r"-?\d+", key):
                    cur = cur[int(key)]
                else:  # match by 'name' field (coeftable, equations, outcomes…)
                    hit = next((e for e in cur if isinstance(e, dict) and e.get("name") == key), None)
                    if hit is None:
                        raise KeyError(q)
                    cur = hit
            elif isinstance(cur, dict):
                cur = cur[key]
            else:
                raise KeyError(q)
        else:
            if not isinstance(cur, dict) or tok not in cur:
                raise KeyError(q)
            cur = cur[tok]
    return cur


# ───────────────────────── numeric compare ─────────────────────────
def relerr(got, want):
    return abs(got - want) / (abs(want) + 1e-30)


def cmp_check(got, want, rtol, atol):
    if got is None or want is None:
        return False, float("inf")
    if not (math.isfinite(got) and math.isfinite(want)):
        return (got == want), (0.0 if got == want else float("inf"))
    re_ = relerr(got, want)
    ok = re_ <= (rtol or 0.0) or abs(got - want) <= (atol or 0.0)
    return ok, re_


# ───────────────────────── check runners ─────────────────────────
def golden_checks(case, result, golden):
    res = []
    for chk in case.get("checks", []):
        q    = chk["q"]       # JSON path into cobra output
        gkey = chk.get("gkey", q)  # golden CSV lookup key (defaults to q)
        rtol = chk.get("rtol", 0.0)
        atol = chk.get("atol", 0.0)
        try:
            got = jget(result, q)
            got = float(got)
        except (KeyError, TypeError, ValueError):
            res.append({"q": q, "ok": False, "relerr": None, "got": None,
                        "want": golden.get(gkey), "note": "cobra field missing"})
            continue
        if gkey not in golden:
            res.append({"q": q, "ok": False, "relerr": None, "got": got,
                        "want": None, "note": "golden key missing"})
            continue
        want = golden[gkey]
        ok, re_ = cmp_check(got, want, rtol, atol)
        res.append({"q": q, "ok": ok, "relerr": re_, "got": got, "want": want,
                    "rtol": rtol, "atol": atol})
    return res


def smoke_checks(case, run):
    """Schema/sanity checks gated for every case: ran, parsed, sane flags."""
    res = []

    def add(name, ok, note=""):
        res.append({"q": name, "ok": bool(ok), "note": note})

    add("exit_zero", run["rc"] == 0, run["err"][:200])
    j = run["json"]
    if case["command"] == "info":
        add("prints_shape", "rows=" in run["out"] and "cols=" in run["out"])
        return res
    # Data-transform commands (e.g. tsops) write an --out file and print a plaintext
    # "wrote <path> (N rows)" confirmation to stderr — they never emit JSON, so the
    # meaningful smoke assertions are: the out file was actually written, and stderr
    # confirms with a row count (proves real work, not a no-op). Declared per-case via
    # `smoke_spec: {expect_json: false, out_exists, stderr_regex}`.
    spec = case.get("smoke_spec")
    if spec and spec.get("expect_json") is False:
        rx = spec.get("stderr_regex")
        if rx:
            add("stderr_confirms", re.search(rx, run["err"]) is not None,
                f"stderr: {run['err'][:120]}")
        if spec.get("out_exists"):
            cmd = run["cmd"]
            outp = cmd[cmd.index("--out") + 1] if "--out" in cmd else None
            add("out_file_written", bool(outp) and Path(outp).exists(), f"out={outp}")
        return res
    add("valid_json", j is not None)
    if not isinstance(j, dict):
        return res
    # sane coefficient table
    ct = j.get("coeftable")
    # Collinearity-omitted coefficients are legitimately zeroed (b=se=0, p=1,
    # ci=[0,0]) — that IS the omission marker (apps/cecon_cli.cpp ~381-394),
    # not a bug. Exempt them from the se>0 assumption (COUNCIL-20260710 G13c:
    # this fired as a false positive on i.foreign##i.rep78's auto-detected
    # collinear term, which — unlike the explicit `o.` operator — leaves a
    # real coeftable row behind). p_in_unit still holds (p=1.0 is in-range).
    omitted = set(j.get("omitted") or [])
    if isinstance(ct, list):
        for e in ct:
            se = e.get("se")
            if isinstance(se, (int, float)) and math.isfinite(se) and e.get("name") not in omitted:
                add(f"se>0[{e.get('name')}]", se > 0)
            p = e.get("p")
            if isinstance(p, (int, float)) and math.isfinite(p):
                add(f"p_in_unit[{e.get('name')}]", 0.0 <= p <= 1.0)
    if "converged" in j:
        add("converged", j["converged"] is True, "MLE did not converge")
    for k in ("demean_converged",):
        d = j.get("diagnostics", {})
        if isinstance(d, dict) and k in d:
            add(k, d[k] is True)
    return res


def reject_checks(case, run):
    """Checks for `oracle_kind: "reject"` cases: cobra MUST refuse this input.

    Inverse of the normal smoke assumption (exit_zero) — these cases assert
    rc != 0 plus (optionally) the specific structured error cobra emits. Errors
    are JSON-formatted but printed to STDERR (not stdout, where golden/smoke
    cases expect their output), so this reads `run["err"]`, not `run["json"]`.
    Case declares an `expect_error` block, e.g.:
        "expect_error": {"code": "bad_input", "message_contains": "insufficient clusters"}
    Both keys are optional; only rc!=0 is checked if `expect_error` is omitted.
    """
    res = []
    exp = case.get("expect_error", {})
    res.append({"q": "rc_nonzero", "ok": run["rc"] != 0, "note": f"rc={run['rc']}"})
    err_obj = None
    stderr_txt = (run.get("err") or "").strip()
    if stderr_txt.startswith("{"):
        try:
            err_obj = json.loads(stderr_txt).get("error")
        except json.JSONDecodeError:
            err_obj = None
    if "code" in exp:
        got_code = (err_obj or {}).get("code")
        res.append({"q": "error.code", "ok": got_code == exp["code"],
                    "note": f"got={got_code!r} want={exp['code']!r}"})
    if "message_contains" in exp:
        got_msg = (err_obj or {}).get("message", "")
        res.append({"q": "error.message_contains",
                    "ok": exp["message_contains"] in got_msg,
                    "note": f"want substring {exp['message_contains']!r} in {got_msg!r}"})
    return res


# ───────────────────────── one case ─────────────────────────
def eval_case(case, data_dir, props):
    command = case["command"]
    overrides = run_prep(case["prep"], data_dir) if case.get("prep") else None
    run = run_cobra(command, case.get("args", []), data_dir, overrides=overrides)

    # reject/qualifier cases: cobra is REQUIRED to error — skip the normal
    # smoke/golden path entirely (its exit_zero assumption would be backwards here).
    if case.get("oracle_kind") == "reject":
        checks = reject_checks(case, run)
        passed = all(c["ok"] for c in checks)
        return {"id": case["id"], "command": command, "group": case.get("group", ""),
                "kind": "reject", "passed": passed, "checks": checks,
                "wall_ms": run["wall_ms"], "rc": run["rc"],
                "suspected_cause": "" if passed else "reject-case did not match expected error"}

    checks = list(smoke_checks(case, run))
    kind = "smoke"

    # golden
    if case.get("oracle_kind") == "golden" and run["json"] is not None:
        gpath = case.get("reuse_existing") or (GOLDEN_DIR / case["golden_csv"])
        gpath = Path(gpath)
        if gpath.exists():
            golden = read_golden(gpath)
            checks += [{**c, "tier": "golden"} for c in golden_checks(case, run["json"], golden)]
            kind = "golden"
        else:
            checks.append({"q": "golden_present", "ok": False, "tier": "golden",
                           "note": f"missing {gpath.name}"})
            kind = "golden"

    # properties
    from_props = []
    for pname in case.get("properties", []):
        fn = props.get(pname)
        if fn is None:
            from_props.append({"q": f"prop:{pname}", "ok": False, "note": "unknown property"})
            continue
        try:
            ok, detail = fn(Ctx(case, run, data_dir))
            from_props.append({"q": f"prop:{pname}", "ok": bool(ok), "note": detail, "tier": "property"})
        except Exception as ex:  # noqa: BLE001
            from_props.append({"q": f"prop:{pname}", "ok": False, "note": f"raised {ex}", "tier": "property"})
    checks += from_props
    if from_props and kind == "smoke":
        kind = "property"

    passed = all(c["ok"] for c in checks)
    return {"id": case["id"], "command": command, "group": case.get("group", ""),
            "kind": kind, "passed": passed, "checks": checks,
            "wall_ms": run["wall_ms"], "rc": run["rc"],
            "suspected_cause": diagnose(case, run, checks)}


def diagnose(case, run, checks):
    if run["rc"] != 0:
        # cobra emits structured errors as JSON on STDERR (see grade_reject);
        # a data_io code means a missing/unreadable fixture — infra, not engine
        # (BUG-093 was spawned by mislabeling exactly this as an engine crash).
        stderr_txt = (run.get("err") or "").strip()
        if stderr_txt.startswith("{"):
            try:
                if (json.loads(stderr_txt).get("error") or {}).get("code") == "data_io":
                    return "missing/unreadable fixture (data_io) — infra, not engine"
            except json.JSONDecodeError:
                pass
        return "engine error / crash (non-zero exit)"
    if run["json"] is None and case["command"] != "info":
        return "output did not parse as JSON (schema regression)"
    fails = [c for c in checks if not c["ok"]]
    if not fails:
        return ""
    if any(c.get("note") == "golden key missing" for c in fails):
        return "golden lacks this key — check the .do dump / manifest q"
    if any(c.get("note") == "cobra field missing" for c in fails):
        return "cobra output lacks this field — schema mismatch or wrong q"
    if any(c.get("tier") == "golden" for c in fails):
        worst = max((c.get("relerr") or 0) for c in fails if c.get("tier") == "golden")
        return f"numeric parity miss (max relerr {worst:.1e}) — engine bug or convention diff"
    return "property/smoke failure"


class Ctx:
    """Handed to property functions."""
    def __init__(self, case, run, data_dir):
        self.case, self.run, self.data_dir = case, run, data_dir
        self.result = run["json"]

    def run_cobra(self, command, args, **kw):
        return run_cobra(command, args, self.data_dir, **kw)

    def data(self, name):
        return str(Path(self.data_dir) / (name + ".hbin"))


# ───────────────────────── reports ─────────────────────────
def write_reports(results, out_dir, bench=None):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rj = {"n": len(results), "passed": sum(r["passed"] for r in results),
          "failed": sum(not r["passed"] for r in results), "cases": results}
    if bench:
        rj["bench"] = bench
    (out_dir / "report.json").write_text(json.dumps(rj, indent=2))

    lines = ["# CoBrA eval report", ""]
    lines.append(f"**{rj['passed']}/{rj['n']} passed** · {rj['failed']} failed")
    lines.append("")
    # coverage matrix
    lines += ["## Coverage", "", "| command | tier | result |", "|---|---|---|"]
    for r in sorted(results, key=lambda r: (r["command"], r["id"])):
        mark = "✅" if r["passed"] else "❌"
        lines.append(f"| {r['command']} `{r['id']}` | {r['kind']} | {mark} |")
    # failures
    fails = [r for r in results if not r["passed"]]
    if fails:
        lines += ["", "## Failures", ""]
        for r in fails:
            lines.append(f"### ❌ {r['command']} `{r['id']}` — {r['suspected_cause']}")
            for c in r["checks"]:
                if not c["ok"]:
                    g, w = c.get("got"), c.get("want")
                    extra = f" got={g:.8g} want={w:.8g} relerr={c.get('relerr'):.1e}" \
                        if isinstance(g, (int, float)) and isinstance(w, (int, float)) else ""
                    lines.append(f"- `{c['q']}`{extra} {c.get('note','')}")
            lines.append("")
    # speed
    if bench:
        lines += ["## Speed", "", "| command | cobra fit ms | cobra invoke ms | pyfixest ms | Stata ms | speedup | flag |",
                  "|---|---|---|---|---|---|---|"]
        for b in bench:
            lines.append("| {command} | {fit} | {invoke} | {pyfixest} | {stata} | {speedup} | {flag} |".format(
                **{k: b.get(k, "") for k in ("command", "fit", "invoke", "pyfixest", "stata", "speedup", "flag")}))
    (out_dir / "report.md").write_text("\n".join(lines) + "\n")


# ───────────────────────── main ─────────────────────────

# ---------------------------------------------------------------------------
# BUG-376: honest SKIPs for an absent DATA FIXTURE.
#
# The data fixtures are gitignored (.gitignore blankets *.hbin / *.csv / *.dta),
# so a stranger who clones this repo has none of them. Until now that produced a
# wall of FAILURES: every case whose dataset is missing errored downstream, and
# absence printed exactly like a defect. That is the same shape the clone
# rehearsal fixed for ctest (SKIP_RETURN_CODE 77) and it was still live here.
#
# The dangerous direction is the opposite one — a harness that skips itself into
# a false green — so this reports skips SEPARATELY from passes and never counts
# a skip as a pass. Locally, where every fixture exists, nothing skips and the
# numbers are unchanged.
def _produced_tokens(cases):
    """Dataset names a case PRODUCES via its prep step.

    These look exactly like input fixtures in the JSON — `{nbreg_count}` — but
    they do not exist on disk before the run and must never be read as a missing
    fixture. Measured on the live manifest: 23 of the 54 tokens have no file, and
    all 23 are prep outputs. A detector that skipped on them would skip 23 cases
    on a machine where nothing is missing, which is the false-green direction
    this whole change exists to avoid."""
    out = set()
    for c in cases:
        pr = c.get("prep")
        preps = pr if isinstance(pr, list) else ([pr] if isinstance(pr, dict) else [])
        for q in preps:
            if isinstance(q, dict) and q.get("as"):
                out.add(q["as"])
    return out


def _fixtures_required(case, produced):
    """Every INPUT dataset this case needs, including through its prep step.

    Two sources, and missing the second one is a mistake this made once: a case
    can name only its prep's OUTPUT (`{nbreg_count}`) while the prep itself reads
    an input fixture (`csdid_input`) that the case never mentions. Skipping on the
    token set alone let such a case through to a prep crash."""
    want = set()
    for tok in re.findall(r"\{([A-Za-z0-9_]+)\}", json.dumps(case)):
        if tok not in produced:
            want.add(tok)
    pr = case.get("prep")
    preps = pr if isinstance(pr, list) else ([pr] if isinstance(pr, dict) else [])
    for q in preps:
        if not isinstance(q, dict):
            continue
        src = q.get("source")
        if src and src not in produced:
            want.add(src)
        # A `convert` prep reads a CSV path rather than a dataset token.
        csv = q.get("csv")
        if csv:
            want.add("csv:" + csv)
    return want


def _fixture_present(token, data_dir):
    d = Path(data_dir)
    if token.startswith("csv:"):
        # A `convert` prep reads a COMMITTED csv under tests/eval/, not a data-dir
        # fixture (run_prep's own comment says so) — so it resolves against EVAL,
        # and it is present in any clone rather than being one of the gitignored
        # binary fixtures.
        return (EVAL / token[4:]).exists()
    for ext in (".hbin", ".dta", ".csv"):
        if (d / (token + ext)).exists():
            return True
    return False


def _missing_fixtures(case, data_dir, produced):
    return sorted(t for t in _fixtures_required(case, produced)
                  if not _fixture_present(t, data_dir))


def main():
    global _COBRA_BIN
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=str(EVAL / "manifest.json"))
    ap.add_argument("--data-dir", default=str(DATA_DIR))
    ap.add_argument("--out", default=str(EVAL))
    ap.add_argument("--only", default="", help="comma list of commands")
    ap.add_argument("--group", default="", help="comma list of groups")
    ap.add_argument("--cobra", default=str(COBRA), help="path to cobra binary (default: main build)")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--with-ctest", action="store_true")
    ap.add_argument("--bench", action="store_true")
    args = ap.parse_args()

    _COBRA_BIN = Path(args.cobra)
    if not _COBRA_BIN.exists():
        print(f"ERROR: cobra binary not found: {_COBRA_BIN}", file=sys.stderr)
        sys.exit(1)

    if args.rebuild:
        print("building engine…")
        subprocess.run(["cmake", "--build", str(ROOT / "build"), "-j4"], check=True)

    cases = load_manifest(args.manifest)
    if args.only:
        want = set(args.only.split(","))
        cases = [c for c in cases if c["command"] in want]
    if args.group:
        want = set(args.group.split(","))
        cases = [c for c in cases if c.get("group") in want]

    # BUG-362: an unimportable properties.py used to degrade SILENTLY — one
    # parenthetical line, then every property-bearing case scored an ordinary
    # "unknown property" failure, indistinguishable in the report from a real
    # content regression. Measured 2026-08-22: a brew bump moved
    # /opt/homebrew/bin/python3 from 3.13 to 3.14, numpy went with it, and the
    # gate reported 106/131 — a RED that named the wrong 25 things and never
    # said the word "environment". The sibling site in percmd_floor's
    # _corpus_factor_profile() already does this right ("unavailable (no
    # pandas) — blindness NOT established"): an environment fault must be
    # reported AS one, never laundered into content verdicts.
    props = {}
    declared_props = any(c.get("properties") for c in cases)
    try:
        import properties as P  # noqa
        props = P.PROPERTIES
    except Exception as ex:  # noqa: BLE001
        if declared_props:
            n_cases = sum(1 for c in cases if c.get("properties"))
            print(f"[run_eval] ENVIRONMENT FAULT — properties.py could not be imported: {ex}",
                  file=sys.stderr)
            print(f"[run_eval] {n_cases} selected case(s) declare property checks, so this run "
                  f"CANNOT adjudicate them. Refusing to report a verdict that would look like "
                  f"content failures.", file=sys.stderr)
            print(f"[run_eval] Fix the interpreter, then re-run: this harness needs numpy "
                  f"(try: /opt/homebrew/bin/python3.13, or any python3 with numpy importable).",
                  file=sys.stderr)
            sys.exit(2)
        print(f"(properties.py unavailable: {ex}; no selected case declares properties)")

    results = []
    skipped = []
    produced = _produced_tokens(cases)
    for c in cases:
        miss = _missing_fixtures(c, args.data_dir, produced)
        if miss:
            skipped.append({"id": c.get("id", "?"), "command": c.get("command", "?"),
                            "missing": miss})
            print(f"  SKIP {c.get('command','?'):10s} {c.get('id','?'):28s} "
                  f"(fixture absent: {', '.join(miss)})")
            continue
        r = eval_case(c, args.data_dir, props)
        results.append(r)
        mark = "ok  " if r["passed"] else "FAIL"
        print(f"  {mark} {r['command']:10s} {r['id']:28s} ({r['kind']})"
              + ("" if r["passed"] else f"  -> {r['suspected_cause']}"))

    bench = None
    if args.bench:
        try:
            import bench as B  # noqa
            bench = B.run_bench([c for c in cases if c["command"] in HOT], args.data_dir)
        except Exception as ex:  # noqa: BLE001
            print(f"(bench unavailable: {ex})")

    write_reports(results, args.out, bench)

    ctest_ok = True
    if args.with_ctest:
        print("running ctest (local exact-answer suite)…")
        cp = subprocess.run(["ctest", "--test-dir", str(ROOT / "build"), "-j4", "--output-on-failure"])
        ctest_ok = cp.returncode == 0

    npass = sum(r["passed"] for r in results)
    print(f"\n{npass}/{len(results)} correctness gates passed; report → {args.out}/report.md")
    if skipped:
        # Named, counted, and never folded into the pass total — a skip is
        # "not asked", not "answered correctly".
        byfix = {}
        for sk in skipped:
            for m in sk["missing"]:
                byfix.setdefault(m, []).append(sk["id"])
        print(f"{len(skipped)} case(s) SKIPPED — their data fixture is not present:")
        for fx in sorted(byfix):
            ids = byfix[fx]
            shown = ", ".join(ids[:4]) + (f", +{len(ids)-4} more" if len(ids) > 4 else "")
            print(f"    {fx:24s} {len(ids):3d} case(s): {shown}")
        print("  (fixtures are gitignored; this is absence, not failure — see BUG-376)")
    sys.exit(0 if (npass == len(results) and ctest_ok) else 1)


if __name__ == "__main__":
    sys.path.insert(0, str(EVAL))
    main()
