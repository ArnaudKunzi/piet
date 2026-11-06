#!/usr/bin/env python3
"""PIET Boundary-B contract test (planner-owned).

Validates that fixtures — and later live kernel responses — conform to the ResultBlock union frozen in
internal note piet/CONTRACT.md. Checks SHAPE only (not numeric parity; numbers live in tests/eval/). Run at each sync
point (D2 / D5 / D7) and whenever the contract changes.

    python3 piet/fixtures/contract_test.py              # validate the fixtures
    python3 piet/fixtures/contract_test.py --live URL   # validate a running kernel (D2+, needs httpx)
"""
import json, sys, glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
COEF_KEYS = {"name", "b", "se", "t", "p", "ci_lo", "ci_hi"}
BLOCK_TYPES = {"echo", "estimation", "summarize", "tabulate", "describe", "browse", "data_mutation", "error"}
ERR_CODES = {"unsupported_command", "unsupported_option", "unsupported_macro", "unsupported_loop",
             "unsupported_program", "unsupported_delimiter", "syntax_error", "engine_error", "no_data_loaded"}


class Bad(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise Bad(msg)


def check_coefrow(r, where):
    need(isinstance(r, dict), f"{where}: coef row not an object")
    need(COEF_KEYS.issubset(r.keys()), f"{where}: coef row missing {COEF_KEYS - set(r.keys())}")
    need(isinstance(r["name"], str), f"{where}: coef name not a string")
    for k in ("b", "se", "t", "p", "ci_lo", "ci_hi"):
        need(r[k] is None or isinstance(r[k], (int, float)), f"{where}: {k} not number|null")


def check_estimation(e, where):
    for k in ("estimator", "depvar", "n_obs", "coeftable"):
        need(k in e, f"{where}: estimation missing '{k}'")
    need(isinstance(e["coeftable"], list) and e["coeftable"], f"{where}: empty coeftable")
    for i, r in enumerate(e["coeftable"]):
        check_coefrow(r, f"{where}.coeftable[{i}]")
    if str(e.get("vcov_type", "")).startswith("cluster"):
        need("(" in e["vcov_type"], f"{where}: cluster vcov_type must be 'cluster(<var>)'")
    for oc in e.get("outcomes", []):
        need("depvar" in oc and "coeftable" in oc, f"{where}: outcome missing depvar/coeftable")
        for i, r in enumerate(oc["coeftable"]):
            check_coefrow(r, f"{where}.outcome[{oc.get('depvar')}][{i}]")


def check_browse(b, where):
    for k in ("offset", "limit", "n_rows_total", "columns", "rows"):
        need(k in b, f"{where}: browse missing '{k}'")
    ncol = len(b["columns"])
    for c in b["columns"]:
        need(c.get("dtype") in ("f64", "factor"), f"{where}: bad column dtype {c.get('dtype')!r}")
    for i, row in enumerate(b["rows"]):
        need(len(row) == ncol, f"{where}: row {i} width {len(row)} != {ncol} columns")


def check_block(b, where):
    t = b.get("type")
    need(t in BLOCK_TYPES, f"{where}: unknown block type {t!r}")
    if t == "estimation":
        check_estimation(b, where)
    elif t == "error":
        need(b.get("code") in ERR_CODES, f"{where}: bad error code {b.get('code')!r}")
        need(isinstance(b.get("message"), str), f"{where}: error message not a string")
    elif t == "echo":
        need(isinstance(b.get("text"), str), f"{where}: echo text not a string")
    elif t == "browse":
        check_browse(b, where)


def check_command_response(d, where):
    need("blocks" in d and isinstance(d["blocks"], list), f"{where}: missing blocks[]")
    need(d["blocks"] and d["blocks"][0].get("type") == "echo", f"{where}: blocks[0] must be an echo")
    for i, b in enumerate(d["blocks"]):
        check_block(b, f"{where}.blocks[{i}]")


def run_fixtures():
    fails = []

    def attempt(label, fn):
        try:
            fn()
            print("ok  ", label)
        except Bad as e:
            fails.append(str(e))
            print("FAIL", label, "—", e)

    for f in sorted(glob.glob(os.path.join(HERE, "api", "command_*.json"))):
        attempt(os.path.basename(f), lambda f=f: check_command_response(json.load(open(f)), os.path.basename(f)))
    bp = os.path.join(HERE, "api", "browse_page.json")
    if os.path.exists(bp):
        attempt("browse_page.json", lambda: check_browse(json.load(open(bp)), "browse_page.json"))

    # golden raw `cobra --tidy` JSON: must carry a valid coeftable[] (or outcomes[].coeftable[])
    for f in sorted(glob.glob(os.path.join(HERE, "golden", "*.json"))):
        name = os.path.basename(f)

        def chk(f=f, name=name):
            g = json.load(open(f))
            tables = [g["coeftable"]] if "coeftable" in g else [o["coeftable"] for o in g.get("outcomes", [])]
            need(tables, f"{name}: no coeftable / outcomes")
            for ti, tb in enumerate(tables):
                for i, r in enumerate(tb):
                    check_coefrow(r, f"{name}.table[{ti}][{i}]")
        attempt(name, chk)

    print()
    if fails:
        print(f"CONTRACT TEST: {len(fails)} failure(s)")
        return 1
    print("CONTRACT TEST: all fixtures conform to CONTRACT.md ✓")
    return 0


def run_live(base_url):
    """D2+: validate a running kernel. Fill in as endpoints come online."""
    import httpx  # dev dep
    fails = []
    r = httpx.post(f"{base_url}/api/command", json={"cmd": "regress price mpg weight, robust"}, timeout=30)
    try:
        check_command_response(r.json(), "live POST /api/command")
        print("ok   live POST /api/command")
    except Bad as e:
        fails.append(str(e))
        print("FAIL live POST /api/command —", e)
    # TODO(planner): add /api/varlist, /api/browse, /api/do, /api/open as the kernel exposes them.
    return 1 if fails else 0


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--live":
        sys.exit(run_live(sys.argv[2].rstrip("/")))
    sys.exit(run_fixtures())
