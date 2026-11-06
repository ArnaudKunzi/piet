> **⚠ ARCHIVED (2026-07-07, governance overhaul).** Pre-1.0 design document for the retired
> Python-kernel architecture — superseded by the 2.0 native core ([ROADMAP.md](ROADMAP.md) ·
> [SPEC.md](SPEC.md) · `piet/native/`). Kept for history — do NOT plan or implement from this file.

# PIET Frontend Design Plan — v0

High-level context and locked decisions live in [MASTER.md](MASTER.md).
Python kernel, router, and API (server side) live in [BACKEND.md](BACKEND.md).
This document is the authority for the frontend implementer.

**요지.** 백엔드/라우터 계획([BACKEND.md](BACKEND.md))은
커널·라우터·API를 깊게 잡았지만 **프론트엔드는 "3-pane 콘솔" 한 줄로만** 남겨뒀다. 이 문서가 그 빈칸 —
PIET 데스크톱 앱이 실제로 **어떻게 보이고 어떻게 동작하는지** — 를 확정한다. 채택안은 다중 에이전트
설계 워크플로(연구 5 → 경쟁안 3 → 심사 3)에서 **2–1로 이긴 "Modern restrained De Stijl workbench"**
컨셉이며, 진 안에서 좋은 부분을 이식(graft)하고 사실 오류를 교정해 합성했다.

This is the authoritative spec the frontend implementer follows. It honors the locked decisions: static
HTML + **vanilla ES modules, no framework, no build step**; talks only to the local Python kernel over
`127.0.0.1` `/api`; renders in a **pywebview** native window (WKWebView mac / WebView2 win); offline;
light on an 8 GB M1. The frontend is a **swappable artifact** — anything here can be rewritten in
React/Tauri later as long as the API in §6 is honored.

---

## 0. The decision (from the judged design workflow)

Three full concepts were generated and scored by three judges on distinct lenses:

| Concept | Stata-familiarity (Judge 1) | MVP feasibility (Judge 2) | Identity restraint (Judge 3) | Winner votes |
|---|---|---|---|---|
| 1. Maximal Stata familiarity | **9** | 8 | 7 | 1 |
| **2. Modern restrained De Stijl workbench** | 7 | **9** | **9** | **2 ✅** |
| 3. Power-user console-first | 6 | 5 | 6 | 0 |

**Winner: Concept 2.** Its load-bearing insight is that **the Mondrian grid and the CSS Grid are the same
object**: the black 2 px dividers are not styling *on top of* a layout — they ARE the layout (the grid `gap`
showing through a dark ground). That makes the De Stijl identity free, structural, and impossible to
over-apply, while the interaction model stays 100 % Stata. Most on-brand AND lowest layout-build risk.

**Grafts folded in from the other concepts** (per judges):
- From C1: Data Browser as an **overlay** (not a 5th pane); the formatter-first build discipline; the adaptive
  coeftable header/footer (cluster/FE/IV diagnostics); Properties read from the in-memory varlist (no extra call);
  a kernel status dot + hairline cobalt highlight on the active pane.
- From C3: keyboard wins (Ctrl/⌘+L focus, PageUp/Down **and** ArrowUp/Down recall, Tab varname completion,
  jump-to-latest pill, caret-returns-to-input, Esc-to-close overlay); the empty-state greeting in the graph-paper
  ground; the strict monospace-for-data / sans-for-chrome typographic split.

**Must-fix corrections baked into this plan** (factual errors the judges caught):
1. **hbin has only two dtypes: `f64` and `factor`. There is NO string dtype** (`tools/hbin_read.py`). So there is
   no "Aa string" glyph and no red-string grid cells. Every non-numeric column is a **factor** rendered with blue
   value-labels. `auto.dta`'s `make` is a factor, not a string.
