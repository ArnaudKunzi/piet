# PIET SPEC — technical codebook (규약집)

**Status:** living document. Seeded 2026-06-22.
**Derives from:** [DOCTRINE.md](DOCTRINE.md). Every entry here serves a gate; if an entry ever
contradicts a gate, the gate wins and the entry is wrong.
**Companion docs:** [ROADMAP.md](ROADMAP.md) (what ships when), `CONTRACT.md` (internal)
(the engine↔backend↔frontend boundaries).

This is where the *technical detail* lives so the doctrine can stay short. Each section is
tagged with the gate it implements. Items marked **TBD** are open decisions.

---

## Ⅰ · Engine-driven — technical rules

**I-1. Biconditional command coverage.** Engine can X ⟺ PIET exposes X. Maintain a registry
mapping every cobra verb ↔ its PIET surface; an audit flags drift in either direction (engine
verb with no PIET path = missing exposure; PIET command with no engine backing = suspect).

**I-2. Translation is bit-identical.** The translate layer must emit argv that makes cobra
return exactly what a native invocation would. Every estimator carries a golden round-trip test
(coeftable **and** scalars SE/R²/adj-R²/RMSE/N/F — see the Eval goldens extension). PIET never
re-derives a number cobra already computes.

**I-3. Honest errors, never silent coercion.** Unsupported input → an explicit error block
(code + token + message + do-stop line). Forbidden anti-patterns: silently downgrading a
request (e.g. `predict, cooksd` → `--xb`), returning empty stats with `_rc=0`, dropping an
option the user wrote. If PIET can't, it says so.

**I-4. No mock in a shipped build.** Fixtures / `useMock` are a development tool only. A
released `.app` that serves mock fixtures is a doctrine violation. The kernel switch and Eval's
real-UI verification (Constitution §11.C) guard this.

**I-5. Legibility for the audience.** Render the engine's JSON into tables and labels a student
or researcher reads at a glance — value labels shown, sensible formatting, Stata-shaped output.
Faithful ≠ raw; faithful = the same number, made readable.

**I-6. The data-op debt — REPAID (2026-07-03, cobra 0.18.1 / PIET 2.0 M3b).** `merge / reshape /
append / sort / gsort / order / recode / duplicates` + `.dta` read are now **cobra engine verbs**
(contract_A §22), inside the golden net. Residue: row-wise **string** gen/replace lives PIET-side
(`piet/native/src/strmutate.cpp`, CSV-round-trip write via `cobra convert --factor`) until the
engine string dtype lands (2.x campaign WP-10/11) — it is differential-tested against the Python
oracle, but flag it in any parity audit of string-typed mutation.

**I-7. Mock fidelity** *(Constitution Part 1; RC2 of the 2.0-GA postmortem)*. A mock of a
vendored/system boundary (webview bridge, OS dialogs, cobra subprocess) is pinned to an **observed
trace** of the real thing — never inferred from our own calling code. When a boundary bug is found,
the mock is corrected in the same commit. Selftest transport mocks resolve **parsed objects**, not
JSON strings (the double-parse that killed 2.0.0's UI passed its own mock).

**I-8. Cutover sweep** *(RC4)*. Any "swap X, keep Y" milestone exits with a dead-idiom grep of the
*untouched* side for the old platform's API surface (`pywebview`, `window.prompt/alert/confirm`,
`127.0.0.1:7331`, `?kernel=`, `baseUrl` fetch …) — findings rewired or deleted, never left.

**I-9. Packaged-app journey gate** *(RC1/RC3)*. Every release touching UI/packaging/transport —
any grade — passes a scripted user journey in the REAL packaged app (`--ui-smoke`: bridge up →
varlist round-trip → command → block rendered → kernel-dot GREEN; until it exists, the User
launch-checklist). Headless PASS alone never declares a UI release GREEN.

---

## Ⅱ · Lightness — technical rules

