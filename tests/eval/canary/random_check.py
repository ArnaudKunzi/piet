#!/usr/bin/env python3
"""random-check — anti-mock / differential parity verification.

Motivation (2026-06-18): PIET shipped in mock mode — the UI served ONE fixed
fixture for every regression, and a fixed-input verification couldn't tell. The cure
is a check whose RESULT MUST DEPEND ON ITS INPUT. Each run randomizes
{dataset, variables, estimator, options}, runs the command through a chosen target
AND the trusted `cobra` CLI reference, and asserts the invariants no static
fixture/mock can satisfy:

  (a) PARITY      — subject result matches the cobra CLI reference within tolerance.
  (b) DIFFERENTIAL— two DIFFERENT commands give DIFFERENT results (the invariant a
                    mock cannot pass: a fixture is input-independent).
  (c) STRUCTURE   — output coef-names == requested vars (+ intercept); n_obs tracks
                    the dataset, not a constant.
  (d) CLOSED-FORM — a freshly-built noise-free y=10*x1-3*x2+7 must be recovered
                    EXACTLY (coef 10, -3, 7). The strongest deterministic canary.

Targets (--target):
  cli     cobra CLI is both subject and reference -> self-test of the canary truth
          + the invariant engine (parity trivially holds; (b)(c)(d) are real).
  kernel  drive PIET's REAL backend kernel path (piet.dispatcher.dispatch: the same
          translate->cobra->normalize chain the UI uses) and compare to the cobra
          CLI reference. Catches kernel translation/routing bugs. Requires the PIET
          backend importable (run with a python that has numpy/pandas + PYTHONPATH to
          the backend; see SKILL.md). NB: the backend has no mock, so this PASSES on
          a correct kernel — it verifies parity, not the frontend mock.
  app     POST randomized commands to a running kernel's HTTP /api/command and apply
          the same invariants. For the *frontend* anti-mock acceptance (FAIL on a
          mock-mode packaged app), the app must be driven through its UI — see
          SKILL.md "App-level acceptance"; the mock lives in the frontend, never the
          backend.

Exit 0 iff every invariant holds. A mock-mode subject fails (b),(c),(d).

MI-139 (2026-07-08): the `kernel` and `app` targets drove the PYTHON backend,
which is runtime-dead since 2.0.0 GA and deleted in 2.0.3 — they now fail with
an explicit backend-not-importable error and are RETIRED. The standing canary
is `--target cli` (backend-free). A `native` target driving `piet_native
--run-block` is the designated re-point if a kernel-level anti-mock leg is
wanted again (the native core is meanwhile gated by tests/eval/piet2/).

Truth + tolerances come from canary_set.json (Eval owns; piet-sanity reuses it).
"""
import argparse
import json
import math
import os
import random
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
EVAL = HERE.parent
ROOT = Path(os.environ.get("COBRA_ROOT", EVAL.parent.parent))  # repo root (tests/eval -> tests -> root)