2. **One color = one meaning.** Red is reserved **strictly for error/stop**. The factor glyph moves to **cobalt
   blue** (categorical = a labeled value = blue, consistent with the grid's blue value-labels). A red factor glyph
   must never sit next to a red error block.
3. **Yellow** — the loudest primary — is held back for the **single stop-the-world moment**: the `.do` execution
   HALT bar. So the full triad appears, but rarely and unmistakably.

**Reframe (2026-06-17, user):** the *artistic* look is the **default identity**, not a second skin — see **§1.4**.
This De Stijl workbench becomes **"Mondrian" mode** (leaned a touch further into the recognizable grid + flat
red/blue/yellow composition), and **"Simple" mode** is redefined as a deliberately **boring, faithful Stata
replica** (no De Stijl, no composition — just plain Stata-grey). The earlier watercolor "Cool mode" is dropped;
that early-Mondrian/watercolor direction now lives only in the user-made app **icon** (§5).

---

## 1. Design language

### 1.1 Disciplined color semantics (the CONTENT layer)
Black = structure · three primaries = three meanings · grid = empty ground · off-white everywhere else.
**In content — the console, glyphs, status dots — nothing is colored for flavor;** every hue below is a state
signal. The default **Mondrian** mode adds a SEPARATE decorative De Stijl *composition* layer (flat primary blocks
à la Mondrian) in the **chrome + empty-state only**, kept spatially apart from content so meaning stays
unambiguous — see §1.4. (The boring **Simple** mode strips even that to a flat Stata-grey skin.)

| Token | Hex | Meaning — used ONLY for | 
|---|---|---|
| `--neutral-ink` (black) | `#1a1a1a` | **STRUCTURE.** The 2 px grid gaps (Mondrian skeleton), menu/status rules, command-input top rule, the left rule wrapping each estimation block, and the `─ │ ┼ ┃` ASCII rules inside output. Never decorative. |
| `--primary-blue` (cobalt) | `#0d47a1` | **ACTIVE / REFERENCE / CATEGORICAL VALUE.** Command-input focus outline (the only blue-as-focus), menu link text, the factor `◆` glyph, factor value-labels in the grid, selected-row tint `#e6f1fb`, the hairline highlight on the active pane's title strip. |
| `--primary-red` | `#d93b3b` | **ERROR / STOP only.** Error & rejection blocks (3 px left rule + `#faeaea` tint), the `✕` history mark, the kernel-down status dot. |
| `--primary-yellow` | `#ffc107` | **RESERVED — the one stop-the-world accent.** Only the `.do` HALT bar (`STOP at line N`) carries a yellow rule. Never a text color (contrast fails). |
| `--color-success` (muted green) | `#2e7d32` | Utility (not part of the De Stijl triad, kept desaturated): the `✓` history mark and the kernel-ready status dot. |
| numeric `#` glyph | `--neutral-text-muted` `#666` | Numbers are the neutral default — muted ink, no primary. |
| `--grid-color-light` | `#c7e0f4` @ ~12–15 % | The faint light-blue graph-paper ground (20 px cells). Shows ONLY behind genuinely empty panes; vanishes the instant content lands. Echoes the icon's data-matrix backdrop. |

Grounds: panels `--neutral-paper #faf9f8` · console + grid pure white `#ffffff` (clean column contrast) ·
app gap-ground `--neutral-ink #1a1a1a` · resting warm off-white `--neutral-ground #f9f7f4` (the icon's ground).
No rounded corners on panes (`--radius-none`); internal chips may use `--radius-sm 2px` sparingly.

### 1.2 Typography (offline, system fonts only — no web fonts in WKWebView/WebView2)
- **Data / results = monospace** (`Menlo`, `'Courier New'`, monospace), 11 px, line-height 1.5. This is where
  every number lives and where Stata-style column alignment is non-negotiable.
- **Chrome / UI = system sans** (`-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Helvetica Neue', sans-serif`),
  13 px. Menu, panel headers, status bar.
- The strict split *is* half of "understated like Stata": color stays out, monospace gravity does the talking.

### 1.3 Tokens — paste-ready `:root` (drop into `css/piet.css`)
```css
:root{
  /* neutrals */
  --neutral-ground:#f9f7f4; --neutral-surface:#ffffff; --neutral-paper:#faf9f8;
  --neutral-ink:#1a1a1a; --neutral-text:#333; --neutral-text-muted:#666; --neutral-text-lighter:#999;
  --neutral-border:#d4d0cc; --neutral-border-strong:#a8a096; --neutral-divider:#e8e4de;
  /* CoBrA primaries — used sparingly & structurally */
  --primary-red:#d93b3b; --primary-blue:#0d47a1; --primary-yellow:#ffc107;
  --color-error:var(--primary-red); --color-error-light:#faeaea; --color-error-border:#e8a9a9;
  --color-success:#2e7d32; --color-info-light:#e6f1fb;
  --color-active:var(--primary-blue); --focus-ring:var(--primary-blue);
  --grid-color-light:#c7e0f4;
  /* type */
  --font-mono:'Menlo','Courier New',monospace; --font-ui:-apple-system,BlinkMacSystemFont,'Segoe UI','Helvetica Neue',sans-serif;
  --fs-mono:11px; --fs-mono-sm:10px; --fs-ui:13px; --fs-ui-sm:11px; --lh:1.5;
  /* structure */
  --rule:2px; --rule-color:var(--neutral-ink); --radius-none:0; --radius-sm:2px;
  /* space (8px scale) */ --s-xs:4px; --s-sm:8px; --s-md:12px; --s-lg:16px; --s-xl:24px;
  --grid-cell:20px;
  /* dark theme (CSS-flip; post-MVP toggle) */
  --dark-ground:#1a1a1a; --dark-surface:#2d2d2d; --dark-paper:#262626; --dark-ink:#f0f0f0;
}
@media (prefers-color-scheme:dark){:root{ /* swap grounds→charcoal, ink→#f0f0f0; primaries UNCHANGED (red still error, blue still active) */ }}
```
Dark mode = a single `:root` rebind that **keeps all three semantic primaries** (they read better saturated on
dark). Shippable but **out of the v0 critical path**.

### 1.4 Two modes — **Mondrian** (default) & **Simple**

Per the user's clarification: the **artistic** look is the **default identity**; "simple" means a deliberately
**boring, faithful Stata replica** for people who want zero flair. Both are the SAME layout / DOM / API — a
`data-mode` flip + token rebind. (The earlier watercolor "Cool mode" is dropped; that early-Mondrian/watercolor
energy now lives only in the user-made app **icon**, §5.)

- **Mondrian mode** (default — the identity) = the De Stijl workbench of §1.1–§1.3, leaned a touch further into the
  **Mondrian we know**: the black grid **plus a disciplined set of flat primary-color composition blocks (red ·
  blue · yellow)** in the chrome + empty-state, on the off-white ground. Recognizably Neoplasticist, still a
  serious tool — "a De Stijl painter's Stata."
- **Simple mode** = a deliberately **boring, faithful Stata replica**: neutral system grey + white, thin grey
  window borders (NOT the black De Stijl grid), monospace results, blue links, red error text — maximum
  familiarity, zero identity. The "just give me Stata" mode. Same layout/DOM/API; it strips the De Stijl structure
  and the composition layer to a flat Stata-grey skin.

**Toggle** — `<html data-mode="mondrian|simple">` (orthogonal to light/dark), `localStorage`-persisted, **Mondrian
is the boot default**, set before first paint (no FOUC). No reflow, no DOM mutation, no kernel/API change — same
`/api/*` and same `ResultBlock` union (§6) in both modes. A toolbar toggle flips it instantly.

**How Mondrian mode adds color without breaking "1 color = 1 meaning":** the primaries do double duty across two
SEPARATE, spatially-disjoint layers:
- **Content layer = state semantics (unchanged, §1.1):** red `#d93b3b` = error/STOP, yellow `#ffc107` = the `.do`
  HALT bar, cobalt = active/reference/categorical, green = success — in the console, glyphs, dots.
- **Composition layer = decorative De Stijl, in CHROME + EMPTY-STATE only** — flat primary blocks placed
  asymmetrically à la Mondrian, never in the content scroll:
  - a clean **Mondrian maker's-mark** (a real ~18 px *Composition*: black rules + one red/blue/yellow block) top-left;
  - a small **blue** cap on the `VARIABLES` header, a small **yellow** cap on the `HISTORY` header;
  - the per-estimation block's left rule shifts black→**cobalt** (reads as "a result," reinforcing blue=reference);
  - a 3-square primary chip on the status bar's left edge;
  - **the empty-state console = a genuine Mondrian composition** (off-white rectangles + thick black rules + one
    each red/blue/yellow block, asymmetric), buried by the first command's output via the z-index trick (zero DOM
    mutation; `clear` re-reveals it).
- **The disambiguation rule:** decorative **red** appears ONLY in the maker's-mark + the empty-state composition
  (both vanish under / aren't adjacent to content) — **never in always-visible chrome** — so a decorative red
  block can never be confused with a real error block. Decorative **blue/yellow** may appear as the small chrome
  caps. This is what lets the default be visibly Mondrian while every content-level signal keeps its single meaning.

**Token overrides** (layered on §1.3). Mondrian mode keeps §1.3 almost intact (it IS the §1–§9 workbench); it only
ADDS the decorative composition primaries + the cobalt estimation-rule. Simple mode strips to flat Stata-grey:
```css
[data-mode="mondrian"]{               /* DEFAULT; §1.3 state tokens otherwise unchanged */
  --mondrian-red:#d42a1c;             /* flat composition red  — DECORATIVE: maker's-mark + empty-state ONLY */
  --mondrian-blue:#1850a8;            /* flat composition blue — maker's-mark, VARIABLES header cap, empty-state */
  --mondrian-yellow:#f4c20d;          /* flat composition yellow — maker's-mark, HISTORY header cap, empty-state */
  --estimation-rule:var(--primary-blue); /* per-result block left rule: black → cobalt ("this is a result") */
  /* STATE signals (§1.3) ALL UNCHANGED: red #d93b3b=error, yellow #ffc107=HALT, cobalt #0d47a1=active */
}
[data-mode="simple"]{                 /* boring Stata replica — strips De Stijl + composition */
  --neutral-ink:#3a3a3a;              /* thin GREY window borders, NOT black De Stijl rules */
  --neutral-ground:#ececec; --neutral-paper:#f5f5f5; --neutral-surface:#ffffff;  /* Stata grey + white */
  --estimation-rule:transparent;      /* no colored result rule — plain Stata output */
  /* composition layer suppressed entirely; greeting is plain text, no Mondrian empty-state */
}
```

**Empty-state console = a Mondrian composition** (Mondrian mode) — inline SVG, crisp & cheap, no image files: the
empty console is divided by **thick black rules** into a few asymmetric off-white rectangles with **one flat block
each of `--mondrian-red` / `--mondrian-blue` / `--mondrian-yellow`** (the *Composition with Red, Blue and Yellow*
silhouette); the greeting (`. Type a command below, or File ▸ Open Data`) sits in a white cell on top. It lives at
z-index 1; **the first command's output (z-index 2) buries it with zero DOM mutation**; `clear` re-reveals it.
Degrade: fall back to the faint graph-paper ground (§1.1) if it ever distracts. (Simple mode: plain greeting, no art.)

**Mondrian-mode notes / risks** (beyond §9): (1) keep the composition blocks **flat and few** — "a bit more
Mondrian," not paint-by-numbers; the workbench must still read as a serious tool. (2) Hold the disambiguation rule:
**no decorative red in always-visible chrome**, and **no composition primary ever colors a glyph/dot/link/state**
(one slip collapses "1 color = 1 meaning"). (3) The composition red/blue/yellow are deliberately a touch
deeper/flatter than the state primaries so a Mondrian block never reads as a live signal sitting in the chrome.
(4) Build Simple mode as a pure token-strip (grey skin + suppressed composition + plain greeting) — it must require
**zero** layout/DOM change, proving the `data-mode` contract.

---

## 2. Window / layout map

The whole window is one CSS Grid; the **2 px gaps are the black De Stijl rules** (the `#1a1a1a` ground shows
through). No borders on panels → crisp, continuous lines, zero double-border artifacts. **Splitters are
non-draggable in v0** (resize is post-MVP); the gap-gridline is the permanent v0 form.

```
┌─────────────────────────────────────────────────────────────────────┐
│ ▣ PIET   acs_wages.dta        File · Data · Statistics · Run .do  32px│  ← cobra glyph + wordmark; menu = blue links
├──────────┬──────────────────────────────────────────┬───────────────┤
│ VARIABLES│              RESULTS CONSOLE              │   HISTORY     │
│  (left)  │                (center spine)             │   (right)     │
│  150px   │▓               1fr (flex)               ▓ │    132px      │
│          │  ← 2px black rule         2px black rule →│               │
│ ┌──────┐ │  ┌─────────────────────────────────────┐ │               │
│ │ prop │ │  │ . command input (single line)   34px│ │               │
│ └──────┘ │  └─────────────────────────────────────┘ │               │
├──────────┴──────────────────────────────────────────┴───────────────┤
│  acs_wages.dta · 6 vars, 4,812 obs · ● kernel                   24px │  ← status bar
└─────────────────────────────────────────────────────────────────────┘
```
```css
#app{
  display:grid;
  grid-template-columns:150px var(--rule) 1fr var(--rule) 132px;
  grid-template-rows:32px var(--rule) 1fr var(--rule) 24px;
  height:100vh; background:var(--neutral-ink); /* gaps show through as black */
}
/* toolbar row 1 / status row 5 span all 5 columns; the three panes sit in row 3, cols 1/3/5 */
```

**Asymmetry is deliberate** (Mondrian never centers): left 150 / center flex / right 132. The center spine
dominates (analysis-as-center); the two flanks are reference rails of unequal weight.

**Region inventory**

| Region | Default size | Always visible? | Notes |
|---|---|---|---|
| Menu/toolbar strip | 32 px, full width | always | cobra glyph (22 px) · `PIET` wordmark · filename (muted) · `File · Data · Statistics · Run .do` blue link-text (no buttons/icons) |
| Variables panel (left) | 150 px | always | type-glyph + name rows; collapsible post-MVP |
| Properties sub-panel | ~80 px footer of left col | always | reads selected var's `VarInfo` from memory — **no extra endpoint** |
| Results console (center) | 1fr | always — **the spine** | append-only block log |
| Command input | 34 px, bottom of center | always | single `<input>`, `.` prompt glyph |
| History panel (right) | 132 px | always | ✓/✕ + command; collapsible post-MVP |
| Status bar | 24 px, full width | always | file · dims · kernel dot |
| **Data Browser grid** | **overlay over center+flanks** | on `browse`/`br`/Data menu | **NOT a pane.** Covers the console (console preserved underneath); **Esc / "Back to Results"** restores it untouched. Lazy-init. |
| tabulate / summarize / describe | **inline console blocks** | n/a | they are output, not windows |
| `.do` runner | OS file picker → streams into the **same** console | n/a | no separate editor window in v0 |

The Data-Browser-as-overlay choice is unanimous across reference GUIs (Stata/RStudio/JASP/gretl): **data is a
lookup tool, not the spine.** It also dodges the hardest CSS (track-spanning a 5th grid column under deadline).

---

## 3. Component inventory

Each lists **source** (endpoint from §6) · **states** · **interactions**.

**3.1 Toolbar** — static + triggers `/api/open`, `/api/do`. `File ▸ Open` → pywebview `create_file_dialog` →
`POST /api/open`. `Run .do` → picker → `POST /api/do` (spinner-dot while running). `Statistics` is a thin
convenience menu that just **prefixes the command input** (e.g. inserts `regress ` and focuses) — **never a modal
dialog in v0**, keep it command-driven. **Graphics is OMITTED entirely** (not greyed) so no disabled-color UI appears.

**3.2 Command input** — `POST /api/command`. Single `<input type=text>` (not textarea), monospace 11 px, a
non-editable `.` prompt glyph in `#999`, on `--neutral-paper`, 2 px black top rule. States: idle · focused
(2 px inset cobalt outline — the only blue focus) · busy/disabled (opacity .5 during a running command).
Keys: `Enter` runs + clears; `PageUp`/`ArrowUp` recall previous (both first-class), `PageDown`/`ArrowDown`
forward, select-all on recall; `Tab` cycles varname completion against `appState.varlist`; `Ctrl/⌘+L` focuses
from anywhere; caret returns here after every action.

**3.3 Results console (spine)** — renders the `blocks[]` from `/api/command` and `/api/do`; dispatches on
`block.type`. Append-only `<div class=console>`, white ground. Block renderers:

| `block.type` | render |
|---|---|
| `echo` | `. {text}` in muted grey `#666` |
| `estimation` | adaptive Stata header + monospace coeftable, wrapped in a 2 px black left rule; multi-eq → one labeled sub-table each |
| `summarize` | aligned Obs/Mean/Std.Dev./Min/Max table |
| `tabulate` | freq table + χ² footer line |
| `describe` | variable-metadata table |
| `data_mutation` | one-line `(variable log_wage created)` / `(2 variables dropped)` |
| `error` | red block (3.8) |

States: empty (graph-paper ground + greeting `Type a command below, or File ▸ Open Data`) · populated ·
auto-scroll-pinned vs user-scrolled-up (pause auto-scroll when the user scrolls up; show a **"jump to latest"
pill** / `Ctrl+End`). Text is real characters (`─` rules included) so copy-paste preserves alignment. A small
`× clear` sits top-right (logs grow unbounded — a known trap).

**3.4 Coeftable formatter** (sub-component, **the critical path — see §8**) — input is
`coeftable:[{name,b,se,t,p,ci_lo,ci_hi}]` plus `n_obs, r2, r2_within, dof_resid, vcov_type, n_clusters,
absorbed{}, first_stage_F, kp_wald_F, hansen_j` (all real engine fields). Compute column widths = max(header, all
formatted values); right-align numerics with Stata `%9.0g`/`%9.3f` semantics; `b===null` → collinear/omitted row.
**Adaptive header/footer**: `Linear regression` + `Number of obs`, conditionally `F(k,dof)`, `R-squared`,
`Within R²` (if `r2_within`), `(Std. Err. adjusted for N clusters)` when `vcov_type=="cluster"`,
`Absorbed: firm (412 levels)` from `absorbed{}`, and the IV diagnostics footer (`first_stage_F`/`kp_wald_F`/
`hansen_j`) when present. **Degradation:** if time runs short, flatten multi-outcome (`outcomes[]`/`equations[]`)
to the first equation rather than ship misaligned nested tables.

**3.5 Variables panel** — `GET /api/varlist` → `VarInfo{name,dtype,format,label,n_levels,levels,n_missing}`;
**refreshed from `varlist`/`varlist_after` after every mutation**. Rows = monospace 11 px: a **type glyph** + name.
Glyph = `#` in muted ink for `f64` (numeric), `◆` in **cobalt blue** for `factor` (categorical) — the single
most-borrowed pattern (JASP/jamovi "variable-type visual is gold") rendered in the De Stijl palette, **with red
removed**. States: default · hover (row tint) · selected (`#e6f1fb` → drives Properties). Click selects; optional
search box at top. (Double-click-to-insert-name is post-MVP.)

**3.6 Properties** (left footer) — the selected row's `VarInfo`, already in memory (no call). ~80 px, 1 px black
top rule, 10 px: `name · factor(2) · level labels (first few) · N missing`.

**3.7 History / Review** — `GET /api/history` + appended client-side. Monospace 10 px rows: a status mark +
truncated command. `✓` muted-green (success), `✕` red (failure). Click → **copies** the command into the input +
focuses (does **not** auto-execute — safer than click-to-rerun for destructive gen/drop). 50-entry cap.

**3.8 Error block** — `CommandErrorResult{code,message,token,line_number,context}`. `#faeaea` bg, 3 px red left
rule, red text. Bold first line = the rejection (`unsupported: foreach`), second = the human reason. For a `.do`,
a third line `line N: {context}` carries a **yellow STOP rule** (the one stop-the-world accent) and halts replay.

**3.9 Data Browser grid (overlay)** — `GET /api/browse?offset&limit&vars` → `{columns[],rows[][],n_rows_total}`.
The `VirtualizedGrid` (§7.3): sticky header, left row-number gutter, **constant ≤100 DOM rows**, spacer divs,
**24 px fixed row height**, monospace. Cells: numeric right-aligned black; factor-labeled left-aligned **cobalt
blue**; missing = `.` in `--neutral-text-lighter`. (No string dtype → the red-string case is moot.) Vertical
scroll fetches windows; horizontal scroll keeps a sticky gutter; **Esc** closes. Read-only in v0.

**3.10 Status bar** — `appState`: `{file} · {nCols} vars, {nRows} obs · ● kernel`. The `●` is muted-green when
ready, **red** when the kernel is unreachable. Static (no interactions).

---

## 4. Core interaction flows

**(a) Open dataset → Variables fill.** `File ▸ Open` → pywebview dialog → `POST /api/open{filepath,format}`.
Kernel converts (`to_hbin.py` for `.dta`) → `{action:"open",varlist_after,n_rows_after}`. Variables panel fills
(`wage age educ` blue `#`; `sex region` blue `◆`); status bar updates; console prints `. use acs_wages.dta` +
`(6 vars, 4,812 obs loaded)`.

**(b) `regress wage age educ i.sex, robust` → echo + coeftable.** Input disables (busy); `POST /api/command`.
Kernel routes → `feols --y wage --x "age,educ,i(sex)" --vcov hc1 --data <hbin> --tidy`; returns
`blocks:[{echo},{estimation}]`. Console shows the grey echo, then the coeftable wrapped in a 2 px black left rule
with the adaptive header (`Number of obs / F / R-squared`) and `(Std. Err. adjusted for heteroskedasticity)`;
auto-scrolls; History gains `✓ regress wage…`; `lastModel` is cached so `test`/`margins`/`predict` work; input
re-enables + focuses.

**(c) `browse` → virtual grid.** Lazy-inits `VirtualizedGrid`; `GET /api/browse?offset=0&limit=300`. The
overlay covers center+flanks: sticky header, gutter, ~50 visible rows; `sex` shows `male`/`female` in blue;
missing = `.`. Scrolling fetches windows; DOM stays ~100 rows even at 1 M. **Esc** restores the console as it was.

**(d) Run a linear `.do` → sequential replay, red STOP on the first unsupported line.** `Run .do` → picker →
`POST /api/do{filepath}`. Kernel runs line-by-line, halting at the first error. Blocks stream into the **same**
console in order (`use` → `gen` → `regress` coeftable …); at the first `foreach` a red block appears with the
**yellow `STOP at line 15: foreach v in age educ {`** bar; nothing after line 15 runs. History shows the
successful lines `✓` and the failing one `✕`. This is the load-bearing safety contract: never silently wrong.

**(e) Unsupported interactive command → rejection block.** `forvalues i = 1/10 {` → kernel returns the
structured `error` (`code:"unsupported_command", token:"forvalues"`). Grey echo, then the red block with the
reason. Dataset + console otherwise untouched; the user types the next command immediately.

---

## 5. CoBrA / Mondrian identity — applied with restraint

- **Black = structure** (the grid gaps, rules, table rules). Load-bearing, never decorative.
- **Three primaries = three meanings** (blue active/reference, red error/stop, yellow the one HALT accent) — §1.1.
- **Two modes (§1.4):** the **default "Mondrian" mode** adds a *decorative* De Stijl composition layer — flat
  red/blue/yellow blocks in the **chrome + empty-state only**, spatially apart from content so the three meanings
  above stay unambiguous; the boring **"Simple" mode** strips all of it to a plain Stata-grey skin.
- **Graph-paper ground** only behind empty panes (console before output, grid past the last row); fades under content.
- **The maker's-mark glyph** appears once, small (22 px), top-left — a quiet mark, not a banner. **The current
  CoBrA-painted-cobra icon is a placeholder being retired** (too on-the-nose). The user is designing the
  replacement themselves: an **early-Mondrian** direction — his *representational* period (*Devotie* 1908, the
  amaryllis/lily flower studies, the luminist dune/sea landscapes), **watercolor + vivid saturated color**,
  possibly a **regression-line / distribution-curve motif** (data-as-art). Treat the 22 px slot as a themeable
  drop-in until that art lands; until then **Mondrian** mode shows a clean ~18 px Mondrian *Composition* maker's-mark
  (black rules + one red/blue/yellow block) and **Simple** mode shows a plain text mark (§1.4).
- **No clichés**: no up-arrows, no bars, no chart decoration, no rounded toy corners. The identity lives entirely
  in the black grid lines and the three disciplined primaries — *a De Stijl painter's Stata, not a colorful dashboard.*
- **Light default** (Stata-traditional); **dark = a CSS-var flip**, primaries preserved, post-MVP toggle.

---

## 6. The API contract the frontend consumes

The **kernel owns** this contract; the frontend is a pure consumer (see backend plan for kernel-side detail).
WS streaming is **deferred** — v0 uses synchronous `POST /api/do`.

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/command` | POST | run one Stata line → `{blocks[], varlist?}` |
| `/api/varlist` | GET | Variables-panel feed `{varlist[], n_rows, n_cols}` |
| `/api/browse?offset&limit&vars` | GET | windowed grid page `{columns[], rows[][], n_rows_total}` |
| `/api/tabulate` | POST | freq table ± χ² |
| `/api/describe` | POST | variable metadata |
| `/api/summarize` | POST | summary stats |
| `/api/do` | POST | run `.do` line-by-line → `{blocks[], stopped_at_line?, stopped_at_reason?}` |
| `/api/open` | POST | load `.dta`/`.csv`/`.hbin` → `{action:"open", varlist_after, n_rows_after}` |
| `/api/history` | GET | command history |
| `/ws/console` | WS | **deferred** (streaming `.do`) |

**One canonical `ResultBlock` union the console renders** (frontend dispatches on `.type`):
```ts
type ResultBlock = EstimationResult | SummarizeResult | DataTableResult /*tabulate*/
                 | DescribeResult | BrowseResult | DataMutationResult | EchoResult | CommandErrorResult;

interface EstimationResult { type:"estimation"; estimator:string; depvar:string; n_obs:number;
  coeftable:CoefficientRow[]; outcomes?:Outcome[];        // multi-eq nesting (reg3/heckman/*_multi)
  r2?:number; r2_within?:number; sigma2?:number; deviance?:number;
  absorbed?:Record<string,number>; vcov_type?:string; n_clusters?:number; dof_resid?:number;
  first_stage_F?:Record<string,number>; kp_wald_F?:number; hansen_j?:number; hansen_j_p?:number;
  converged?:boolean; iters?:number; }
interface CoefficientRow { name:string; b:number|null; se:number|null; t:number|null;
  p:number|null; ci_lo:number|null; ci_hi:number|null; }   // null b = collinear/omitted
interface VarInfo { name:string; dtype:"f64"|"factor"; format?:string; label?:string;
  n_levels?:number; levels?:string[]; n_missing?:number; }  // NOTE: only f64 | factor exist
interface BrowseResult { type:"browse"; offset:number; limit:number; n_rows_total:number;
  columns:{name:string;dtype:"f64"|"factor";index:number;levels?:string[]}[];
  rows:(number|string|null)[][]; }                          // NaN/factor -1 → null → render "."
interface DataMutationResult { type:"data_mutation"; action:"gen"|"egen"|"drop"|"keep"|"keepif"|"open";
  varlist_after:VarInfo[]; n_rows_after:number; new_vars?:string[]; dropped_vars?:string[]; }
interface CommandErrorResult { type:"error"; code:"unsupported_command"|"unsupported_option"|"syntax_error"|"engine_error"|"no_data_loaded";
  message:string; token?:string; line_number?:number; context?:string; }
```

**Routing facts the frontend relies on** (kernel-side, but they shape what we render):
`regress`→`feols`; `,robust`→`--vcov hc1`; `i.var`→`i(col)`; `vce(cluster id)`→`--vcov cluster --cluster id`;
`reghdfe …,absorb()`→`feols --fe`; **`xtreg …,fe`→`feols --fe`** but `xtreg …,re`→`xtreg`; `poisson`→`fepois`;
`gen/egen/drop/keep`→`eval --… --out` + hbin swap; `summarize/describe/tabulate/browse` = python-native.
Rejected constructs (loops, macros, programs, graphics, `#delimit`, `c.`/`ib#.`/`##`) are detailed in §6.1.

### 6.1 Language layer — the rejection contract (authority: internal language notes)

The kernel's `.do` tokenizer and rejection rules are **not** ad-hoc: they are specified from the StataNow
manuals in internal language notes (not in the public tree) — `lexing.md` (comments · `#delimit` · quote nesting ·
tokenize), `macro.md` (macro expansion), `control-flow.md` (`foreach`/`forvalues`/`while`/`if`), plus
`program.md`, `syntax-cmd.md`, `varlist-factor-ts.md`, `sample-prefix-expr.md`, `results.md`. This corpus is the
authority for **both** the v0 rejection layer **and** the Tier-A interpreter. The frontend just renders whatever
structured `error` the kernel emits — but the whole "never silently wrong" promise rests on these guarantees:

**Why rejection is a *correctness* contract, not just a feature gate** (`macro.md` §2a, §2c): (1) macro expansion
is a **textual pre-pass over the entire line, before the command is parsed**; (2) an **undefined macro expands to
the empty string — and that is NOT an error.** So if PIET ever *stripped or ignored* a macro reference and ran the
rest, it would silently change the command's meaning (`regress y x`i'` with `` `i' `` deleted ≠ what the user
wrote) — precisely the silent-wrongness the project forbids. **⇒ The kernel must DETECT macro / control-flow /
program syntax at the lexing stage — before command classification — and reject the whole line or block. Never
strip-and-proceed.**

**Linear lines the tokenizer must handle (supported)** — per `lexing.md`: strip `*`-line / inline `//`
(leading-blank-required) / `/* */` comments; join `///` and `/* */` line-continuations; split options on the
**first unparenthesised, unquoted comma**, honoring **compound double quotes** `` `"…"' `` (they nest: +1 on
`` `" ``, −1 on `"'`) and `vce(…)`-style parentheses.

**What it must explicitly REJECT** (named `error`, offending `token`; in a `.do` also `line_number` + `context`):
- **Macro references anywhere on a line:** local `` `name' `` (grave + apostrophe), global `$name` / `${name}`,
  inline `` `=exp' ``, `` `:macro fcn' ``, `` `macval(x)' ``, positional `` `0'…`9' `` / `` `*' ``.
- **Macro statements:** `local`, `global`, `tempvar`, `tempname`, `tempfile`, `macro` (`macro shift`),
  `tokenize`, `gettoken`, `local ++x` / `--x`.
- **Control flow:** `foreach`, `forvalues`, `while`, program-level `if exp { … }` / `else`, `continue`.
- **Programs / lexer modes:** `program [define] …`, `#delimit ;` (and any `#…` preprocessor command).
- **Factor/interaction options on a supported command:** `c.`, `ib#.`, `##` (engine offers `i(col)` only).
  → **distinct message** (this is NOT a "next-milestone interpreter" item): *interactions aren't in the engine
  yet — create them manually with `gen xz = x*z` for now; native factor-var `##`/`c.` is the **#1 prioritized
  engine roadmap gap** (per the v0 coverage audit; DID `i.treat##i.post` is the painful case).*
- **Unknown command names:** anything outside the supported router table (§6).

Suggested `code` values for the frontend's red-block renderer (it dispatches the same either way):
`unsupported_macro` · `unsupported_loop` · `unsupported_program` · `unsupported_delimiter` ·
`unsupported_option` · `unsupported_command`. Message template:
`"<construct> is a next-milestone feature in PIET v0 (loops, macros, and programs arrive with the Tier-A
interpreter)."` In a `.do` this is the **red + yellow STOP block** (§3.8 / §4d) and execution halts at that line;
interactively it is the plain **red block** (§3.8 / §4e).

### 6.2 Patch compatibility — engine · kernel · frontend independently versionable

**문제.** 엔진(cobra)은 브랜치에서 계속 패치되고, 프론트는 스왑 가능한 아티팩트다. "서로 패치에 상관없이 호환"
되려면 **계약을 명시·버전화·테스트**해야 한다 — `--tidy` 앵커는 옳은 본능이지만 그것만으론 두 쪽 버전이
따로 놀 때의 안전을 보장하지 못한다.

**두 경계, 두 계약:**

| 경계 | 계약 | 앵커 / 스왑 심 |
|---|---|---|
| **A. cobra 바이너리 ↔ Python 커널** | 서브커맨드명 · 플래그명 · JSON 스키마 · exit code | **`--tidy` → 단일 `coeftable[]`** (estimator별 native 필드는 그 아래서 변해도 커널 무영향; 엔진이 분리용으로 의도한 인터페이스 — `apps/cecon_cli.cpp` 주석) |
| **B. 커널 ↔ JS 프론트** | §6 `/api/*` + `ResultBlock` 유니언 | HTTP API 자체 (프론트는 React/Tauri로 재작성 가능 — 본 문서 서두) |

**출하 현실 = 번들-핀.** `.app`/`.exe`는 cobra를 **버전 고정으로 동봉**(`packaging/bin/<os>/cobra`) → 릴리스된
PIET 한 벌은 엔진+커널+프론트가 **함께 테스트된 트리플**이다. **사용자는 미스매치를 만들 수 없다.** '독립 패치
호환'은 사용자가 아니라 **개발/릴리스 단계** 문제다(엔진 브랜치가 앞서가거나, 프론트 재빌드 없이 새 엔진을
내보낼 때).