**II-0. The measured 2.0 profile (RE-GROUNDED 2026-07-22 per the FLOOR campaign's PHASE0/AUDIT-
stage0 re-accounting — supersedes the 2026-07-09 MSG-MI-170 pass; design against THESE numbers).**
The native core is light at rest (~7.0 MB bundle, ~10 ms launch, no interpreter). `makeWorkdir`
lands scratch on local storage (`$PIET_WORKDIR`/OS-temp), never the external volume that produced
the now long-superseded "≈2.5 min per mutating command" July-3 acceptance-era mismeasurement.

**The engine is not the cost center.** Measured on the shipped artifact (real `analysis.do` slice,
1 group / 2 depvars, non-invasive `cobra serve`-protocol proxy shim, ~3 % overhead): wall 86.8 s,
engine 16.6 s = **19 % of wall** (PIET-side 81 %). Per-op share of wall: `stateless:setcol` 4.8 %
(32 calls, 130 ms mean) · **`frame:use` 4.2 %** (26 calls, 140 ms mean — this KILLS "the six
corpus re-opens are the biggest lever": it was the dominant term of the old ~19-min floor
derivation and is not dominant now) · `frame:snapshot` 3.8 % (32 calls, 104 ms mean) ·
`stateless:import-dta` 2.3 % (2 calls, 1011 ms mean, likely irreducible real data load). Source:
internal note PHASE0-reaccounting.

**Where the PIET-side 81 % goes**, symbolicated main-thread CPU (quiet machine, RelWithDebInfo
build): card rebuild `Datastore::buildVarInfo()` (`datastore.cpp:84` full deep-copy of every
factor level, per card build) **32 %** · the evaluator's string-keyed `std::map` lookups **31 %**
· engine-wait `CobraSession::readLine` 24 % (~10 min above the ~19-min engine floor over a
120-min wall = pure call-count/spawn waste, not engine work) · **`evalRows` row-wise re-parse +
re-bind body (`strmutate.cpp:380-480`) 11 %** · genuine other 2 %. Source:
internal note AUDIT-stage0 §0-3.

**Post-estimation re-fit (contract_A §31) — SHIPPED in {piet B.6.0, cobra 0.34.0}.**
`lincom`/`test`/`nlcom` used to rebuild and re-fit the FULL model per call (one depvar: 54
`lincom` calls, 22.61 s total, 419 ms each — 17 % of that depvar's wall — source:
internal note GATE23-VERB-OVERHEAD-MAP ADDENDUM); §31 answers post-estimation from the
session's stored `(b, V)` instead. End-to-end exit evidence, byte-identical answers before/after:
one real depvar 73.95 s → 43.76 s (**41 % faster**), CPU 59.29 s → 28.41 s. ≈30 s/depvar ×
160 depvars ≈ **80 min off the full `analysis.do` run.** Source:
internal note EXIT-EVIDENCE-depvar.

**Per-verb overhead, at floor vs not** (real 108,384×192 corpus, min-of-3): most registry verbs
sit at **84–88 % engine share** (`gen`/`replace`/`gsort`/`reghdfe`/`summarize`/`egen`/`sort` —
"at the floor" from the outside). Two verbs still pay ~1 s of pure PIET-side cost per call —
**`order`** (20 % engine share) and **`drop`** (25 % engine share) — the same fix already proven
on `sort`, filed not yet applied; `encode`/`rename`/`keepif` sit mid-tier (77–81 % engine share,
0.17–0.24 s overhead each). Source: internal note GATE23-VERB-OVERHEAD-MAP.