def _resolve_data_dir():
    """Locate the canary source fixtures across worktrees.

    They physically live in `tests/validate/` (present on this Mac; gitignored).
    Inside op-eval that dir is symlinked as `tests/eval/data`; on a bare main
    checkout `tests/eval/data` is empty, so fall back to `tests/validate`. Honor a
    COBRA_EVAL_DATA override first; pick the first candidate that actually holds the
    fixtures (PC-024). No hardcoded user paths.
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
    return EVAL / "data"


DEFAULT_DATA = _resolve_data_dir()
DEFAULT_COBRA = Path(os.environ.get("COBRA_BIN", ROOT / "build" / "cobra"))
CANARY_SET = HERE / "canary_set.json"


# ───────────────────────── result model ─────────────────────────
class Result:
    """Normalised estimation result: {name: {b, se}} + estimator + n_obs."""
    def __init__(self, estimator, n_obs, coefs):
        self.estimator = estimator
        self.n_obs = n_obs
        self.coefs = coefs          # {name: {"b": float, "se": float|None}}

    @property
    def names(self):
        return set(self.coefs)

    def signature(self):
        # input-dependent fingerprint: sorted (name, rounded-b)
        return tuple(sorted((n, round(c["b"], 9)) for n, c in self.coefs.items()))


def _coefs_from_coeftable(ct):
    out = {}
    for e in ct or []:
        name = e.get("name")
        if name is None:
            continue
        out[name] = {"b": _f(e.get("b")), "se": _f(e.get("se"))}
    return out


def _f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


# ───────────────────────── subjects ─────────────────────────
def run_cli(cobra_bin, cli_cmd, y, xs, data_path, extra):
    argv = [str(cobra_bin), cli_cmd, "--data", str(data_path),
            "--y", y, "--x", ",".join(xs)] + list(extra) + ["--format", "tidy"]
    p = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError(f"cobra CLI rc={p.returncode}: {p.stderr.strip()[:200]}")
    j = json.loads(p.stdout)
    return Result(j.get("estimator", cli_cmd), j.get("n_obs"), _coefs_from_coeftable(j.get("coeftable")))


_KERNEL = {"dispatch": None, "Session": None}


def _load_kernel():
    if _KERNEL["dispatch"] is None:
        backend = os.environ.get("PIET_BACKEND",
                                 str(ROOT / "piet" / "backend"))
        if backend not in sys.path:
            sys.path.insert(0, backend)
        try:
            from piet.dispatcher import dispatch          # noqa
            from piet.session import SessionState         # noqa
        except Exception as ex:  # noqa: BLE001
            raise RuntimeError(
                f"PIET backend not importable ({ex}). Run with a python that has "
                f"numpy/pandas and PYTHONPATH={backend} (see SKILL.md).")
        _KERNEL["dispatch"], _KERNEL["Session"] = dispatch, SessionState
    return _KERNEL["dispatch"], _KERNEL["Session"]


def _block_to_result(blocks, fallback_cmd):
    est = next((b for b in blocks if b.get("type") == "estimation"), None)
    if est is None:
        kinds = [b.get("type") for b in blocks]
        err = next((b for b in blocks if b.get("type") == "error"), None)
        detail = f" error={err.get('message')}" if err else ""
        raise RuntimeError(f"no estimation block (got {kinds}){detail}")
    return Result(est.get("estimator", fallback_cmd), est.get("n_obs"),
                  _coefs_from_coeftable(est.get("coeftable")))


def run_kernel(stata_cmd, data_path):
    dispatch, SessionState = _load_kernel()
    sess = SessionState(Path("/tmp/cobra_canary_kernel"))
    sess.load(str(data_path))
    blocks = dispatch(stata_cmd, sess)
    return _block_to_result(blocks, stata_cmd)


# Faithful replay of PIET's frontend mock fixture (frontend/js/api.js
# MOCK_COMMAND_REGRESS + mockCommand): the SAME canned coeftable / n_obs for EVERY
# command, regardless of dataset or variables. Used by --target mock to prove the
# harness actually catches a mock (input-independent) subject — the no-gaming proof
# that the check can fail. This is the exact shape the 2026-06-18 mock-mode app served.
_MOCK_REGRESS_COEFTABLE = [
    {"name": "pm25", "b": -0.07513568, "se": 0.00229063},
    {"name": "t2m_max_C", "b": -0.11019545, "se": 0.00320676},
    {"name": "(intercept)", "b": 30.26699820, "se": 0.08782266},
]


def run_mock(stata_cmd):
    return Result("feols", 620279, _coefs_from_coeftable(_MOCK_REGRESS_COEFTABLE))


def run_app(stata_cmd, url):
    req = urllib.request.Request(
        url.rstrip("/") + "/api/command",
        data=json.dumps({"cmd": stata_cmd}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=300) as r:
        payload = json.loads(r.read().decode())
    return _block_to_result(payload.get("blocks", []), stata_cmd)


def subject_run(target, *, cobra_bin, cli_cmd, y, xs, data_path, extra, stata_cmd, url):
    if target == "cli":
        return run_cli(cobra_bin, cli_cmd, y, xs, data_path, extra)
    if target == "kernel":
        return run_kernel(stata_cmd, data_path)
    if target == "app":
        return run_app(stata_cmd, url)
    if target == "mock":
        return run_mock(stata_cmd)
    raise ValueError(target)


# ───────────────────────── checks ─────────────────────────
def relerr(a, b):
    return abs(a - b) / (abs(b) + 1e-30)


class Report:
    def __init__(self):
        self.checks = []

    def add(self, name, ok, note=""):
        self.checks.append((name, bool(ok), note))
        return ok

    def ok(self):
        return all(c[1] for c in self.checks)

    def dump(self):
        for name, ok, note in self.checks:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {note}" if note else ""))
        n_pass = sum(c[1] for c in self.checks)
        print(f"\n{n_pass}/{len(self.checks)} invariants held")


def build_stata(estimator, y, xs):
    cmd = estimator["stata"].replace("{y}", y).replace("{x}", " ".join(xs))
    return cmd


def check_closed_form(cs, target, cobra_bin, cache, url, rpt):
    tol = cs["tolerances"]["closed_form"]
    for cf in cs.get("closed_form", []):
        data_path = Path(cache) / cf["hbin"]
        y = cf["cli"][cf["cli"].index("--y") + 1]
        xs = cf["cli"][cf["cli"].index("--x") + 1].split(",")
        extra = cf["cli"][cf["cli"].index("--vcov"):] if "--vcov" in cf["cli"] else []
        res = subject_run(target, cobra_bin=cobra_bin, cli_cmd=cf["cli"][0], y=y, xs=xs,
                          data_path=data_path, extra=extra,
                          stata_cmd=cf["stata"], url=url)
        for name, want in cf["expect"].items():
            got = res.coefs.get(name, {}).get("b")
            ok = got is not None and abs(got - want) <= tol["b"]
            rpt.add(f"closed_form[{cf['id']}].{name}=={want}", ok,
                    f"got={got}")
        rpt.add(f"closed_form[{cf['id']}].n_obs=={cf['n_rows']}",
                res.n_obs == cf["n_rows"], f"got={res.n_obs}")


def check_draws(cs, target, cobra_bin, data_dir, url, rng, n_draws, rpt):
    datasets = cs["datasets"]
    estimators = cs["estimators"]
    families_tol = cs["tolerances"]
    draws = []
    for _ in range(n_draws):
        ds = rng.choice(datasets)
        est = rng.choice([e for e in estimators])
        y = rng.choice(ds["y"])
        pool = [v for v in ds["x_continuous"] + ds.get("x_binary", []) if v != y]
        k = rng.randint(est["min_x"], min(est["max_x"], len(pool)))
        xs = rng.sample(pool, k)
        data_path = Path(data_dir) / ds["hbin"]
        stata = build_stata(est, y, xs)
        ref = run_cli(cobra_bin, est["cli_cmd"], y, xs, data_path, est["cli_extra"])
        sub = subject_run(target, cobra_bin=cobra_bin, cli_cmd=est["cli_cmd"], y=y, xs=xs,
                          data_path=data_path, extra=est["cli_extra"],
                          stata_cmd=stata, url=url)
        tag = f"{ds['id']}:{est['id']}:{y}~{'+'.join(xs)}"
        # (a) PARITY — subject vs cobra CLI reference
        btol = families_tol[est["family"]]["b"]
        setol = families_tol[est["family"]]["se"]
        for name, rc in ref.coefs.items():
            sc = sub.coefs.get(name)
            ok_b = sc is not None and rc["b"] is not None and relerr(sc["b"], rc["b"]) <= btol
            rpt.add(f"parity.b[{tag}|{name}]", ok_b,
                    f"sub={sc['b'] if sc else None} ref={rc['b']}")
            if est.get("compare_se") and rc["se"] is not None and sc and sc["se"] is not None:
                rpt.add(f"parity.se[{tag}|{name}]", relerr(sc["se"], rc["se"]) <= setol,
                        f"sub={sc['se']} ref={rc['se']}")
        # (c) STRUCTURE — names == requested vars (+ intercept), n_obs tracks data
        want_names = set(xs) | {"(intercept)"}
        rpt.add(f"structure.names[{tag}]", sub.names == want_names,
                f"got={sorted(sub.names)} want={sorted(want_names)}")
        rpt.add(f"structure.n_obs[{tag}]", sub.n_obs == ref.n_obs and sub.n_obs not in (None, 0),
                f"sub={sub.n_obs} ref={ref.n_obs}")
        draws.append((tag, xs, sub))

    # (b) DIFFERENTIAL — the invariant a mock cannot pass.
    sep = families_tol["differential_min_sep"]
    sigs = {d[2].signature() for d in draws}
    rpt.add("differential.not_all_identical", len(sigs) > 1,
            f"{len(sigs)} distinct signatures across {len(draws)} draws "
            f"(==1 means input-independent => MOCK)")
    # Stronger: find two draws requesting different var-sets; their coef-name sets
    # must differ (a fixed fixture returns the same names regardless of request).
    diffpair = None
    for i in range(len(draws)):
        for j in range(i + 1, len(draws)):
            if set(draws[i][1]) != set(draws[j][1]):
                diffpair = (draws[i], draws[j]); break
        if diffpair:
            break
    if diffpair:
        a, b = diffpair
        names_differ = a[2].names != b[2].names
        # if the two requests share no overlap in coef names at all OR b-vectors differ
        bvec_differ = a[2].signature() != b[2].signature()
        rpt.add("differential.distinct_requests_distinct_output",
                names_differ or bvec_differ,
                f"[{a[0]}] vs [{b[0]}]")
    else:
        rpt.add("differential.distinct_requests_present", False,
                "no two draws with different var-sets (increase --draws)")


# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser(description="anti-mock / differential parity check")
    ap.add_argument("--target", choices=["cli", "kernel", "app", "mock"], default="cli",
                    help="mock = replay PIET's frontend fixture; the check MUST fail "
                         "against it (acceptance: FAIL on mock, PASS on real)")
    ap.add_argument("--cobra", default=str(DEFAULT_COBRA))
    ap.add_argument("--data-dir", default=str(DEFAULT_DATA))
    ap.add_argument("--cache-dir", default="/tmp/cobra_canary")
    ap.add_argument("--canary-set", default=str(CANARY_SET))
    ap.add_argument("--url", default="http://127.0.0.1:7331", help="app target kernel URL")
    ap.add_argument("--draws", type=int, default=8)
    ap.add_argument("--seed", type=int, default=None, help="omit to randomize per run")
    a = ap.parse_args()

    cobra_bin = Path(a.cobra)
    if not cobra_bin.exists():
        print(f"ERROR: cobra not found: {cobra_bin}", file=sys.stderr); sys.exit(2)
    cs = json.loads(Path(a.canary_set).read_text())

    # build the deterministic synthetic canaries into the cache dir
    import make_canary_data as mcd
    mcd.build_all(cobra_bin, a.data_dir, a.cache_dir)

    seed = a.seed if a.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    print(f"random-check target={a.target} seed={seed} draws={a.draws}\n")

    rpt = Report()
    check_closed_form(cs, a.target, cobra_bin, a.cache_dir, a.url, rpt)
    check_draws(cs, a.target, cobra_bin, a.data_dir, a.url, rng, a.draws, rpt)
    rpt.dump()

    if not rpt.ok():
        print("\nRESULT: FAIL — an input-independence / parity invariant broke. "
              "If differential.* failed, the subject is likely MOCK (output does not "
              "depend on input).")
        sys.exit(1)
    print(f"\nRESULT: PASS — output depends on input; parity holds (target={a.target}, seed={seed}).")


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    main()