**패치-내성 4종** — 엔진 패치가 커널을 *조용히* 깨는 일을 차단(§6.1과 같은 "never silently wrong" 법칙의 인프라판):
1. **버전 핸드셰이크.** cobra가 `cobra version --json` → `{cobra_version, contract_version}`를 노출하고, 커널이
   **부팅 시 검사**. 불일치(커널 요구 contract ≥ X, 바이너리 < X)면 명령 중간에 조용히 죽는 대신 **명시적
   경고/거부** + status bar 커널 점 빨강·툴팁. (열린 질문 §11.7)
2. **엔진 계약 = additive-only / semver.** 서브커맨드·플래그·JSON 필드 **추가 = minor**(커널 무파손);
   이름변경·삭제·의미변경 = **major**(커널 lockstep 갱신). **`coeftable[]`는 동결(frozen)** — 모양이 절대 안
   바뀌는 단 하나의 필드. §6 `/api`도 프론트 향해 같은 규율(필드 추가는 자유, 재용도 금지).
3. **경계 contract-test** (CI, 양쪽 심). (a) 커널↔cobra: 골든 spec 몇 개로 커널의 argv 빌더를 실제 바이너리에
   때려 **argv 수용(exit 0) + `coeftable` 파싱 + 핵심 필드 존재** 단언 → 어느 쪽이 패치되든 드리프트를 즉시
   포착. (b) 커널↔프론트: `/api/*` 응답을 `ResultBlock` 스키마로 검증(pydantic). **수치 eval 하니스와
   별개·상보** — 저건 "답이 맞나", 이건 "모양이 호환되나".
