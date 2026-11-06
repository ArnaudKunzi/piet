#!/usr/bin/env python3
"""Session-mode (contract_A section 26, RESIDENT FRAME) parity leg -- gate (1)b
for MAXPERF Stage 1b, sibling of `../serve_parity` (section 25, gate (1)). See
README.md in this directory for the full rationale (masking, day-1 SKIP
evidence, value-check design, scenario list, spec/engine findings) -- this
docstring is a quick-reference only.

Commissioned as the Eval Harness lane's Task 1 for MAXPERF Stage 1b (cobra
0.29.0, built in a parallel lane on the same 0.28.0 base that shipped `cobra
serve` section 25). Golden-first law, same as serve_parity: lands BEFORE
section 26 exists on any reachable binary (confirmed live: neither
`<private-coordination-tree>/releases/engine/current/cobra` 0.28.0 nor the in-progress
`op-cobra` build recognize a `"frame"` request key yet -- see "Day-1 SKIP").

Protocol under test (pinned by the commission, contract_A section 26 draft):
  use      {"id":N,"frame":{"op":"use","path":"/abs/f.hbin"}}
             -> resp {"id","exit","stdout","stderr",
                       "frame":{rows,cols,vars:[{name,type,n_missing}]}}
  run      {"id":N,"frame":{"op":"run","argv":[<subcommand tokens,
             NO --data/--out>]}}
             -> standard resp + "frame":{rows,cols,changed,added,dropped,
                       structure,vars_delta}; a mutator's result becomes
                       resident IN RAM; state advances ONLY on exit==0.
  snapshot {"id":N,"frame":{"op":"snapshot","path":"/abs/out.hbin"}}
  drop     {"id":N,"frame":{"op":"drop"}}
  run/snapshot without a loaded frame -> exit 3. A frame-run argv containing
  --data/--out -> exit 2, state untouched. Stateless {"argv":[...]} requests
  (section 25) may interleave and must not affect resident state.

Extended for MAXPERF Stage 2 (contract_A section 27, PROPOSED, MI-2xx): three
additive session-snapshot ops layered on the same `"frame"` request/response
shape --
  stash        {"id":N,"frame":{"op":"stash","name":"<key>"}}
                 -> "frame":{"rows","cols","name"}. exit 3 if no resident.
  unstash      {"id":N,"frame":{"op":"unstash","name":"<key>"[,"keep":bool]}}
                 -> the section-26 `use` body {"rows","cols","vars":[...]}.
                 keep defaults false (consumes the stash); true retains it.
                 exit 3 ("no stash named '<key>'") if the name is unknown --
                 this does NOT require a resident frame to be present.
  save_virtual {"id":N,"frame":{"op":"save_virtual","path":"/abs/x"}}
                 -> "frame":{"rows","cols","path"}. Registers `path` in an
                 in-RAM virtual-file table (no disk write, confirmed live --
                 the path never appears on disk). exit 3 if no resident.
  use (existing op, added fast-path) -- a `path` present in the virtual-file
                 table is restored from RAM (same use-body shape); otherwise
                 falls through to the unchanged disk open. Virtual/stash
                 entries are cleared only by `drop` (a plain disk `use` of a
                 different root leaves them intact).
Missing "name"/"path" on stash/unstash/save_virtual -> exit 2 (confirmed
live, distinct from the exit-3 "no resident"/"no stash named" paths). Day-1
absence (section 27 not yet built) answers any of these three new ops with
the SAME generic-unknown-op path section 26 itself uses -- exit 2, stderr
`serve: unknown frame op '<op>'` -- confirmed live against the 0.29.0
release binary. See `probe_stash_support()` and README "Day-1 SKIP".

Proves parity along a different axis than serve_parity: not "same argv, two
transports" (session argv deliberately OMITS --data/--out, so the two arms'
argv are NOT byte-identical by construction) but "resident frame after N
session ops == disk state after the equivalent N one-shot `--data in --out
out` invocations chained by hand", checked both per-verb (masked stdout/
stderr byte-compare) and at the value level (final `snapshot` vs the
reference chain head, parsed back with `tools/hbin_read.py`). The section-27
scenarios (s13-s16) apply the identical axis to stash/unstash/save_virtual:
a restored snapshot (in-session, via `unstash` or a virtual-hit `use`) must
value-match the one-shot chain's on-disk state AT THE MOMENT the snapshot
was taken, regardless of whatever further mutation happened to the resident
(or to other stashes) afterward.

Usage:
    python3 run_serve_session.py --cobra PATH [--only s01,s04] [--limit N]
                                  [--scratch DIR] [--allow-no-frame]
                                  [--allow-no-stash] [--json OUT.json]

Exit codes: 0 = no RED (including the graceful all-SKIP day-1 path, but only
if --allow-no-frame/--allow-no-stash was passed) * 1 = at least one RED, OR
section 26 unavailable AND --allow-no-frame was NOT passed, OR section 27
unavailable AND --allow-no-stash was NOT passed (these are now the hard-RED
defaults -- --allow-no-frame/--allow-no-stash opt back into the old
graceful-SKIP behavior). Section-26 absence SKIPs every scenario (s01-s16);
section-27 absence (with section 26 present) SKIPs only s13-s16, leaving
s01-s12 comparable as before.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
import pandas as pd

SERVE_SESSION = Path(__file__).resolve().parent
EVAL = SERVE_SESSION.parent
sys.path.insert(0, str(EVAL))
sys.path.insert(0, str(EVAL / "serve_parity"))
import run_serve_parity as SP  # noqa: E402 -- reused for ServeSession/ServeUnavailable/mask_timing/
                                #  first_diff_excerpt (see module docstring "Masking"); SP.RE is
                                #  run_eval.py, imported transitively, reused only for its COBRA/
                                #  ROOT path-resolution constants (no manifest/case logic needed here
                                #  -- this leg's scenarios are self-generated, not manifest-sourced).

ROOT = SP.RE.ROOT
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import hbin_read  # noqa: E402 -- reused verbatim for the end-of-scenario value compare.
import to_hbin as TH  # noqa: E402 -- reused verbatim (encode_column/align_up/MAGIC) for fixture-writing.


# ───────────────────────── fixtures (self-generated, scratch-only) ─────────────────────────
def write_hbin(df, path, factor_cols=()):
    """Write `df` to the cecon .hbin format at `path`, reusing to_hbin.py's own
    encode_column/align_up/MAGIC (the exact functions `tools/to_hbin.py` itself calls from
    its CLI main()) rather than reimplementing the columnar layout."""
    meta_cols, blobs, rel = [], [], 0
    for name in df.columns:
        meta, blob = TH.encode_column(name, df[name], name in factor_cols)
        meta["offset"] = rel
        meta["nbytes"] = len(blob)
        meta_cols.append(meta)
        blobs.append(blob)
        rel = TH.align_up(rel + len(blob))
    header = {"magic": "CECONHB1", "version": 1, "n_rows": int(len(df)), "columns": meta_cols}
    header_json = json.dumps(header, ensure_ascii=False).encode("utf-8")
    data_start = TH.align_up(16 + len(header_json))
    with open(path, "wb") as f:
        f.write(TH.MAGIC)
        f.write(struct.pack("<Q", len(header_json)))
        f.write(header_json)
        f.write(b"\0" * (data_start - (16 + len(header_json))))
        pos = data_start
        for meta, blob in zip(meta_cols, blobs):
            want = data_start + meta["offset"]
            if pos < want:
                f.write(b"\0" * (want - pos))
                pos = want
            f.write(blob)
            pos += len(blob)
    return path


def make_fixture(path, n=60, seed=0):
    """The canonical tiny scenario fixture: one string factor ('g'), one numeric
    group key suitable for `collapse --by` (which rejects factor --by -- see README
    'Spec/engine inconsistencies found'), a small-integer column for `recode`, and
    a pure-random binary column for `logit` (independent of the other regressors,
    so it never hits perfect separation)."""
    rng = np.random.default_rng(seed)
    df = pd.DataFrame({
        "id": np.arange(1, n + 1, dtype=float),
        "x1": rng.normal(10, 2, n),
        "x2": rng.normal(0, 1, n),
        "cat": rng.integers(1, 5, n).astype(float),
        "grp": rng.integers(1, 4, n).astype(float),
        "g": rng.choice(["A", "B", "C"], n),
        "d": rng.integers(0, 2, n).astype(float),
    })
    write_hbin(df, path, factor_cols=("g",))
    return path


def make_using_fixture(path, n=60, seed=0):
    rng = np.random.default_rng(seed)
    df = pd.DataFrame({"id": np.arange(1, n + 1, dtype=float),
                        "extra1": rng.normal(size=n), "extra2": rng.normal(size=n)})
    write_hbin(df, path)
    return path


def ground_truth_shape(path):
    n, cols = hbin_read.read_hbin(path)
    names = sorted(k for k in cols if not k.endswith("__levels"))
    return n, names


# ───────────────────────── masking / comparison ─────────────────────────
def mask_provenance(s):
    """Regex can't safely match `"provenance":{...}` (it nests -- confirmed live: a
    "data":{"path":...,"fnv1a64":...} sub-object one level deep) the way serve_parity's
    non-nested time_ms/timing_ms regexes can, so this does a real brace-depth scan
    (quote/escape aware) and replaces the WHOLE object with a placeholder, preserving
    everything else byte-for-byte. See module docstring "Masking" for why this is
    load-bearing for this leg specifically (argv/data.path legitimately differ between
    arms by construction, unlike serve_parity where argv is identical across arms)."""
    if not s or '"provenance"' not in s:
        return s
    key_re = re.compile(r'"provenance"\s*:\s*')
    out, i, n = [], 0, len(s)
    while True:
        m = key_re.search(s, i)
        if not m:
            out.append(s[i:])
            break
        out.append(s[i:m.start()])
        j = m.end()
        if j >= n or s[j] != "{":
            out.append(s[m.start():])  # not an object -- bail out safely, keep the rest verbatim
            break
        depth, in_str, esc, k = 0, False, False, j
        while k < n:
            c = s[k]
            if in_str:
                if esc:
                    esc = False
                elif c == "\\":
                    esc = True
                elif c == '"':
                    in_str = False
            else:
                if c == '"':
                    in_str = True
                elif c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        k += 1
                        break
            k += 1
        out.append('"provenance":"<masked>"')
        i = k
    return "".join(out)


def mask_all(s):
    return mask_provenance(SP.mask_timing(s))


def strip_wrote_lines(s):
    """Drop any line containing 'wrote' -- the one-shot mutator write-confirmation,
    format varies per verb (see module docstring). Reference-arm stderr only; the
    session arm never writes to disk for a `run`, so it has no such line to strip."""
    if not s:
        return s
    return "\n".join(ln for ln in s.splitlines() if "wrote" not in ln)


def compare_verb(label, one, sess, diffs):
    if one["rc"] != sess["rc"]:
        diffs.append(f"{label}:exit  one-shot={one['rc']!r} session={sess['rc']!r}")
    a_out, b_out = mask_all(one.get("out") or ""), mask_all(sess.get("out") or "")
    if a_out != b_out:
        diffs.append(f"{label}:stdout diverged -- {SP.first_diff_excerpt(a_out, b_out)}")
    a_err = mask_all(strip_wrote_lines(one.get("err") or ""))
    b_err = mask_all(sess.get("err") or "")
    if a_err != b_err:
        diffs.append(f"{label}:stderr diverged -- {SP.first_diff_excerpt(a_err, b_err)}")


def expect_exit(rec, expected, ctx, diffs):
    if rec["rc"] != expected:
        diffs.append(f"{ctx}: expected exit={expected}, got exit={rec['rc']!r} "
                      f"stderr={rec.get('err', '')[:200]!r}")
        return False
    return True


def check_use_frame_meta(frame, gt_rows, gt_names, diffs, name):
    if not isinstance(frame, dict):
        diffs.append(f"{name}.use: response missing a 'frame' object")
        return
    if frame.get("rows") != gt_rows:
        diffs.append(f"{name}.use: frame.rows={frame.get('rows')!r} expected {gt_rows}")
    if frame.get("cols") != len(gt_names):
        diffs.append(f"{name}.use: frame.cols={frame.get('cols')!r} expected {len(gt_names)}")
    vlist = frame.get("vars")
    if isinstance(vlist, list):
        got = sorted(v.get("name") for v in vlist if isinstance(v, dict))
        if got != gt_names:
            diffs.append(f"{name}.use: frame.vars names {got} != fixture columns {gt_names}")


def check_run_frame_coherence(frame, expect_path, diffs, label):
    """Spec-required check (see commission): rows/cols after a `run` must match the
    reference chain-head's actual shape. Only rows/cols -- the commission does not
    pin changed/added/dropped/structure/vars_delta's exact shape, so this tool does
    not guess at asserting them."""
    if not isinstance(frame, dict):
        diffs.append(f"{label}: run response missing a 'frame' object (rows/cols unverifiable)")
        return
    gt_rows, gt_names = ground_truth_shape(expect_path)
    if frame.get("rows") != gt_rows:
        diffs.append(f"{label}: frame.rows={frame.get('rows')!r} != reference shape rows={gt_rows}")
    if frame.get("cols") != len(gt_names):
        diffs.append(f"{label}: frame.cols={frame.get('cols')!r} != reference shape cols={len(gt_names)}")


def compare_hbin_values(session_path, ref_path, label, diffs):
    """End-of-scenario VALUE compare (see module docstring): column set, per-column
    dtype, factor level dictionaries, and every value -- in stored row order."""
    try:
        n_s, cols_s = hbin_read.read_hbin(session_path)
        n_r, cols_r = hbin_read.read_hbin(ref_path)
    except Exception as ex:  # noqa: BLE001
        diffs.append(f"{label}: could not read hbin for value compare: {ex!r}")
        return
    names_s = sorted(k for k in cols_s if not k.endswith("__levels"))
    names_r = sorted(k for k in cols_r if not k.endswith("__levels"))
    if names_s != names_r:
        diffs.append(f"{label}: column set differs: session={names_s} reference={names_r}")
        return
    if n_s != n_r:
        diffs.append(f"{label}: row count differs: session={n_s} reference={n_r}")
        return
    for nm in names_s:
        va, vb = cols_s[nm], cols_r[nm]
        lv_a, lv_b = cols_s.get(nm + "__levels"), cols_r.get(nm + "__levels")
        if (lv_a is not None) != (lv_b is not None):
            diffs.append(f"{label}: column '{nm}' dtype differs (factor vs f64)")
            continue
        if lv_a is not None:
            if list(lv_a) != list(lv_b):
                diffs.append(f"{label}: column '{nm}' factor levels differ: {lv_a} vs {lv_b}")
            if not np.array_equal(va, vb):
                idx = int(np.argmax(va != vb))
                diffs.append(f"{label}: column '{nm}' factor codes differ (first at row {idx}): "
                              f"session={va[idx]} reference={vb[idx]}")
        else:
            if not np.array_equal(va, vb, equal_nan=True):
                bad = ~((va == vb) | (np.isnan(va) & np.isnan(vb)))
                idx = int(np.argmax(bad))
                diffs.append(f"{label}: column '{nm}' values differ (first at row {idx}): "
                              f"session={va[idx]!r} reference={vb[idx]!r}")


def check_stash_like_frame(frame, gt_rows, gt_names, key_field, key_expected, diffs, label):
    """Section-27 success-response check for `stash`/`save_virtual`: both return
    the SAME shape `{"rows","cols",<key_field>}` (`"name"` or `"path"`), unlike
    `unstash`/a virtual-hit `use` which re-establish the full section-26 `use`
    body (see `check_use_frame_meta`, reused verbatim for those two)."""
    if not isinstance(frame, dict):
        diffs.append(f"{label}: response missing a 'frame' object")
        return
    if frame.get("rows") != gt_rows:
        diffs.append(f"{label}: frame.rows={frame.get('rows')!r} expected {gt_rows}")
    if frame.get("cols") != len(gt_names):
        diffs.append(f"{label}: frame.cols={frame.get('cols')!r} expected {len(gt_names)}")
    got = frame.get(key_field)
    if got != key_expected:
        diffs.append(f"{label}: frame.{key_field}={got!r} expected {key_expected!r}")


def snapshot_and_compare(session, workdir, tag, ref_path, diffs):
    """Interim value-check (no drop): snapshot whatever is CURRENTLY resident and
    compare it to `ref_path` via `compare_hbin_values`. Used mid-scenario at each
    stash/unstash/save_virtual restore point in s13-s16, where the final
    `snapshot_compare_and_drop` (end-of-scenario, unchanged) is too late -- the
    point of these scenarios is that an EARLIER restore must match a
    correspondingly earlier one-shot reference state, not the scenario's final
    state."""
    snap_path = workdir / f"{tag}.hbin"
    snap = session.frame_snapshot(snap_path)
    if snap["rc"] != 0:
        diffs.append(f"{tag}: interim snapshot failed: exit={snap['rc']!r} stderr={snap['err']!r}")
        return
    compare_hbin_values(snap_path, ref_path, tag, diffs)


# ───────────────────────── session transport ─────────────────────────
class FrameUnavailable(Exception):
    pass


class FrameSession(SP.ServeSession):
    """Extends serve_parity's ServeSession (handshake/`_readline`/shutdown/cleanup,
    all reused verbatim -- section 26 is additive on top of the same handshake and
    the same line-JSON transport) with the four `"frame":{...}` request ops. The
    stateless `.request(argv)` method (section 25) is inherited unchanged, used
    directly by scenario s12's interleaved-request check."""

    def _send_frame(self, frame_obj, timeout):
        self._next_id += 1
        rid = self._next_id
        try:
            self.proc.stdin.write(json.dumps({"id": rid, "frame": frame_obj}) + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as ex:
            raise RuntimeError(f"serve session pipe broke sending id={rid} frame={frame_obj}: {ex}") from ex
        line = self._readline(self.proc.stdout, timeout)
        if line is None:
            rc = self.proc.poll()
            raise RuntimeError(f"serve session died/EOF answering id={rid} frame={frame_obj} (exit={rc})")
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as ex:
            raise RuntimeError(f"serve response id={rid} not valid JSON: {line[:200]!r}") from ex
        # A request-shape rejection (day-1 absence, or a genuine section-26 validation
        # error) may echo id=null rather than rid -- confirmed live on 0.28.0's generic
        # "malformed request line" path. Tolerate null; still catch real cross-talk.
        if obj.get("id") not in (rid, None):
            raise RuntimeError(f"serve id mismatch: sent {rid}, got {obj.get('id')!r} ({line[:200]!r})")
        # "est" (contract_A section 31) is carried through ADDITIVELY: `.get` yields
        # None on every pre-1.20.0 response and on every op that does not mint, so
        # scenarios s01-s16 are byte-unaffected by its presence in this dict. The
        # scenarios that DO read it (s17-s20) distinguish "absent" (None) from
        # "present" explicitly -- never by truthiness, since an empty dict would be
        # a contract violation we want to SEE rather than silently treat as absent.
        return {"cmd": ["<frame>", json.dumps(frame_obj)], "rc": obj.get("exit"),
                "out": obj.get("stdout", ""), "err": obj.get("stderr", ""), "frame": obj.get("frame"),
                "est": obj.get("est"), "has_frame_member": "frame" in obj, "has_est_member": "est" in obj}

    def frame_use(self, path, timeout=60.0):
        return self._send_frame({"op": "use", "path": str(path)}, timeout)

    def frame_run(self, argv, timeout=120.0):
        return self._send_frame({"op": "run", "argv": list(argv)}, timeout)

    def frame_snapshot(self, path, timeout=60.0):
        return self._send_frame({"op": "snapshot", "path": str(path)}, timeout)

    def frame_drop(self, timeout=30.0):
        return self._send_frame({"op": "drop"}, timeout)

    # ── section 27 (session snapshots) -- additive over the four ops above ──
    def frame_stash(self, name, timeout=30.0):
        return self._send_frame({"op": "stash", "name": name}, timeout)

    def frame_unstash(self, name, keep=None, timeout=60.0):
        obj = {"op": "unstash", "name": name}
        if keep is not None:
            obj["keep"] = bool(keep)
        return self._send_frame(obj, timeout)

    def frame_save_virtual(self, path, timeout=60.0):
        return self._send_frame({"op": "save_virtual", "path": str(path)}, timeout)

    # ── section 31 (session estimation store) -- post-estimation by handle ──
    def frame_postest(self, est_handle, argv, timeout=120.0):
        """`{"op":"postest","est":"<handle>","argv":[...]}`. The handle is passed
        through VERBATIM and is never constructed or parsed here: section 31 requires
        the client to treat it as opaque, so a gate that asserted on its internal
        shape would pin something the contract explicitly declines to promise."""
        return self._send_frame({"op": "postest", "est": str(est_handle), "argv": list(argv)}, timeout)


def probe_frame_support(session, probe_fixture, timeout=15.0):
    """Day-1 absence probe (see module docstring). Raises FrameUnavailable with a
    dense reason on anything other than a well-formed successful `use` response."""
    try:
        resp = session.frame_use(probe_fixture, timeout=timeout)
    except RuntimeError as ex:
        raise FrameUnavailable(f"frame mode unavailable (session error on probe: {ex})") from ex
    frame = resp.get("frame")
    if resp["rc"] != 0 or not isinstance(frame, dict) or "rows" not in frame:
        raise FrameUnavailable(
            "frame mode (contract_A section 26) unavailable on this binary (probe frame-use "
            f"returned exit={resp['rc']!r}, frame={frame!r}, stderr={resp.get('err', '')[:200]!r}) "
            "-- expected on cobra builds before the RESIDENT FRAME session feature lands "
            "(e.g. 0.28.0); treating as day-1 SKIP.")
    return resp


class StashUnavailable(Exception):
    pass


def probe_stash_support(session, timeout=15.0):
    """Day-1 absence probe for contract_A section 27 (stash/unstash/save_virtual).
    Requires a resident frame already loaded (reuses whatever `probe_frame_support`
    established -- section 27's ops all need a resident to stash FROM). Confirmed
    live: a binary that lacks section 27 answers `stash` (or any of the three new
    ops) via the SAME generic-unknown-op path section 26 itself uses for a bad op
    string -- `exit 2`, stderr `serve: unknown frame op 'stash'` -- on the real
    0.29.0 release binary reachable at authoring time. A binary that HAS section
    27 answers a well-formed `stash` with `exit 0` and a `frame:{rows,cols,name}`
    body -- confirmed live against the op-cobra fix/cow-stage2 build. Treats
    anything else (nonzero exit, or exit 0 without a dict frame containing
    'name') as StashUnavailable, same conservative convention as
    probe_frame_support. On success, the probe's own stash entry is consumed
    (unstash, keep=False) so it cannot collide with any scenario's stash name."""
    probe_key = "__s27_probe_stash__"
    try:
        resp = session.frame_stash(probe_key, timeout=timeout)
    except RuntimeError as ex:
        raise StashUnavailable(f"section 27 unavailable (session error on stash probe: {ex})") from ex
    frame = resp.get("frame")
    if resp["rc"] != 0 or not isinstance(frame, dict) or "name" not in frame:
        raise StashUnavailable(
            "contract_A section 27 (stash/unstash/save_virtual) unavailable on this binary "
            f"(probe frame-stash returned exit={resp['rc']!r}, frame={frame!r}, "
            f"stderr={resp.get('err', '')[:200]!r}) -- expected on cobra builds before session "
            "snapshots land (e.g. 0.29.0 release); treating as day-1 SKIP.")
    try:
        session.frame_unstash(probe_key, keep=False, timeout=timeout)
    except RuntimeError:
        pass  # probe cleanup best-effort; a scenario's own `use` replaces the resident regardless
    return resp


class PostestUnavailable(Exception):
    pass


def probe_postest_support(session, timeout=15.0):
    """Day-1 absence probe for contract_A section 31 (the `postest` op + the `est`
    response member). Same golden-first convention as probe_stash_support: this leg
    lands alongside the engine change and must SKIP -- not RED -- on every binary
    that predates it, including the pinned reference engine at
    `<private-coordination-tree>/releases/engine/current/cobra` and every operator branch that has
    not yet merged the change.

    The discriminator is exact and needs no resident frame (section 31: "No resident
    frame is required" -- a stored model references no data):
      - a binary WITHOUT section 31 routes `postest` through the SAME generic
        unknown-op path sections 26/27 use -> exit 2, stderr
        `serve: unknown frame op 'postest'`;
      - a binary WITH section 31 validates the request, looks the handle up, misses,
        and answers the section-31 honest-miss -> exit 3, stderr `no stored model`.
    Anything else is treated as unavailable, same conservative convention as the
    other two probes -- an unrecognized answer must never be read as coverage.

    The probe handle is deliberately synthetic and cannot collide with a minted one:
    handles are nonce-salted per serve PROCESS, so no live process can have minted
    this string."""
    probe_handle = "est:0000000000000000:__s31_probe__"
    try:
        resp = session.frame_postest(probe_handle, ["lincom", "--exp", "_cons"], timeout=timeout)
    except RuntimeError as ex:
        raise PostestUnavailable(f"section 31 unavailable (session error on postest probe: {ex})") from ex
    if resp["rc"] == 3 and "no stored model" in (resp.get("err") or ""):
        return resp
    raise PostestUnavailable(
        "contract_A section 31 (postest / est handle) unavailable on this binary "
        f"(probe frame-postest returned exit={resp['rc']!r}, "
        f"stderr={resp.get('err', '')[:200]!r}) -- expected on cobra builds before the "
        "session estimation store lands; treating as day-1 SKIP.")


def est_of(rec, ctx, diffs, required=True):
    """Pull the section-31 `est` object off a response, recording a diff when it is
    absent-but-required or present-but-malformed. Returns None when unusable, so
    callers can bail without a second isinstance dance at every site."""
    est = rec.get("est")
    if est is None:
        if required:
            diffs.append(f"{ctx}: response carries no 'est' member (contract_A section 31 "
                          f"requires one on a successful mint / on every postest response); "
                          f"exit={rec['rc']!r} stderr={rec.get('err', '')[:200]!r}")
        return None
    if not isinstance(est, dict):
        diffs.append(f"{ctx}: 'est' is {type(est).__name__}, expected an object: {est!r}")
        return None
    return est


def lincom_b_se_df(rec, ctx, diffs):
    """Parse a lincom result to (b, se, df) for its FIRST --exp. Returns a triple of
    Nones on any parse failure, recording the reason -- a scenario that silently
    compared None to None would pass while proving nothing."""
    try:
        payload = json.loads(rec.get("out") or "")
    except (ValueError, TypeError) as ex:
        diffs.append(f"{ctx}: stdout is not valid JSON ({ex}); stdout={str(rec.get('out'))[:200]!r} "
                      f"stderr={rec.get('err', '')[:200]!r}")
        return None, None, None
    coefs = payload.get("coefs")
    if not isinstance(coefs, list) or not coefs or not isinstance(coefs[0], dict):
        diffs.append(f"{ctx}: lincom payload has no usable 'coefs' array: {str(payload)[:200]!r}")
        return None, None, None
    c = coefs[0]
    return c.get("b"), c.get("se"), payload.get("df")


# ───────────────────────── one-shot reference half ─────────────────────────
class Verb:
    __slots__ = ("subcmd", "flags", "mutator")

    def __init__(self, subcmd, flags, mutator=True):
        self.subcmd = subcmd
        self.flags = list(flags)
        self.mutator = mutator  # False = read-only/estimator: no --out in one-shot,
                                 #  chain head unchanged regardless of exit code.


def one_shot_call(cobra_bin, verb, data_path, out_path, timeout=120):
    argv = [verb.subcmd, "--data", str(data_path)] + verb.flags
    if verb.mutator:
        argv = argv + ["--out", str(out_path)]
    cmd = [str(cobra_bin)] + argv
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return {"cmd": cmd, "rc": p.returncode, "out": p.stdout, "err": p.stderr}


def snapshot_compare_and_drop(session, workdir, name, ref_final_path, diffs,
                               ok_to_compare=True, post_drop_check=False):
    snap_path = workdir / f"{name}_snapshot.hbin"
    snap = session.frame_snapshot(snap_path)
    if snap["rc"] != 0:
        diffs.append(f"{name}: snapshot failed: exit={snap['rc']!r} stderr={snap['err']!r}")
    elif ok_to_compare:
        compare_hbin_values(snap_path, ref_final_path, f"{name}.final", diffs)
    drop = session.frame_drop()
    if drop["rc"] != 0:
        diffs.append(f"{name}: drop failed: exit={drop['rc']!r} stderr={drop['err']!r}")
        return
    if post_drop_check:
        # Bonus coverage of "run/snapshot without a frame -> exit 3" -- exercised once
        # here (rather than inventing a 13th named scenario) since every OTHER scenario
        # ends with a live resident frame by design.
        pr = session.frame_run(["eval", "--summarize", "x1"])
        if pr["rc"] != 3:
            diffs.append(f"{name}: post-drop frame-run expected exit=3 (no frame loaded) "
                          f"per contract_A section 26, got exit={pr['rc']!r}")
        ps = session.frame_snapshot(workdir / f"{name}_postdrop.hbin")
        if ps["rc"] != 3:
            diffs.append(f"{name}: post-drop frame-snapshot expected exit=3 (no frame loaded), "
                          f"got exit={ps['rc']!r}")


# ───────────────────────── scenarios 1-9: plain verb chains ─────────────────────────
def run_scenario(name, session, workdir, cobra_bin, fixture, verbs):
    diffs = []
    u = session.frame_use(fixture)
    if not expect_exit(u, 0, f"{name}.use", diffs):
        return "RED", diffs
    gt_rows, gt_names = ground_truth_shape(fixture)
    check_use_frame_meta(u.get("frame"), gt_rows, gt_names, diffs, name)

    head = fixture
    for k, v in enumerate(verbs, 1):
        label = f"{name}.v{k}[{v.subcmd}]"
        out_path = workdir / f"{name}_v{k}.hbin"
        one = one_shot_call(cobra_bin, v, head, out_path)
        sess = session.frame_run([v.subcmd] + v.flags)
        expect_exit(one, 0, f"{label}.reference", diffs)
        expect_exit(sess, 0, f"{label}.session", diffs)
        compare_verb(label, one, sess, diffs)
        if one["rc"] == 0:
            check_run_frame_coherence(sess.get("frame"), out_path if v.mutator else head, diffs, label)
        if v.mutator and one["rc"] == 0:
            head = out_path

    snapshot_compare_and_drop(session, workdir, name, head, diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_1(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 1)
    verbs = [Verb("eval", ["--gen", "z1 = x1 + x2"]),
             Verb("eval", ["--gen", "z2 = z1 * 2"]),
             Verb("eval", ["--gen", "z3 = ln(abs(z2) + 1)"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_2(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 2)
    verbs = [Verb("eval", ["--gen", "cat = cat + 10"]),  # replace-in-place (--gen overwrite)
             Verb("recode", ["--var", "cat", "--rules", "(11 12=1)(13 14=2)(else=3)", "--gen", "cat_r"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_3(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 3)
    verbs = [Verb("eval", ["--egen", "gmean_x1 = mean(x1), by(g)"]),
             Verb("eval", ["--egen", "rank_x1 = rank(x1), by(g)"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_4(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 4)
    # seq() numbers rows in CURRENT PHYSICAL order (no internal by/sort clause of its
    # own, unlike --bygen) -- genuinely sensitive to whether gsort's reorder actually
    # persisted in the resident frame, which is the point of this scenario's name.
    verbs = [Verb("gsort", ["--vars", "g,-x1"]),
             Verb("eval", ["--egen", "rownum = seq(), by(g)"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_5(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 5)
    verbs = [Verb("eval", ["--drop", "x2"]),
             Verb("eval", ["--keepif", "x1 > 9"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_6(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 6)
    using = workdir / f"{name}_using.hbin"
    make_using_fixture(using, seed=seed + 60)
    verbs = [Verb("merge", ["--using", str(using), "--type", "1:1", "--key", "id"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_7(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 7)
    # NOTE: collapse --by REJECTS a factor column ("column 'g' is a factor, not f64"
    # -- confirmed live) despite the scenario's own "by factor" name; 'grp' (plain
    # numeric group code) is used instead. See README "Spec/engine inconsistencies".
    verbs = [Verb("collapse", ["--stat", "(mean) mx1=x1 (mean) mx2=x2", "--by", "grp"])]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_8(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 8)
    verbs = [Verb("eval", ["--gen", "x1x2 = x1 * x2"]),
             Verb("ols", ["--y", "x1", "--x", "x2,grp"], mutator=False),
             Verb("eval", ["--summarize", "x1,x2,x1x2"], mutator=False)]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


def scenario_9(name, session, workdir, cobra_bin, seed):
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 9)
    verbs = [Verb("eval", ["--keepif", "id > 5"]),
             Verb("logit", ["--y", "d", "--x", "x1,x2"], mutator=False)]
    return run_scenario(name, session, workdir, cobra_bin, fx, verbs)


# ───────────────────────── scenarios 10-12: protocol-behavior checks ─────────────────────────
def scenario_10(name, session, workdir, cobra_bin, seed):
    """Error mid-chain: verb2 fails for real (see README -- substituted an undefined-
    variable reference for the commission's literal "unknown flag", since `eval`'s
    own hand-rolled mini-language silently ignores unrecognized flags by documented
    design, ctest gate BUG-060/E6 -- ANY unknown flag on `eval` specifically would
    NOT reproduce an error at all). State must not advance: verb3 must match a
    reference chain where verb2 never happened."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 10)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v1 = Verb("eval", ["--gen", "ok1 = x1 + 1"])
    v2_bad = Verb("eval", ["--gen", "bad1 = zzz_not_a_real_var * 2"])
    v3 = Verb("eval", ["--gen", "ok2 = ok1 * 2"])

    expect_exit(session.frame_run([v1.subcmd] + v1.flags), 0, f"{name}.verb1-setup", diffs)
    s2 = session.frame_run([v2_bad.subcmd] + v2_bad.flags)
    if s2["rc"] == 0:
        diffs.append(f"{name}: verb2 (deliberately bad) unexpectedly SUCCEEDED in the session "
                      f"arm (expected a real engine error; state must not advance)")
    s3 = session.frame_run([v3.subcmd] + v3.flags)
    expect_exit(s3, 0, f"{name}.verb3-post-error", diffs)

    out1 = workdir / f"{name}_r1.hbin"
    r1 = one_shot_call(cobra_bin, v1, fx, out1)
    if not expect_exit(r1, 0, f"{name}.reference-verb1", diffs):
        return "RED", diffs
    r2_bad = one_shot_call(cobra_bin, v2_bad, out1, workdir / f"{name}_r2bad.hbin")
    if r2_bad["rc"] == 0:
        diffs.append(f"{name}: sanity check failed -- verb2 ALSO succeeded in the one-shot "
                      f"reference (not a real error on this engine build; scenario needs a "
                      f"different injected failure)")
    out3 = workdir / f"{name}_r3.hbin"
    r3 = one_shot_call(cobra_bin, v3, out1, out3)  # data=out1 (post-verb1 only) -- verb2 SKIPPED

    compare_verb(f"{name}.verb3-vs-skip-verb2", r3, s3, diffs)
    if r3["rc"] == 0:
        check_run_frame_coherence(s3.get("frame"), out3, diffs, f"{name}.verb3")
    snapshot_compare_and_drop(session, workdir, name, out3, diffs, ok_to_compare=(r3["rc"] == 0))
    return ("RED" if diffs else "PASS"), diffs


def scenario_11(name, session, workdir, cobra_bin, seed):
    """--data-in-run / --out-in-run rejection: both must exit 2 without disturbing
    resident state; a normal verb afterward must succeed and match a reference
    chain that never saw the illegal requests."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 11)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v0 = Verb("eval", ["--gen", "ok = x1 + 1"])
    vrec = Verb("eval", ["--gen", "ok2 = ok * 3"])
    expect_exit(session.frame_run([v0.subcmd] + v0.flags), 0, f"{name}.setup", diffs)

    illegal_data = session.frame_run(["eval", "--data", "/tmp/__serve_session_bogus_ignore.hbin",
                                       "--gen", "z = 1"])
    expect_exit(illegal_data, 2, f"{name}.illegal-data-in-run", diffs)
    illegal_out = session.frame_run(["eval", "--gen", "z = 1",
                                      "--out", "/tmp/__serve_session_bogus_ignore_out.hbin"])
    expect_exit(illegal_out, 2, f"{name}.illegal-out-in-run", diffs)

    srec = session.frame_run([vrec.subcmd] + vrec.flags)
    expect_exit(srec, 0, f"{name}.recovery-after-rejection", diffs)

    out0 = workdir / f"{name}_r0.hbin"
    r0 = one_shot_call(cobra_bin, v0, fx, out0)
    if not expect_exit(r0, 0, f"{name}.reference-setup", diffs):
        return "RED", diffs
    outrec = workdir / f"{name}_rrec.hbin"
    rrec = one_shot_call(cobra_bin, vrec, out0, outrec)

    compare_verb(f"{name}.recovery-vs-clean-chain", rrec, srec, diffs)
    if rrec["rc"] == 0:
        check_run_frame_coherence(srec.get("frame"), outrec, diffs, f"{name}.recovery")
    snapshot_compare_and_drop(session, workdir, name, outrec, diffs, ok_to_compare=(rrec["rc"] == 0))
    return ("RED" if diffs else "PASS"), diffs


def scenario_12(name, session, workdir, cobra_bin, seed):
    """Interleaved stateless request mid-session: a plain section-25 {"argv":[...]}
    request against a DIFFERENT file must behave exactly like one-shot (already the
    subject of serve_parity; re-verified locally here) AND must leave the resident
    frame (a wholly separate file) untouched."""
    fxA = workdir / f"{name}_a.hbin"
    make_fixture(fxA, seed=seed + 12)
    fxB = workdir / f"{name}_b.hbin"
    make_fixture(fxB, seed=seed + 120)
    diffs = []
    if not expect_exit(session.frame_use(fxA), 0, f"{name}.use", diffs):
        return "RED", diffs

    va1 = Verb("eval", ["--gen", "a1 = x1 + 1"])
    va2 = Verb("eval", ["--gen", "a2 = a1 * 2"])
    expect_exit(session.frame_run([va1.subcmd] + va1.flags), 0, f"{name}.verb1", diffs)

    stateless_argv = ["eval", "--data", str(fxB), "--summarize", "x1"]
    sess_stateless = session.request(stateless_argv)  # inherited section-25 stateless path
    p = subprocess.run([str(cobra_bin)] + stateless_argv, capture_output=True, text=True, timeout=60)
    ref_stateless = {"cmd": [str(cobra_bin)] + stateless_argv, "rc": p.returncode,
                      "out": p.stdout, "err": p.stderr}
    compare_verb(f"{name}.interleaved-stateless-vs-oneshot", ref_stateless, sess_stateless, diffs)

    s2 = session.frame_run([va2.subcmd] + va2.flags)
    expect_exit(s2, 0, f"{name}.verb2-post-interleave", diffs)

    out1 = workdir / f"{name}_r1.hbin"
    r1 = one_shot_call(cobra_bin, va1, fxA, out1)
    if not expect_exit(r1, 0, f"{name}.reference-verb1", diffs):
        return "RED", diffs
    out2 = workdir / f"{name}_r2.hbin"
    r2 = one_shot_call(cobra_bin, va2, out1, out2)

    compare_verb(f"{name}.verb2-vs-clean-chain", r2, s2, diffs)
    if r2["rc"] == 0:
        check_run_frame_coherence(s2.get("frame"), out2, diffs, f"{name}.verb2")
    snapshot_compare_and_drop(session, workdir, name, out2, diffs, ok_to_compare=(r2["rc"] == 0),
                               post_drop_check=True)
    return ("RED" if diffs else "PASS"), diffs


# ───────────────────────── scenarios 13-16: section 27 (session snapshots) ─────────────────────────
def scenario_13(name, session, workdir, cobra_bin, seed):
    """stash -> mutate -> unstash (default, no `keep`) round-trip: the restored
    resident must value-match the one-shot chain AT THE STASH POINT, unaffected
    by the mutation that happened while the stash was held. Also confirms the
    documented default (`keep` omitted) DROPS the stash -- a second `unstash` of
    the same name must then fail with exit 3."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 13)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v1 = Verb("eval", ["--gen", "p1 = x1 + 5"])       # establishes the state to be preserved
    v2 = Verb("eval", ["--gen", "p2 = p1 * 3"])        # the mutation the stash must survive

    s1 = session.frame_run([v1.subcmd] + v1.flags)
    expect_exit(s1, 0, f"{name}.v1", diffs)
    out1 = workdir / f"{name}_out1.hbin"
    r1 = one_shot_call(cobra_bin, v1, fx, out1)
    if not expect_exit(r1, 0, f"{name}.reference-v1", diffs):
        return "RED", diffs
    compare_verb(f"{name}.v1", r1, s1, diffs)
    if r1["rc"] == 0:
        check_run_frame_coherence(s1.get("frame"), out1, diffs, f"{name}.v1")

    stash_key = "rt13"
    st = session.frame_stash(stash_key)
    gt_rows1, gt_names1 = ground_truth_shape(out1)
    check_stash_like_frame(st.get("frame"), gt_rows1, gt_names1, "name", stash_key, diffs, f"{name}.stash")
    expect_exit(st, 0, f"{name}.stash", diffs)

    s2 = session.frame_run([v2.subcmd] + v2.flags)
    expect_exit(s2, 0, f"{name}.v2-post-stash", diffs)
    out2 = workdir / f"{name}_out2.hbin"
    r2 = one_shot_call(cobra_bin, v2, out1, out2)
    if r2["rc"] == 0:
        compare_verb(f"{name}.v2-post-stash", r2, s2, diffs)
        check_run_frame_coherence(s2.get("frame"), out2, diffs, f"{name}.v2-post-stash")

    un = session.frame_unstash(stash_key)  # keep omitted -> default drop
    expect_exit(un, 0, f"{name}.unstash", diffs)
    check_use_frame_meta(un.get("frame"), gt_rows1, gt_names1, diffs, f"{name}.unstash")
    snapshot_and_compare(session, workdir, f"{name}_post_unstash", out1, diffs)

    un_again = session.frame_unstash(stash_key)
    expect_exit(un_again, 3, f"{name}.unstash-consumed-by-default", diffs)

    snapshot_compare_and_drop(session, workdir, name, out1, diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_14(name, session, workdir, cobra_bin, seed):
    """save_virtual -> further mutate -> use(virtual path) round-trip: the
    restored resident must value-match the one-shot chain saved-to-disk AT THE
    SAVE POINT (never written to the virtual path itself -- confirmed live, the
    path never appears on disk). Also confirms `use` of a virtual path does NOT
    consume the entry (unlike `unstash`'s default) -- a second `use` of the same
    virtual path, after yet another intervening mutation, must restore the same
    save-point state again."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 14)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v1 = Verb("eval", ["--gen", "q1 = x2 - 1"])
    v2 = Verb("eval", ["--gen", "q2 = q1 * 2"])
    v3 = Verb("eval", ["--gen", "q3 = q1 + 9"])

    s1 = session.frame_run([v1.subcmd] + v1.flags)
    expect_exit(s1, 0, f"{name}.v1", diffs)
    out1 = workdir / f"{name}_out1.hbin"
    r1 = one_shot_call(cobra_bin, v1, fx, out1)
    if not expect_exit(r1, 0, f"{name}.reference-v1", diffs):
        return "RED", diffs
    compare_verb(f"{name}.v1", r1, s1, diffs)

    # Never actually written -- registered only in the session's in-RAM virtual-file
    # table (Stata tempfile). The path is intentionally a name under `workdir` that
    # this scenario never asks anything else to create.
    vpath = workdir / f"{name}_virtual_never_on_disk.hbin"
    gt_rows1, gt_names1 = ground_truth_shape(out1)
    sv = session.frame_save_virtual(vpath)
    check_stash_like_frame(sv.get("frame"), gt_rows1, gt_names1, "path", str(vpath), diffs, f"{name}.save_virtual")
    expect_exit(sv, 0, f"{name}.save_virtual", diffs)
    if vpath.exists():
        diffs.append(f"{name}: save_virtual wrote a real file at {vpath} -- must be RAM-only per section 27")

    s2 = session.frame_run([v2.subcmd] + v2.flags)
    expect_exit(s2, 0, f"{name}.v2-post-save_virtual", diffs)
    out2 = workdir / f"{name}_out2.hbin"
    r2 = one_shot_call(cobra_bin, v2, out1, out2)
    if r2["rc"] == 0:
        compare_verb(f"{name}.v2-post-save_virtual", r2, s2, diffs)

    u1 = session.frame_use(vpath)
    expect_exit(u1, 0, f"{name}.use-virtual-1st", diffs)
    check_use_frame_meta(u1.get("frame"), gt_rows1, gt_names1, diffs, f"{name}.use-virtual-1st")
    snapshot_and_compare(session, workdir, f"{name}_post_use1", out1, diffs)

    # Mutate the just-restored resident, then use() the SAME virtual path again --
    # must still restore the save-point state (non-consumption + CoW isolation).
    s3 = session.frame_run([v3.subcmd] + v3.flags)
    expect_exit(s3, 0, f"{name}.v3-post-restore", diffs)
    out3 = workdir / f"{name}_out3.hbin"
    r3 = one_shot_call(cobra_bin, v3, out1, out3)
    if r3["rc"] == 0:
        compare_verb(f"{name}.v3-post-restore", r3, s3, diffs)

    u2 = session.frame_use(vpath)
    expect_exit(u2, 0, f"{name}.use-virtual-2nd", diffs)
    check_use_frame_meta(u2.get("frame"), gt_rows1, gt_names1, diffs, f"{name}.use-virtual-2nd")
    snapshot_and_compare(session, workdir, f"{name}_post_use2", out1, diffs)

    snapshot_compare_and_drop(session, workdir, name, out1, diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_15(name, session, workdir, cobra_bin, seed):
    """stash with `keep:true` -> unstash twice: the first `unstash(keep=true)`
    must retain the stash (Stata `restore, preserve`) so a SECOND `unstash` of
    the same name still succeeds, restoring the identical stash-point state even
    though the resident was mutated differently in between each restore. A
    THIRD `unstash` (default, no `keep`) must then fail -- the second call
    consumed it."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 15)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v1 = Verb("eval", ["--gen", "k1 = x1 - x2"])
    v2 = Verb("eval", ["--gen", "k2 = k1 * 2"])
    v3 = Verb("eval", ["--gen", "k3 = k1 + 9"])

    s1 = session.frame_run([v1.subcmd] + v1.flags)
    expect_exit(s1, 0, f"{name}.v1", diffs)
    out1 = workdir / f"{name}_out1.hbin"
    r1 = one_shot_call(cobra_bin, v1, fx, out1)
    if not expect_exit(r1, 0, f"{name}.reference-v1", diffs):
        return "RED", diffs
    gt_rows1, gt_names1 = ground_truth_shape(out1)

    stash_key = "keep15"
    st = session.frame_stash(stash_key)
    expect_exit(st, 0, f"{name}.stash", diffs)

    s2 = session.frame_run([v2.subcmd] + v2.flags)
    expect_exit(s2, 0, f"{name}.v2-post-stash", diffs)
    out2 = workdir / f"{name}_out2.hbin"
    one_shot_call(cobra_bin, v2, out1, out2)

    un1 = session.frame_unstash(stash_key, keep=True)
    expect_exit(un1, 0, f"{name}.unstash-1st-keep", diffs)
    check_use_frame_meta(un1.get("frame"), gt_rows1, gt_names1, diffs, f"{name}.unstash-1st-keep")
    snapshot_and_compare(session, workdir, f"{name}_post_unstash1", out1, diffs)

    s3 = session.frame_run([v3.subcmd] + v3.flags)
    expect_exit(s3, 0, f"{name}.v3-post-1st-restore", diffs)
    out3 = workdir / f"{name}_out3.hbin"
    one_shot_call(cobra_bin, v3, out1, out3)

    un2 = session.frame_unstash(stash_key)  # keep omitted -> default drop this time
    expect_exit(un2, 0, f"{name}.unstash-2nd-default", diffs)
    check_use_frame_meta(un2.get("frame"), gt_rows1, gt_names1, diffs, f"{name}.unstash-2nd-default")
    # Must still be the ORIGINAL stash point (out1), not out3 -- proves the kept
    # stash was untouched by v3's mutation of the restored copy.
    snapshot_and_compare(session, workdir, f"{name}_post_unstash2", out1, diffs)

    un3 = session.frame_unstash(stash_key)
    expect_exit(un3, 3, f"{name}.unstash-3rd-consumed", diffs)

    snapshot_compare_and_drop(session, workdir, name, out1, diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_16(name, session, workdir, cobra_bin, seed):
    """Section-27 error paths, each followed by a recovery verb proving state was
    left untouched: (1) unknown stash name -> exit 3; (2) `unstash` with no
    resident AND no stash table entry at all (right after a `drop`, before any
    subsequent `use`) -> exit 3, plus `stash`/`save_virtual` with no resident ->
    exit 3 each; (3) a `frame` object missing the required "name"/"path" key on
    stash/unstash/save_virtual -> exit 2. Every error is a malformed-request-
    style rejection (loop continues, no resident state is disturbed), verified
    each time by chaining one more real verb and comparing it to the one-shot
    reference rooted at whatever the CURRENT head is."""
    fx = workdir / f"{name}.hbin"
    make_fixture(fx, seed=seed + 16)
    diffs = []
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    v0 = Verb("eval", ["--gen", "e0 = x1 + 1"])
    expect_exit(session.frame_run([v0.subcmd] + v0.flags), 0, f"{name}.v0-setup", diffs)
    head = workdir / f"{name}_head0.hbin"
    if not expect_exit(one_shot_call(cobra_bin, v0, fx, head), 0, f"{name}.reference-v0", diffs):
        return "RED", diffs

    def recover(tag, head_path, k):
        """Run one more verb on the session, compare it to the one-shot reference
        rooted at `head_path` (proving the preceding error left state untouched),
        and return the new head path. References `x1` (present in every
        make_fixture-generated frame, unlike `e0` which only exists on `fx`'s own
        chain -- `head_path` may instead be `fx2`, a fresh fixture reloaded after
        the mid-scenario `drop`)."""
        vr = Verb("eval", ["--gen", f"rec{k} = x1 + {k}"])
        sr = session.frame_run([vr.subcmd] + vr.flags)
        expect_exit(sr, 0, f"{name}.{tag}-recovery", diffs)
        new_head = workdir / f"{name}_head{k}.hbin"
        rr = one_shot_call(cobra_bin, vr, head_path, new_head)
        if rr["rc"] == 0:
            compare_verb(f"{name}.{tag}-recovery", rr, sr, diffs)
            check_run_frame_coherence(sr.get("frame"), new_head, diffs, f"{name}.{tag}-recovery")
        return new_head

    # (1) unknown stash name, resident present.
    un_unknown = session.frame_unstash("totally_unknown_key_999")
    expect_exit(un_unknown, 3, f"{name}.unknown-stash", diffs)
    head = recover("unknown-stash", head, 1)

    # (2) drop -> no resident AND registries cleared -> "unstash before use", plus
    # stash/save_virtual with no resident. All three per contract_A section 27
    # text are exit 3 (a resident-independent "no stash named" for unstash; an
    # explicit "no resident" for stash/save_virtual).
    dr = session.frame_drop()
    expect_exit(dr, 0, f"{name}.drop-for-error-probe", diffs)
    un_before_use = session.frame_unstash("anything_at_all")
    expect_exit(un_before_use, 3, f"{name}.unstash-before-use", diffs)
    st_no_resident = session.frame_stash("no_resident_stash")
    expect_exit(st_no_resident, 3, f"{name}.stash-no-resident", diffs)
    sv_no_resident = session.frame_save_virtual(workdir / f"{name}_no_resident.hbin")
    expect_exit(sv_no_resident, 3, f"{name}.save_virtual-no-resident", diffs)

    fx2 = workdir / f"{name}_fx2.hbin"
    make_fixture(fx2, seed=seed + 160)
    expect_exit(session.frame_use(fx2), 0, f"{name}.use-fx2-after-drop", diffs)
    head = recover("after-drop-reload", fx2, 2)

    # (3) missing required "name"/"path" -> exit 2 (resident present, from the
    # recovery above). Uses the raw `_send_frame` transport directly (the public
    # frame_stash/frame_unstash/frame_save_virtual wrappers always supply the
    # key) to construct a frame object that omits it entirely.
    missing_name_stash = session._send_frame({"op": "stash"}, 30.0)
    expect_exit(missing_name_stash, 2, f"{name}.stash-missing-name", diffs)
    missing_name_unstash = session._send_frame({"op": "unstash"}, 30.0)
    expect_exit(missing_name_unstash, 2, f"{name}.unstash-missing-name", diffs)
    missing_path_sv = session._send_frame({"op": "save_virtual"}, 30.0)
    expect_exit(missing_path_sv, 2, f"{name}.save_virtual-missing-path", diffs)
    head = recover("missing-key-errors", head, 3)

    snapshot_compare_and_drop(session, workdir, name, head, diffs)
    return ("RED" if diffs else "PASS"), diffs


# ───────── scenarios 17-20: contract_A section 31, session estimation store ─────────
# Authored by the Eval Harness operator as the standing coverage for section 31
# (ruling 2026-07-21, messenger/eval-harness/log.md). The engine change ships a
# `fits` counter precisely so a gate can assert on it; COMMS section 4 item 11
# requires that assertion to EXIST in a suite, not to have been run once by hand.
# The four legs below are that assertion.
#
# Why a value pin alone is not coverage here (item 11's whole point): a resurrected
# re-fit produces the SAME numbers, just slowly. Only two things distinguish a reuse
# from a re-fit from outside the engine --
#   (a) `est.fits`, the engine's own count of post-estimation fits (s17), and
#   (b) answering from data that no longer exists in the session (s18).
# s19 pins the BS-098/BS-013 vcov guarantee structurally rather than by discipline,
# and s20 pins the honest-error surface, including the one path (a model-spec flag
# arriving WITH a handle) that nothing asserted before this leg.

LINCOM_MODEL = ["--y", "x1", "--x", "x2,cat"]  # frame-run argv omits --data (section 26)

# The engine spells a one-way cluster vcov as `cluster(<varname>)` (and a twoway as
# `twoway(<a>,<b>)`) -- src/vcov/vcov.cpp:126. That convention lives in the shared
# vcov layer, so it is common to every estimator and PREDATES section 31 entirely;
# confirmed against both the section-31 build and the pinned 0.33.0 engine, 2026-07-21.
#
# Pinned EXACTLY rather than by a "cluster" prefix test, deliberately. The
# discriminator s19 needs is "not iid", which a prefix match would give -- but a
# prefix match would ALSO accept a bare `cluster` that had silently dropped the
# variable name, and stating WHICH covariance answered is precisely the promise
# section 31 makes for this field. The exact string is fixture-coupled (it names
# make_fixture's `grp`), which is correct: the fixture and the expectation live in
# this same file and move together.
CLUSTER_VCOV_TYPE = "cluster(grp)"


def _mint(session, argv, ctx, diffs):
    """Frame-run a post-estimation verb from a FULL model specification (the fitting
    entry form), which publishes the fit and mints a handle. Returns (rec, est)."""
    rec = session.frame_run(argv)
    if not expect_exit(rec, 0, ctx, diffs):
        return rec, None
    est = est_of(rec, ctx, diffs)
    if est is None:
        return rec, None
    handle = est.get("handle")
    if not isinstance(handle, str) or not handle:
        diffs.append(f"{ctx}: est.handle is not a non-empty string: {handle!r}")
        return rec, None
    for key in ("k", "n_obs", "vcov_type", "fits"):
        if key not in est:
            diffs.append(f"{ctx}: est is missing required member '{key}' "
                          f"(contract_A section 31 mint body): {est!r}")
    return rec, est


def scenario_17(name, session, workdir, cobra_bin, seed):
    """s17 -- `est.fits` does not climb across handle-addressed post-estimation.

    THE MECHANISM GATE (COMMS section 4 item 11). One frame-run `lincom` from the
    full specification fits once and mints; three subsequent `postest` calls against
    that handle must each report the IDENTICAL `fits`. If the re-fit ever comes back
    -- a reverted short-circuit, a lost `serve_active_model()` wire, a refactor that
    drops the stored-model branch -- `fits` climbs and this leg goes RED while every
    value pin in the tree stays green.

    GATE-NESS PROOF (item 11's corollary -- prove the gate by breaking the mechanism):
    make `serve_active_model()` in apps/cecon_cli.cpp return `nullptr` unconditionally.
    The handle path then falls through to the fitting branch, `fits` climbs 1->4, and
    ONLY this leg fails. (`data_missing()` is false on the postest path, since
    `serve_resident_active()` is false there, so a nulled bridge surfaces as a usage
    error rather than a wrong answer -- either way the assertion below is what moves.)
    """
    diffs = []
    fx = workdir / f"{name}_fx.hbin"
    make_fixture(fx, seed=seed + 170)
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    mint, est = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"],
                      f"{name}.mint", diffs)
    if est is None:
        return "RED", diffs
    handle = est["handle"]
    fits0 = est.get("fits")
    if not isinstance(fits0, int):
        diffs.append(f"{name}.mint: est.fits is {fits0!r}, expected an integer")
        return "RED", diffs
    mint_b, mint_se, mint_df = lincom_b_se_df(mint, f"{name}.mint", diffs)

    # Three combinations through the handle. Each must (a) hold `fits` flat, (b) echo
    # the handle, (c) carry NO `frame` member -- section 31: no data was read and the
    # resident is untouched, so a frame body would misreport what happened.
    for i, expr in enumerate(("x2", "x2 + cat", "2*x2 - cat"), start=1):
        ctx = f"{name}.postest{i}"
        rec = session.frame_postest(handle, ["lincom", "--exp", expr])
        if not expect_exit(rec, 0, ctx, diffs):
            continue
        pest = est_of(rec, ctx, diffs)
        if pest is None:
            continue
        if pest.get("fits") != fits0:
            diffs.append(
                f"{ctx}: est.fits={pest.get('fits')!r} but the minting call reported {fits0!r} -- "
                f"the post-estimation RE-FIT is back (contract_A section 31: one mint followed by "
                f"N postest calls must report the SAME fits on all N+1 responses)")
        if pest.get("handle") != handle:
            diffs.append(f"{ctx}: est.handle={pest.get('handle')!r} not echoed "
                          f"(expected the handle that was spent, {handle!r})")
        if rec.get("has_frame_member"):
            diffs.append(f"{ctx}: response carries a 'frame' member; section 31 requires none "
                          f"on a postest response (no data was read, resident untouched)")

    # Value-identity: the SAME combination through the handle must reproduce the
    # minting call's own numbers exactly. Not a substitute for the fits assertion --
    # it is the half that would stay green if the optimization died.
    same = session.frame_postest(handle, ["lincom", "--exp", "x2"])
    if expect_exit(same, 0, f"{name}.identity", diffs):
        b, se, df = lincom_b_se_df(same, f"{name}.identity", diffs)
        for label, got, want in (("b", b, mint_b), ("se", se, mint_se), ("df", df, mint_df)):
            if got is None or want is None:
                diffs.append(f"{name}.identity: {label} unavailable on one side "
                              f"(handle={got!r}, mint={want!r})")
            elif got != want:
                diffs.append(f"{name}.identity: {label} through the handle = {got!r} but the "
                              f"minting call produced {want!r} (section 31 requires byte-identical "
                              f"stdout -- both entry forms share one arithmetic tail)")

    expect_exit(session.frame_drop(), 0, f"{name}.drop", diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_18(name, session, workdir, cobra_bin, seed):
    """s18 -- CHANGED DATA: a handle answers from the model it was minted for, not
    from whatever the session is holding now.

    The assertion a resurrected re-fit cannot survive. Mint against fixture A, swap
    the resident to fixture B (same schema, different values -- section 31: `use`
    does NOT clear the store, deliberately, matching Stata where `lincom` after
    `regress` reads the stored e(b)/e(V) and does not care that the data moved),
    then spend the A handle. A stored-model answer returns A's coefficient. A re-fit
    returns B's. `fits` cannot distinguish these two if the re-fit is what MINTS, so
    this leg is not redundant with s17 -- it is the one that survives a re-fit hidden
    behind a correctly-incrementing counter.

    POWER CHECK: the leg first proves A and B actually disagree. A fixture that
    happened to produce the same coefficient would make this scenario vacuous while
    reporting PASS, so the discriminator is itself asserted before it is relied on.
    """
    diffs = []
    fx_a = workdir / f"{name}_A.hbin"
    fx_b = workdir / f"{name}_B.hbin"
    make_fixture(fx_a, seed=seed + 180)
    # B: same column names/types, different values. `x1` (the depvar) is tripled
    # while the regressors are redrawn, so the fitted slope MUST move -- scaling
    # every column uniformly would leave the slope invariant (cov/var both scale)
    # and silently defeat the test.
    rng = np.random.default_rng(seed + 181)
    n = 60
    df_b = pd.DataFrame({
        "id": np.arange(1, n + 1, dtype=float),
        "x1": rng.normal(10, 2, n) * 3.0,
        "x2": rng.normal(0, 1, n),
        "cat": rng.integers(1, 5, n).astype(float),
        "grp": rng.integers(1, 4, n).astype(float),
        "g": rng.choice(["A", "B", "C"], n),
        "d": rng.integers(0, 2, n).astype(float),
    })
    write_hbin(df_b, fx_b, factor_cols=("g",))

    if not expect_exit(session.frame_use(fx_a), 0, f"{name}.use-A", diffs):
        return "RED", diffs
    mint, est = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"],
                      f"{name}.mint-on-A", diffs)
    if est is None:
        return "RED", diffs
    handle = est["handle"]
    a_b, a_se, _ = lincom_b_se_df(mint, f"{name}.mint-on-A", diffs)

    # Swap the resident to B and fit there too -- this both establishes B's answer
    # and proves the swap really took effect.
    if not expect_exit(session.frame_use(fx_b), 0, f"{name}.use-B", diffs):
        return "RED", diffs
    b_rec, b_est = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"],
                         f"{name}.fit-on-B", diffs)
    b_b, b_se, _ = lincom_b_se_df(b_rec, f"{name}.fit-on-B", diffs)

    if a_b is None or b_b is None:
        diffs.append(f"{name}: could not read both coefficients (A={a_b!r}, B={b_b!r}) -- "
                      f"the changed-data discriminator cannot be evaluated")
        return "RED", diffs
    if a_b == b_b:
        diffs.append(f"{name}: POWER CHECK FAILED -- fixtures A and B produce the identical "
                      f"coefficient {a_b!r}, so this scenario cannot distinguish a stored-model "
                      f"answer from a re-fit. Fix the fixture, do not relax the assertion.")
        return "RED", diffs

    # The assertion. Spend A's handle while B is resident.
    spent = session.frame_postest(handle, ["lincom", "--exp", "x2"])
    if expect_exit(spent, 0, f"{name}.spend-A-handle-while-B-resident", diffs):
        got_b, got_se, _ = lincom_b_se_df(spent, f"{name}.spend-A-handle", diffs)
        if got_b != a_b:
            which = "fixture B's (A RE-FIT AGAINST THE CURRENT RESIDENT)" if got_b == b_b else "neither fixture's"
            diffs.append(
                f"{name}: handle minted on A returned b={got_b!r} -- that is {which}. Expected A's "
                f"{a_b!r}. contract_A section 31: a stored model is a self-contained (names, beta, V) "
                f"tuple that references no data; a later `use` must not change what it answers.")
        if got_se != a_se:
            diffs.append(f"{name}: handle minted on A returned se={got_se!r}, expected A's {a_se!r}")

    # B's own handle must still be independently valid, and `fits` must not have
    # climbed on the A-handle call above (B's mint is the only fit since).
    if b_est is not None:
        after = session.frame_postest(b_est["handle"], ["lincom", "--exp", "x2"])
        if expect_exit(after, 0, f"{name}.spend-B-handle", diffs):
            aft = est_of(after, f"{name}.spend-B-handle", diffs)
            if aft is not None and aft.get("fits") != b_est.get("fits"):
                diffs.append(f"{name}.spend-B-handle: est.fits={aft.get('fits')!r} but B's mint "
                              f"reported {b_est.get('fits')!r} -- a re-fit happened between them")

    expect_exit(session.frame_drop(), 0, f"{name}.drop", diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_19(name, session, workdir, cobra_bin, seed):
    """s19 -- BS-098 / BS-013: a handle keeps the FITTED model's covariance and can
    never fall back to cobra's `iid` default.

    The hazard is live in the specification entry form: `cmd_lincom` fits with
    `parse_vcov(opt(a,"--vcov","iid"))`, so a caller who omits `--vcov cluster` on a
    post-estimation call against a clustered fit silently receives iid SEs. The
    handle form is meant to remove the hazard STRUCTURALLY -- a postest call passes
    no `--vcov` at all, so there is no flag to forget and no default to fall back to.
    This leg pins that claim instead of trusting it.

    Both branches of the dof rule are exercised by construction: `lincom` under
    cluster takes G-1 (3 groups in make_fixture -> df 2), under iid it takes
    dof_resid (60 rows, 3 params -> df 57). A silent iid fallback therefore moves
    BOTH `se` and `df`, and the leg asserts on both -- an SE alone could in principle
    coincide, a df of 57 where 2 is required cannot be mistaken for noise.

    POWER CHECK: cluster and iid SEs are asserted to DIFFER before either is used as
    an expectation.
    """
    diffs = []
    fx = workdir / f"{name}_fx.hbin"
    make_fixture(fx, seed=seed + 190)
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    cl_argv = ["lincom"] + LINCOM_MODEL + ["--vcov", "cluster", "--cluster", "grp", "--exp", "x2"]
    mint, est = _mint(session, cl_argv, f"{name}.mint-cluster", diffs)
    if est is None:
        return "RED", diffs
    handle = est["handle"]
    if est.get("vcov_type") != CLUSTER_VCOV_TYPE:
        diffs.append(f"{name}.mint-cluster: est.vcov_type={est.get('vcov_type')!r}, expected "
                      f"{CLUSTER_VCOV_TYPE!r} -- section 31 requires the mint body to state the "
                      f"vcov AS FITTED, naming which clustering answered")
    cl_b, cl_se, cl_df = lincom_b_se_df(mint, f"{name}.mint-cluster", diffs)

    # The `est` envelope is a NEW surface; nothing else in the tree pins that it
    # agrees with the payload it claims to describe. Cross-check it against the
    # minting call's own stdout, which IS golden-pinned elsewhere.
    try:
        mint_payload = json.loads(mint.get("out") or "")
        if est.get("vcov_type") != mint_payload.get("vcov_type"):
            diffs.append(f"{name}.mint-cluster: est.vcov_type={est.get('vcov_type')!r} disagrees "
                          f"with the same response's stdout vcov_type="
                          f"{mint_payload.get('vcov_type')!r} -- the est envelope has drifted from "
                          f"the fit it describes")
    except (ValueError, TypeError):
        pass  # stdout parse failure already reported by lincom_b_se_df

    # The iid arm: same model, default vcov. This is what a fallback would produce.
    iid_rec, iid_est = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"],
                             f"{name}.fit-iid", diffs)
    iid_b, iid_se, iid_df = lincom_b_se_df(iid_rec, f"{name}.fit-iid", diffs)
    # Spelling-independent discriminator, kept alongside the exact pin above: whatever
    # the two arms are called, the clustered fit must not be labelled the same as the
    # iid one. This half survives a legitimate future change to the vcov_type spelling.
    if iid_est is not None and est.get("vcov_type") == iid_est.get("vcov_type"):
        diffs.append(f"{name}: the clustered mint and the iid mint report the SAME "
                      f"est.vcov_type ({est.get('vcov_type')!r}) -- the field cannot be "
                      f"distinguishing the fitted covariance")

    if cl_se is None or iid_se is None:
        diffs.append(f"{name}: could not read both SEs (cluster={cl_se!r}, iid={iid_se!r}) -- "
                      f"the fallback discriminator cannot be evaluated")
        return "RED", diffs
    if cl_se == iid_se and cl_df == iid_df:
        diffs.append(f"{name}: POWER CHECK FAILED -- cluster and iid produce the identical "
                      f"(se, df) = ({cl_se!r}, {cl_df!r}), so an iid fallback would be invisible "
                      f"here. Fix the fixture, do not relax the assertion.")
        return "RED", diffs

    # The assertion: spend the cluster handle, get cluster numbers.
    spent = session.frame_postest(handle, ["lincom", "--exp", "x2"])
    if expect_exit(spent, 0, f"{name}.spend-cluster-handle", diffs):
        got_b, got_se, got_df = lincom_b_se_df(spent, f"{name}.spend-cluster-handle", diffs)
        if got_se != cl_se:
            hint = " -- this is the IID value: the BS-098 fallback is live on the handle path" \
                if got_se == iid_se else ""
            diffs.append(f"{name}: handle minted under --vcov cluster returned se={got_se!r}, "
                          f"expected the fitted cluster SE {cl_se!r}{hint}")
        if got_df != cl_df:
            hint = " -- this is the IID dof_resid: the cluster (G-1) branch was not reproduced" \
                if got_df == iid_df else ""
            diffs.append(f"{name}: handle minted under --vcov cluster returned df={got_df!r}, "
                          f"expected {cl_df!r}{hint}")
        if got_b != cl_b:
            diffs.append(f"{name}: handle returned b={got_b!r}, expected {cl_b!r} (the point "
                          f"estimate is vcov-independent, so this moving means the wrong model answered)")
        try:
            payload = json.loads(spent.get("out") or "")
            if payload.get("vcov_type") != CLUSTER_VCOV_TYPE:
                diffs.append(f"{name}: postest stdout reports vcov_type="
                              f"{payload.get('vcov_type')!r}, expected {CLUSTER_VCOV_TYPE!r}")
            # NOTE deliberately not asserted here: the postest `est` body carries only
            # {handle, fits} per section 31 -- vcov_type appears on the MINT body, not on
            # every postest response (cecon_cli.cpp's postest branch builds exactly those
            # two members). A cross-check against est.vcov_type here could never fire, and
            # a gate branch that cannot execute reads as coverage without being any. The
            # stdout pin above is the real assertion; the mint body is cross-checked at
            # the mint, where the field actually exists.
        except (ValueError, TypeError):
            pass  # already reported by lincom_b_se_df

    expect_exit(session.frame_drop(), 0, f"{name}.drop", diffs)
    return ("RED" if diffs else "PASS"), diffs


def scenario_20(name, session, workdir, cobra_bin, seed):
    """s20 -- the honest-error surface, including eviction.

    Section 31's whole safety argument is that a handle resolves to exactly its own
    model or to NOTHING. Every way of failing to resolve must be an explicit error
    with no stdout -- never a silent re-fit, never another model's answer. Covered:
      (a) a handle that was never minted;
      (b) a handle invalidated by `drop` (the session reset, Stata `clear`);
      (c) a model-specification flag arriving WITH a handle -- section 31 states this
          is caught by the E6 unconsumed-flag net (exit 3, bad_input, no stdout)
          because the handle path never READS those flags. `dispatch_command` routes
          through `dispatch_guard` so it should hold, but nothing asserted it before
          this leg; a future refactor that reads `--vcov` "just in case" on the handle
          path would silently let a partial specification win over the stored model;
      (d) a verb that is not a post-estimation command -> exit 2;
      (e) `--data` / `--out` in a postest argv -> exit 2;
      (f) a malformed request (missing `est`, missing `argv`) -> exit 2;
      (g) EVICTION: the capacity bound must express itself as an honest miss on the
          OLDEST handle while the NEWEST still resolves.

    On (g) -- scope boundary, per the Eval ruling of 2026-07-21. The eviction POLICY
    (oldest-first until under both the count and byte bounds; never evict the newest;
    the byte budget itself) is engine-internal state that is not observable from the
    wire, and belongs in cobra's own ctest white-box suite -- COMMS section 4 item 11
    puts internal-state assertions there by name. What belongs HERE is the
    contract-visible consequence the client depends on. The leg therefore mints
    comfortably past the documented cap and asserts BEHAVIOUR (oldest gone and honest,
    newest alive with flat `fits`) rather than pinning the constant 32 -- pinning the
    cap would turn a legitimate future capacity change into a false RED, and the cap
    is the engine's to state, not Eval's to freeze.
    """
    diffs = []
    fx = workdir / f"{name}_fx.hbin"
    make_fixture(fx, seed=seed + 200)
    if not expect_exit(session.frame_use(fx), 0, f"{name}.use", diffs):
        return "RED", diffs

    def expect_honest_miss(rec, ctx):
        """exit 3 + empty stdout + an explanatory stderr. The empty-stdout half is
        the load-bearing one: a silent re-fit would have PRODUCED a result."""
        if expect_exit(rec, 3, ctx, diffs):
            if rec.get("out"):
                diffs.append(f"{ctx}: exit 3 but stdout is non-empty ({str(rec.get('out'))[:120]!r}) "
                              f"-- section 31 requires no stdout on an unresolved handle")
            if "no stored model" not in (rec.get("err") or ""):
                diffs.append(f"{ctx}: stderr does not name the failure ('no stored model'): "
                              f"{rec.get('err', '')[:200]!r}")

    def expect_reject(rec, ctx, code):
        if expect_exit(rec, code, ctx, diffs) and rec.get("out"):
            diffs.append(f"{ctx}: rejected with exit {code} but stdout is non-empty "
                          f"({str(rec.get('out'))[:120]!r})")

    # (a) never minted.
    expect_honest_miss(session.frame_postest("est:ffffffff:999999", ["lincom", "--exp", "x2"]),
                       f"{name}.unknown-handle")

    # Mint a real one for (b)-(e).
    _, est = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"], f"{name}.mint", diffs)
    if est is None:
        return "RED", diffs
    handle = est["handle"]

    # (c) a model-spec flag alongside the handle -> E6 bad_input, exit 3, no stdout.
    for flag, val in (("--vcov", "cluster"), ("--y", "x1"), ("--x", "x2,cat"), ("--keepif", "x2>0")):
        rec = session.frame_postest(handle, ["lincom", "--exp", "x2", flag, val])
        ctx = f"{name}.spec-flag{flag}"
        if expect_exit(rec, 3, ctx, diffs):
            if rec.get("out"):
                diffs.append(f"{ctx}: exit 3 but stdout is non-empty -- a handle-addressed call "
                              f"carrying {flag} must produce NO result; section 31 forbids one of "
                              f"the handle and the partial specification silently winning")
        else:
            diffs.append(f"{ctx}: {flag} passed alongside a handle was NOT rejected -- if the "
                          f"handle path now reads {flag}, the stored model can be silently overridden")

    # (d) a non-post-estimation verb.
    expect_reject(session.frame_postest(handle, ["summarize", "--vars", "x1"]),
                  f"{name}.wrong-verb", 2)
    # (e) --data / --out are the stored model's inputs/outputs; both rejected up front.
    expect_reject(session.frame_postest(handle, ["lincom", "--exp", "x2", "--data", str(fx)]),
                  f"{name}.postest-with-data", 2)
    expect_reject(session.frame_postest(handle, ["lincom", "--exp", "x2", "--out",
                                                 str(workdir / f"{name}_no.json")]),
                  f"{name}.postest-with-out", 2)
    # (f) malformed requests -- raw transport, since frame_postest always supplies both keys.
    expect_reject(session._send_frame({"op": "postest", "argv": ["lincom", "--exp", "x2"]}, 30.0),
                  f"{name}.postest-missing-est", 2)
    expect_reject(session._send_frame({"op": "postest", "est": handle}, 30.0),
                  f"{name}.postest-missing-argv", 2)
    expect_reject(session._send_frame({"op": "postest", "est": handle, "argv": []}, 30.0),
                  f"{name}.postest-empty-argv", 2)

    # The handle must still be good after all those rejections -- an error path that
    # quietly consumed or corrupted the store would be its own defect.
    still = session.frame_postest(handle, ["lincom", "--exp", "x2"])
    expect_exit(still, 0, f"{name}.handle-survives-rejections", diffs)

    # (g) EVICTION. Mint well past the documented cap (32) so the assertion does not
    # depend on the exact constant, then require: oldest -> honest miss, newest ->
    # alive AND still not re-fitting.
    first_handle = handle
    last_est = None
    for i in range(40):
        _, e = _mint(session, ["lincom"] + LINCOM_MODEL + ["--exp", "x2"],
                     f"{name}.evict-mint{i}", diffs)
        if e is None:
            return "RED", diffs
        last_est = e
    expect_honest_miss(session.frame_postest(first_handle, ["lincom", "--exp", "x2"]),
                       f"{name}.evicted-oldest")
    newest = session.frame_postest(last_est["handle"], ["lincom", "--exp", "x2"])
    if expect_exit(newest, 0, f"{name}.newest-survives-eviction", diffs):
        nest = est_of(newest, f"{name}.newest-survives-eviction", diffs)
        if nest is not None and nest.get("fits") != last_est.get("fits"):
            diffs.append(f"{name}.newest-survives-eviction: est.fits={nest.get('fits')!r} but its "
                          f"mint reported {last_est.get('fits')!r} -- the newest handle resolved by "
                          f"RE-FITTING rather than from the store")

    # (b) drop clears the store; the surviving handle must then miss honestly.
    surviving = last_est["handle"]
    expect_exit(session.frame_drop(), 0, f"{name}.drop", diffs)
    expect_honest_miss(session.frame_postest(surviving, ["lincom", "--exp", "x2"]),
                       f"{name}.handle-after-drop")
    return ("RED" if diffs else "PASS"), diffs


SCENARIOS = [
    ("s01_gen_chain", scenario_1),
    ("s02_replace_recode", scenario_2),
    ("s03_egen_mean_rank", scenario_3),
    ("s04_sort_then_gen_order_dependence", scenario_4),
    ("s05_drop_keepif", scenario_5),
    ("s06_merge_1to1_using", scenario_6),
    ("s07_collapse_mean_by_group", scenario_7),
    ("s08_regress_and_summarize", scenario_8),
    ("s09_logit_after_keepif", scenario_9),
    ("s10_error_mid_chain_state_unadvanced", scenario_10),
    ("s11_data_in_run_rejection", scenario_11),
    ("s12_interleaved_stateless_request", scenario_12),
    ("s13_stash_unstash_roundtrip", scenario_13),
    ("s14_save_virtual_use_roundtrip", scenario_14),
    ("s15_stash_keep_unstash_twice", scenario_15),
    ("s16_stash_error_paths", scenario_16),
    ("s17_postest_fits_flat", scenario_17),
    ("s18_postest_changed_data", scenario_18),
    ("s19_postest_vcov_cluster", scenario_19),
    ("s20_postest_honest_errors", scenario_20),
]

# Scenarios that need contract_A section 27 (stash/unstash/save_virtual) -- these
# SKIP individually (leaving s01-s12 comparable as before) when section 26 IS
# available but section 27 is not (e.g. the 0.29.0 release engine, which has
# section 26 but predates the stash/unstash/save_virtual ops). If section 26
# itself is unavailable, every scenario SKIPs together via `frame_reason`, same
# as before this leg existed.
STASH_SCENARIO_NAMES = frozenset({
    "s13_stash_unstash_roundtrip", "s14_save_virtual_use_roundtrip",
    "s15_stash_keep_unstash_twice", "s16_stash_error_paths",
})

# Same convention one section later: s17-s20 need contract_A section 31 (the session
# estimation store). A binary with sections 26/27 but without 31 -- every engine
# before the OVERHEAD-ZERO S3 change, including the pinned reference engine -- SKIPs
# these four and runs s01-s16 normally. That SKIP is now a hard exit-1 by
# default (FIX-SERVE-REQUIRE); `--allow-no-postest` opts back into the SKIP,
# which is what stops the skip from becoming permanent.
POSTEST_SCENARIO_NAMES = frozenset({
    "s17_postest_fits_flat", "s18_postest_changed_data",
    "s19_postest_vcov_cluster", "s20_postest_honest_errors",
})


# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cobra", default=str(SP.RE.COBRA), help="path to cobra binary")
    ap.add_argument("--only", default="", help="comma list of scenario name substrings to run")
    ap.add_argument("--limit", type=int, default=0, help="cap to the first N scenarios (smoke)")
    ap.add_argument("--scratch", default="", help="keep fixtures/snapshots in this dir (default: temp, cleaned up)")
    ap.add_argument("--allow-no-frame", action="store_true",
                     help="opt-in: exit 0 (graceful SKIP) when section-26 frame mode is "
                          "unavailable, instead of the default hard RED")
    ap.add_argument("--allow-no-stash", action="store_true",
                     help="opt-in: exit 0 (graceful SKIP of s13-s16 only) when section-27 "
                          "(stash/unstash/save_virtual) is unavailable, instead of the default "
                          "hard RED (implies section 26 IS available -- if section 26 itself is "
                          "missing, --allow-no-frame is the relevant flag)")
    ap.add_argument("--allow-no-postest", action="store_true",
                     help="opt-in: exit 0 (graceful SKIP of s17-s20 only) when contract_A "
                          "section 31 (the postest op / est handle) is unavailable, instead of "
                          "the default hard RED (implies section 26 IS available)")
    ap.add_argument("--seed", type=int, default=20260718, help="base RNG seed for fixture generation")
    ap.add_argument("--handshake-timeout", type=float, default=10.0)
    ap.add_argument("--json", default="", help="optional path to write a full JSON verdict dump")
    args = ap.parse_args()

    cobra_bin = Path(args.cobra)
    if not cobra_bin.exists():
        print(f"ERROR: cobra binary not found: {cobra_bin}", file=sys.stderr)
        sys.exit(2)

    scenarios = SCENARIOS
    if args.only:
        wants = args.only.split(",")
        scenarios = [(n, f) for n, f in scenarios if any(w in n for w in wants)]
    if args.limit:
        scenarios = scenarios[:args.limit]
    if not scenarios:
        print("ERROR: no scenarios selected (check --only/--limit)", file=sys.stderr)
        sys.exit(2)

    keep_scratch = bool(args.scratch)
    tmp_ctx = None
    if keep_scratch:
        workdir = Path(args.scratch)
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        tmp_ctx = tempfile.TemporaryDirectory(prefix="serve_session_")
        workdir = Path(tmp_ctx.name)

    print(f"serve-session: {len(scenarios)} scenario(s) -- section-26 RESIDENT FRAME leg")
    print(f"  binary: {cobra_bin}")
    print(f"  scratch dir: {workdir}{' (kept)' if keep_scratch else ''}")

    frame_reason = None
    stash_reason = None
    postest_reason = None
    session = None
    try:
        session = FrameSession(cobra_bin, handshake_timeout=args.handshake_timeout)
        print(f"  serve handshake: {session.handshake}")
        probe_fixture = workdir / "_probe.hbin"
        make_fixture(probe_fixture, n=5, seed=1)
        probe_frame_support(session, probe_fixture)
        print("  frame mode: AVAILABLE (contract_A section 26 answered frame-use)")
    except SP.ServeUnavailable as ex:
        frame_reason = f"serve mode unavailable: {ex}"
    except FrameUnavailable as ex:
        frame_reason = str(ex)
    if frame_reason is not None:
        print(f"  SKIP: {frame_reason}")
    elif any(n in STASH_SCENARIO_NAMES for n, _ in scenarios):
        try:
            probe_stash_support(session)
            print("  stash mode: AVAILABLE (contract_A section 27 answered frame-stash)")
        except StashUnavailable as ex:
            stash_reason = str(ex)
            print(f"  SKIP (s13-s16 only): {stash_reason}")
    if frame_reason is None and any(n in POSTEST_SCENARIO_NAMES for n, _ in scenarios):
        try:
            probe_postest_support(session)
            print("  postest mode: AVAILABLE (contract_A section 31 answered frame-postest)")
        except PostestUnavailable as ex:
            postest_reason = str(ex)
            print(f"  SKIP (s17-s20 only): {postest_reason}")

    results = []
    t0 = time.perf_counter()
    for name, fn in scenarios:
        if frame_reason is not None:
            results.append({"id": name, "verdict": "SKIP", "diffs": [frame_reason]})
            print(f"  SKIP  {name}")
            continue
        if name in STASH_SCENARIO_NAMES and stash_reason is not None:
            results.append({"id": name, "verdict": "SKIP", "diffs": [stash_reason]})
            print(f"  SKIP  {name}")
            continue
        if name in POSTEST_SCENARIO_NAMES and postest_reason is not None:
            results.append({"id": name, "verdict": "SKIP", "diffs": [postest_reason]})
            print(f"  SKIP  {name}")
            continue
        try:
            verdict, diffs = fn(name, session, workdir, cobra_bin, args.seed)
        except Exception as ex:  # noqa: BLE001 -- a scenario crashing is itself a RED finding
            verdict, diffs = "RED", [f"scenario crashed: {ex!r}"]
        results.append({"id": name, "verdict": verdict, "diffs": diffs})
        print(f"  {verdict:4s}  {name}")
        for d in diffs:
            for ln in str(d).splitlines():
                print(f"        {ln}")
    print(f"  ({time.perf_counter() - t0:.1f}s)")

    if session is not None:
        try:
            rc = session.shutdown()
            if rc != 0:
                print(f"  NOTE: serve session exited rc={rc} on shutdown (expected 0)", file=sys.stderr)
        except Exception:  # noqa: BLE001
            pass

    if tmp_ctx is not None:
        tmp_ctx.cleanup()

    n_pass = sum(1 for r in results if r["verdict"] == "PASS")
    n_red = sum(1 for r in results if r["verdict"] == "RED")
    n_skip = sum(1 for r in results if r["verdict"] == "SKIP")
    n_compared = n_pass + n_red
    print(f"\nserve-session: {n_pass}/{n_compared} identical ({n_skip} skipped)")

    if args.json:
        out = {"n": len(results), "identical": n_pass, "red": n_red, "skipped": n_skip,
               "frame_available": frame_reason is None, "frame_reason": frame_reason,
               "stash_available": frame_reason is None and stash_reason is None,
               "stash_reason": stash_reason,
               "postest_available": frame_reason is None and postest_reason is None,
               "postest_reason": postest_reason,
               "scenarios": results}
        Path(args.json).write_text(json.dumps(out, indent=2))
        print(f"  wrote {args.json}")

    if n_red > 0:
        sys.exit(1)
    if frame_reason is not None and not args.allow_no_frame:
        print("  frame mode unavailable -> exit 1 (default; pass --allow-no-frame for the "
              "old graceful-SKIP behavior)", file=sys.stderr)
        sys.exit(1)
    if frame_reason is None and stash_reason is not None and not args.allow_no_stash:
        print("  section 27 (stash) unavailable -> exit 1 (default; pass --allow-no-stash "
              "for the old graceful-SKIP behavior)", file=sys.stderr)
        sys.exit(1)
    if frame_reason is None and postest_reason is not None and not args.allow_no_postest:
        print("  section 31 (postest) unavailable -> exit 1 (default; pass "
              "--allow-no-postest for the old graceful-SKIP behavior)", file=sys.stderr)
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