- **Gate ②/③ bench baseline of record, unchanged** (MacBookPro17,1 M1 / 8 GB; {piet 2.1.1, cobra
  0.20.1}; archived .app, `--do` headless; scratch on local SSD; median/min-of-3,
  `/usr/bin/time -l`; ratified in `releases/bench/baselines/MacBookPro17_1.json`; recapture is
  User/Art-8 gated, never a worker call — see PENDING-USER #7): `use` of the 67 MB / 192-col /
  108k-row `.dta` (→ 156 MB hbin write) ≈ **0.8 s**; each `gen` (full hbin rewrite) ≈ **0.41 s
  marginal**; peak RSS ≈ **568.8 MB** (re-tightened from the original 410.2 MB on 2026-07-18 per
  Eval Art-10 ruling `RULING-stage1-rss-20260718.md` — Stage-1 resident-worker memory model).
  This is the Gate ②/③ bench baseline of record — reproduce via
  the internal `bench_gate.py --product …` (law in the internal `releases/bench/README.md`).

Speed ladder (ascending invasiveness — dossier §3-②), **reframed against the standing bench gate**:

- **(a) lazy column loading — GATE-WATCH.** Escalate only if the bench REDs or the User reports jank.
- **(b) scratch superseded-delete — SHIPPED 2.2.0 (item E), LIVE.** Implemented as a **bounded
  scratch budget with oldest-superseded eviction** (not eager per-mutation delete): `SessionState::
  load()` registers the superseded predecessor as reclaimable; the oldest registered files are
  evicted (async unlink) only once the retained total exceeds the budget (`PIET_SCRATCH_BUDGET_MB`,
  default **0** = evict on supersede since 2026-08-31 — the 4096 default hid two missing pins,
  BUGLOG BUG-381; it was 4096 when this paragraph was written). This is the gate-③ scratch-accumulation policy (132 GB precedent) **without** the
  cost cause we measured: the eager per-mutation unlink contends for disk bandwidth with the next
  mutation's 157 MB write and regressed `p2_load3gen` ~0.9 s — a short burst (well under budget)
  therefore does **zero** deletion I/O on the critical path. Safety: never a user file (only files
  inside `workdir_`), never a live `preserve` snapshot.
- **(c) dirty-column partial rewrite — RE-ESCALATED (via the Python floor).** `p2_load3gen` is the
  one mapped bench leg slower than its Python equivalent (2.03 s vs pandas 1.73 s; floor-excepted
  work item in `releases/bench/README.md`). Still gated behind hbin v2's per-column offsets;
  schedule before `mi`.
- **(d) hbin mmap read path / (e) persistent cobra session — GATE-WATCH.** Escalate only if the
  bench REDs or the User reports jank. (maintainer recommendation, User ratification pending — MSG-MI-170.)

Benchmark the 192-col fixture at each wave boundary before escalating a rung.

The browse-grid governor below remains the design intent for the *viewing* path: one memory budget
caps prefetch depth → cache → working set. One knob, many effects.

**II-1. Hardware detection.** At launch, read total RAM, available RAM, CPU count (native core —
`sysctl`/`GlobalMemoryStatusEx` per platform; the Python-era `psutil` is gone). Recompute
periodically. *(As shipped, audited 2026-09-02: total RAM and CPU count, read on every
`settings.get`; available RAM and the periodic recompute are NOT implemented.)*

**II-2. Recommended budget + user override.** Compute a recommended data budget as a fraction
of available RAM (target a safe default for an 8 GB machine). Show it in settings; let the user
raise/lower it with a slider; **persist** the choice. **SHIPPED 2.2.0 (item D):** recommended =
**½ of total RAM** (→ 4096 MB on an 8 GB machine); hardware detected natively (`sysctl` /
`GlobalMemoryStatusEx` / `sysconf`, II-1); shown in the Settings panel with a slider (ceiling
widened to total RAM) and persisted natively (see the 2.2.0 design record below). The budget's
*governing* of caching/prefetch/working-set (II-3) is the design intent; 2.2.0 delivers the
detection + recommendation + persistence, the plumbing follows the viewing-path work.

**II-3. Budget governs caching/prefetch/working-set.** The budget sets: max cached grid
windows, max prefetch depth, and the ceiling for Python data-op materialization. **SHIPPED
2026-09-02 (User directive):** grid cache depth = clamp(round(budget/256), 3, 32) windows and
prefetch depth = 2 windows once the cache can hold 8 (`grid.js` gridCapsForBudget, pushed live by
`setGridMemoryBudgetMb` beside the console cap); the budget reaches the engine as
`CECON_MEMORY_BUDGET_MB` (auto = the recommended number, a `parallel` worker gets 1/N) and bounds
the engine's parallel fan-out scratch (`src/core/threads.h` threads_for_work); it is one term of
the automatic worker count and caps the console/gallery scrollback. The "Python data-op
materialization" clause is obsolete (there is no Python layer); the working-set governor on the
native side is II-4 below.

**II-4. Pre-flight warning, never OOM.** Before a large op (merge/reshape/…), estimate its
memory cost; if it would exceed budget, ask *before* executing — *proceed / raise budget /
cancel*. PIET never crashes on memory; it negotiates. **SHIPPED 2026-09-02 (User directive):**
`piet/native/src/preflight.cpp` gates merge · joinby · append · reshape · expand · cross with a
stated cost model (merge/joinby = master + using + 2×max; append = 2×(master+using); reshape =
3×master; expand k = master×(1+k); cross = master + using + 8×max — deliberately high, and
printed in the message). Over budget in a do-file run under the app it ASKS through the
`request_dialog` push with fixed `choices` (proceed / raise the budget to the next 128 MB step —
the frontend persists it via settings.set, then answers / cancel); where it cannot ask (headless
`--do`, the single command line) it warns on stderr and in an echo block and proceeds;
`PIET_PREFLIGHT=strict` refuses instead (`memory_preflight` error, nothing executed),
`PIET_PREFLIGHT=off` disables the gate. Selftest pins the model and all three policies; the DOM
runner pins the choices dialog.

**II-5. NO silent truncation.** *(cross-constraint with I-3.)* Never drop rows to fit a budget.
The governor bounds caching/prefetch/working-set and *warns* on big ops; it must never show
partial data as if complete.

**II-6. Adaptive predictive prefetch (grid).**
- Track scroll velocity + direction (rolling average over recent frames).
- Adaptive depth: `ahead = clamp(round(|v|·k), 1, MAX_AHEAD)` (**TBD** k, MAX_AHEAD).
- Direction-aware: prefetch ahead in travel direction; evict behind first.
- Phase 2: fling **landing-point prediction** via the momentum/deceleration model (standard
  scroll physics — **not** ML).
- **In-flight cancellation:** seq-tag fetches; discard results for windows no longer near the
  viewport.
- **Constraints:** NO LOD / quality-scaling (data has no low-res form; stale/approx values
  violate I-2 + Ⅲ). A cell is the true value OR a clean grid-aligned skeleton — never a fake.
  NO ML. Adaptive + capped, never prefetch-everything.

**II-7. Established lightness wins (keep).** Virtualized grid (~100 DOM rows for 600k+ rows;
300-row windows; spacer-based positioning). The 2.0 native core removed the Python-materialization
pressure source entirely; today's cost center is the mutation path's full hbin rewrite (→ II-0
ladder), not viewing data.

**II-8. Hardware bounds.** Floor = the User's old i5 laptop / 8 GB M-series Mac; if it janks
there, it fails. Ceiling = on a capable machine, raise budgets/concurrency to use it fully.

---

## Ⅲ · Precision — technical rules

**III-1. Baseline grid.** All layout snaps to a base unit (**TBD**: 4 px or 8 px). No element
floats off-grid.

**III-2. Spacing scale.** A fixed token set for margins/padding/gaps (**TBD** scale, e.g.
4/8/12/16/24/32). No magic numbers in CSS; every space is a token.

**III-3. Optical alignment + proportion.** Align to the eye, not just the box; deliberate
proportions between panes and within blocks.

**III-4. Typography.** A defined hierarchy (sizes/weights as tokens); monospace for data and
the console; numbers column-aligned in tables.

**III-5. Lines are structural.** Every rule/divider expresses layout logic. No decorative
lines, no noise behind dense text.

**III-6. No dead surface.** Four failure modes, each a Precision violation:

- **Redundant controls.** Two affordances that do the same thing in the same context. *Bad
  example:* a "Run" button and a separate "Execute" button both submitting the current command.
  *Allowed:* a Run button and a Ctrl+Enter shortcut — different modalities, one role. If two
  controls coexist, they must have distinct, clearly differentiated roles.

- **Buried essentials.** A feature the user needs for their primary task, reachable only through
  a chain of non-obvious clicks. *Bad example:* `xtset` entry hidden two levels deep in a
  settings panel, when panel commands are a first-class use case; the data browser requiring a
  three-click menu chain to open. If a user needs it to do core work, one action from the main
  interface must reach it.

- **Orphaned chrome.** A visible element with no live role in the current context. *Bad
  examples:* a "Clear" button permanently greyed out in a pane where nothing can ever be
  cleared; a status chip that reads "Ready" at all times regardless of state; a pane header with
  a label that names a feature not yet implemented. If removing the element would not be missed,
  it must not exist.

- **Label lying.** The text on a control does not describe what happens on first press. *Bad
  examples:* a "Run" button that queues the command rather than executing it; an "Export" item
  that saves to a temp location and opens Finder instead of saving to the user's chosen path; a
  "Reset" button that resets only some state. The label is a contract: what it says is exactly
  what happens, immediately, with no surprising intermediates.

---

## Two faces — design tokens

Both modes inherit III-1…III-5. They differ only in skin.

### ◆ De Stijl tokens
- **Palette:** primary red / blue / yellow + black / white / grey. Primaries are **accent
  only** — pane caps, status chip, active markers, maker's-mark — **never behind data or text**.
- **Rules:** bold black orthogonal lines express structure; asymmetric, dynamic balance
  (Mondrian equilibrium, not centered symmetry).
- **Grid:** strict orthogonal; every block justified.
- **Dark variant:** dark + green glow on accent chrome only (kernel dot, focus ring, Run
  button, pane caps, prompt) — body text/data stay crisp.

### ◆ Boring tokens
- **Layout parity with Stata:** console + results log + variables + data browser; familiar
  output table shapes; the Run/Do-file affordances where a Stata user expects them.
- **Palette:** neutral greys; no De Stijl color blocks. Recognizability over flair.
- **Default mode** — the onboarding/adoption face.

---

## 2.2.0 UX/Settings wave — design record (items A–E, User-directed 2026-07-09)

A MINOR wave over the 2.0 native core; contract_B **unchanged** (2.0.0 — the settings routes are
app-shell, same classification as `dialog.open`/`legal.notices`, maintainer-ratified at merge). Serves
Gate Ⅲ (Precision — the toolbar IA, split layout, settings panel) and Gate Ⅱ (Lightness — the
scratch budget). Both display modes (De Stijl/Boring) and both themes inherit every change.

- **A · Toolbar group swap (Ⅲ-5).** The two UX-047 groups swap around the structural divider:
  **view tabs (Results | Do-file Editor | Graphics) LEFT**, **action group (File ▾ · Data ▾ ·
  Run .do) RIGHT** (USER-corrected order; internal order of each group unchanged). The token
  divider (`--rule`/`--rule-color`) and active-tab styling are kept — the swap is pure DOM
  reorder + one CSS note (the actions group is no longer the flex cluster leader, so its
  `margin-left:auto` is dropped). ui-smoke asserts DOM order `views | divider | actions`.
- **B · Editor run-selection (Ⅰ-mirror parity).** A drag-selected range in the do-file editor
  runs alone via the **Stata Do-button binding ⇧⌘D** (macOS; Ctrl+Shift+D elsewhere — verified
  against [GSM] 13's "If text is highlighted, the button becomes Execute selection (do)"). With no
  selection it runs the whole buffer (Stata parity). It uses the **same `do_string` execution
  path** as Run .do (no new engine surface), so BUG-045's stop-on-error editor re-open applies.
  The native guarantee (do_string runs exactly the handed source, isolated from unselected lines)
  is a selftest leg; the editor journey is a ui-smoke step.
- **C · Split panes (Ⅲ-1, De Stijl orthogonal grid; NO floating windows).** Smallest mechanism:
  `#pane-console` flips to a flex **row** of surface columns (`.split`). Results is the always-
  present base column (`#results-surface`, `display:contents` when not split, so the single-view
  DOM the gates assert on is byte-unchanged); Do-file Editor / Graphics **dock** as equal columns
  (`.docked` + `.open`). In split mode the view tabs become per-column toggles. One-click **Reset
  layout** (shown only while split is active — Ⅲ-6 no orphaned chrome) restores single-view tabs
  and clears persistence. **Persistence:** `localStorage 'piet-split' = {on, docked:[…]}`; Reset
  removes the key (recovery must be easy — User). ui-smoke drives split → dock → reset and asserts
  the key is cleared.
- **D · Settings panel.** Two groups by persistence tier:
  - **Engine & scratch** (persisted **natively**, restart-to-apply where noted) — scratch
    directory (restart), default working directory (restart, seeds `session.cwd`), memory-budget
    slider (II-2, recommended default from RAM; routed to the engine as `CECON_MEMORY_BUDGET_MB`,
    auto = the recommended number), compute cores (the TOTAL core budget — routed to the cobra
    subprocess as `CECON_NUM_THREADS` + `VECLIB_MAXIMUM_THREADS`; a `parallel` block divides it
    among its workers; applies next command). Store: a JSON file in the per-user config dir
    (`~/Library/Application Support/PIET/settings.json` · `%APPDATA%\PIET` · `$XDG_CONFIG_HOME|
    ~/.config/PIET`); `$PIET_SETTINGS_FILE` overrides for gate isolation. Bridge routes
    `settings.get` (fields + detected `sys` + `settings_file`) / `settings.set` (partial merge →
    persist → apply cores + memory). Startup reads the store on BOTH launch paths (GUI and `--do`,
    BUG-346): `scratch_path` → `$PIET_WORKDIR` (if unset), `default_working_dir` → cwd seed (if it
    exists), `compute_cores`/`memory_budget_mb` → env. Auto releases only a value PIET itself
    set (BUG-386); a pin the launcher placed — a gate's, or a parent `parallel` worker's share —
    survives, exactly as `$PIET_WORKDIR` does.
  - **Display** (persisted in **localStorage**, applied live) — the **strengthened set** kept
    minimal per smallest-change law: **editor/results font size** (drives the `--fs-*` tokens via
    `<html data-fontsize>`; `medium` = no attribute so the default DOM is byte-unchanged) and a
    **paste-autorun mirror** of the existing do-editor toggle (both checkboxes kept in sync). The
    other strengthened candidates (results precision, recent-files count) were deliberately NOT
    added (no speculative bloat). Selftest proves the settings round-trip + cores→env; ui-smoke
    drives open → font-size live → memory-budget Apply → Saved.
- **E · Scratch superseded-delete → bounded scratch budget (Ⅱ, II-0 rung b).** See II-0 above —
  registered-and-evicted-over-budget (`PIET_SCRATCH_BUDGET_MB`; default 4096 MB at ship, **0** =
  evict-on-supersede since 2026-08-31, BUG-381), async unlink,
  never a user file / preserve snapshot. The eager per-mutation delete was measured and rejected
  (~0.9 s `p2_load3gen` regression — the disk-bandwidth contention with the next write). Selftest
  proves eviction (budget 0), the safety exclusions, retention under budget, and the opt-out.

---

## Cross-references
- Gate verification flow + per-version gate: ROADMAP.md "Per-version gate process".
- Boundary/contract details: CONTRACT.md (Contract A engine↔backend, Contract B
  backend↔frontend; block types; versioning).
- Engine-side items (e.g. data-op migration in I-6, GPU bootstrap) are **cobra** roadmap, not
  PIET — routed to CoBrA Op as prose.