4. **방어적 렌더(이미 §3.4·§6).** 모르는 JSON 필드 무시 · 누락 헤더 통계 생략 · 모르는 estimator도 generic
   coeftable로 렌더. ⇒ additive 엔진 변경은 크래시 대신 **우아한 degrade**(새 통계는 커널/프론트가 배울 때까지
   안 보일 뿐).

**Net:** 번들-핀이 출하본을 무위험으로 만들고, 핸드셰이크 + semver-additive + contract-test가 개발 중
엔진/커널/프론트의 독립 패치를 안전하게 만든다.

---

## 7. Frontend engineering (vanilla ES modules, no build)

### 7.1 Files (~1,700 lines total)
| File | ~lines | Purpose | Exports |
|---|---|---|---|
| `index.html` | 150 | single DOM shell: grid, 3 panes, toolbar, input | (DOM) |
| `css/piet.css` | 250 | tokens + De Stijl layout, console, panels, grid | (styles) |
| `js/api.js` | 200 | `fetch` wrappers, error envelope, (later) WS | `HTTPClient`, `ApiError` |
| `js/app.js` | 350 | central state, event bus, lifecycle, `runCommand/loadFile` | `init`, `runCommand`, `loadFile`, `getState` |
| `js/console.js` | 200 | append-only render, `formatEstimation()` & friends | `appendBlock`, `appendTable`, `clear` |
| `js/grid.js` | 450 | **windowed virtualization** (scroll→fetch→render) | `VirtualizedGrid` |
| `js/panels.js` | 150 | Variables + History sidebars | `updateVariables`, `updateHistory` |
| `js/util.js` (opt) | 100 | number/format/string helpers, `pad()` for table alignment | — |

### 7.2 State + events (no framework)
Single `AppState` is the source of truth; panels subscribe to native `CustomEvent`s on `document.documentElement`.
```ts
interface AppState { currentFile:string|null; varlist:VarInfo[]; nRows:number; nCols:number;
  busy:boolean; busyMsg:string; commandHistory:{cmd:string;ts:number;success:boolean}[]; historyIndex:number;
  lastModel:Model|null; consoleBlocks:ResultBlock[]; kernelReady:boolean; kernelUrl:string; theme:'light'|'dark'; }
```
`app.js` emits `app:command-run`, `app:state-update`, `app:console-append`; `console.js`/`panels.js`
`addEventListener` and re-render their slice. No NPM, no virtual DOM — plain listeners scale fine for 4 panes.

### 7.3 The windowed data grid (the one genuinely stateful component)
Keep **only ~50–100 row divs**; spacer divs above/below preserve scroll height. **24 px fixed row height.**
```
on scroll:
  topRow   = floor(scrollTop / 24)
  if [topRow-buffer, topRow+visible+buffer) not in cachedWindows:
      fetch GET /api/browse?offset=windowStart&limit=300   // fetch a 300-row window, prefetch 1 ahead
      cachedWindows.set(windowStart, rows); evict to keep ≤3 windows (~360 KB)
  render rows [topRow, topRow+visible) into the ~100 reused divs   // DOM stays constant
  spacerTop.height = topRow*24;  spacerBottom.height = (n_rows_total-bottomRow)*24
```
Sticky header `overflow-x:auto` synced with the body's horizontal scroll; left gutter = row numbers, sticky.
**Degraded fallback** if it janks on the GTX-660 / 8 GB target: a paged static table (first ~200–300 rows +
"next page") that touches no other flow.

### 7.4 Console render + `.do` replay + busy state
Append-only blocks, monospace `<pre>` preserves alignment, auto-scroll unless the user scrolled up (jump-to-latest
pill). `.do` replay = iterate the returned `blocks[]` in order, stopping at the `error` block (yellow STOP bar).
During any in-flight command the input + Run are disabled (opacity .5) and re-enabled on response; the caret
returns to the input after every action.

---

## 8. Build sequence (frontend slice — maps onto the backend plan's D1–D7)

The backend plan already sequences kernel work D1–D7. The **frontend-specific order**, risk-first:

1. **D2 — `formatEstimation()` FIRST, against goldens.** Before any layout polish, build the coeftable formatter
   and verify it against **≥4 real `cobra --tidy` JSON outputs**: plain `ols`, `feols ,robust` (HC1), one with
   factor levels (`i.sex` → `1.sex`), one clustered (the adjusted-SE note), and one multi-equation
   (`reg3`/`heckman`, to exercise `outcomes[]`). **This is the true critical path** — an "almost Stata" table
   breaks the whole promise. Multi-eq may degrade to first-equation if time is short.
2. **D3 — the shell** (`index.html` + tokens + the 5×3 grid; `console.js`; `app.js`; `api.js`). **On day one,
   prototype the overlay + a scrolling console together** to flush out the gap-as-gridline seam risk (§9).
3. **D4 — `grid.js`** virtualization + the browse overlay (Esc to close); `panels.js` Variables/Properties/History.
4. **D5 — `.do` replay** wiring (POST `/api/do`, stop-block render), the rejection block, mutation-driven varlist
   refresh.
5. **D6 — polish**: number formatting, the keyboard set (recall, `Tab` completion, `Ctrl/⌘+L`, jump-to-latest),
   estimator long-tail tables.
6. **D7 — smoke-test inside the real pywebview window** (not a browser tab): load → browse → regress → `.do`.

---

## 9. Risks & mitigations (from the judge panel)

1. **Coeftable formatter is THE critical path (all 3 judges).** Mitigation = §8 step 1: golden-first, degrade
   multi-eq to a flat table rather than chase nested alignment.
2. **gap-as-black-gridline can crack** when a pane gets internal scroll or the browse overlay z-stacks (hairline
   white seams / doubled rules on WKWebView/WebView2). Mitigation: prototype overlay + scrolling console **day
   one**; keep the explicit-`border` fallback with a shared-edge convention ready; verify the 2 px line stays
   crisp (no anti-alias blur) on both webviews. Keep the browser a full-region **overlay**, never a 5th track.
3. **Virtual-grid jank** on the GTX-660 Windows box and 8 GB M1. Mitigation: hard-cap DOM ≤100 rows, 24 px fixed
   height, and ship the paged-static fallback (this is the backend plan's cut #3 — honor it).
4. **Post-estimation depends on a backend contract**: `test`/`lincom`/`margins`/`predict` need the kernel to cache
   `lastModel` and resolve them server-side. Confirm the kernel persists it — else the highest-value Stata
   follow-ups silently fail.
5. **varlist staleness**: Tab-completion + the Variables/Properties panels read `appState.varlist`; **refresh it
   from `varlist_after` on every mutation** or the panel mis-paints and completion breaks.
6. **Scope creep**: NO do-file editor window, NO resizing secondary-tab area in v0. `.do` = `File ▸ Run .do` →
   native picker → `POST /api/do` → replay into the same console.
7. **pywebview file dialog** (`create_file_dialog`) is the only OS integration — **smoke-test in the real window
   on D7**, since browser-tab dev (D3) masks file-picker and webview behavior.

---

## 10. v0 cut list (deliberately NOT building)

Graphics (any) · loops/macros (`foreach/forvalues/while/local/global`) · `program define` · RNG/`simulate`/
`bootstrap` · multi-equation pretty-display (flatten to first eq) · `matrix`/`scalar` · grid cell selection/copy ·
grid find/filter · dark-mode toggle (tokens exist) · advanced keyboard beyond the set in §3.2 · WS streaming
(`/api/do` is synchronous) · PyInstaller `.app/.exe` (post-Wed) · Windows `cobra` baseline build & `/tmp`
workaround (post-Wed, see backend plan).

All rejections return a named `error` block — **never silently wrong** (the lexing-stage detect-and-reject
contract + the precise rejected-construct set live in §6.1, grounded in the internal language notes).

---

## 11. Open questions for the kernel (frontend is blocked-soft on these)
1. Relative vs absolute file paths in `use`/`open` (resolve against `workdir`?).
2. `--keepif` temp-hbin lifecycle (persist / in-memory / auto-clean?).
3. Factor-level truncation in `varlist`/`browse` (full vs first-N for JSON size).
4. Does the kernel actually cache `lastModel` and route `test/margins/predict/lincom` to it? (risk #4).
5. Temp-hbin cleanup on close / new-open.
6. UTF-8 varnames/filenames on Windows.
7. Does `cobra` expose `version --json` (cobra + `contract_version`) for the §6.2 boot handshake, and does the kernel enforce it on mismatch?

---

## 12. Provenance
Synthesized 2026-06-17 from a multi-agent design workflow: 5 parallel research agents (Stata UI reference model ·
CoBrA/Mondrian design tokens · the frontend↔kernel API contract read from `apps/cecon_cli.cpp` + `tools/hbin_read.py` ·
vanilla-JS engineering · comparable stats-GUI IA survey) → 3 competing concepts → a 3-judge panel
(Stata-familiarity · MVP feasibility · identity restraint). Winner: Concept 2 (2–1), with grafts from 1 & 3 and
the factual corrections in §0. Verified engine fields against `apps/cecon_cli.cpp`; hbin
dtypes (`f64`/`factor` only) against `tools/hbin_read.py`; branding against
the internal branding masters. The language layer (§6.1) is grounded in the internal language notes
(`lexing.md`, `macro.md`, `control-flow.md`, `program.md`,
`syntax-cmd.md`, `varlist-factor-ts.md`, `sample-prefix-expr.md`, `results.md`) — the same authority for the
Tier-A interpreter. The two-mode system (§1.4) was **reframed by the user (2026-06-17)**: the artistic De Stijl
workbench is the **default ("Mondrian" mode)** — leaned a touch further into the recognizable grid + flat
red/blue/yellow composition — and **"Simple" mode** is a deliberately boring Stata replica. An earlier
watercolor/early-Mondrian exploration was set aside for the UI; that energy now lives only in the user-made app
**icon** (§5), which is out of scope here.
