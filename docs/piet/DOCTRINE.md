# PIET DOCTRINE — supreme design law for the user-facing layer

**Status:** ratified by the User, 2026-06-22.
**Governance:** subordinate to the master Doctrine (the internal CONSTITUTION, Part 0).
**Scope:** PIET — the desktop app that puts cobra in front of students and researchers.

PIET inherits all four gates of the master Doctrine (and its Verification law). The three gates
below **specialize** them for the layer cobra never touches: the one a human looks at and uses.
Strict lexicographic order — satisfy each gate before optimizing the next. The PIET technical
specification ([SPEC.md](SPEC.md)) is **derived from** these principles and may never contradict them.

---

## Identity

**PIET is the conduit that makes cobra usable. cobra owns the truth; PIET owns how it is shown.**

---

## The gates

### Ⅰ. Engine-driven  *(specializes master ① — Identical to Stata)*

PIET is a clear, precise, and honest **mirror of what cobra says** — nothing more, nothing
less, nothing invented. It reflects every result faithfully, and renders the engine's output
in words a student or researcher reads and understands at a glance. What the engine cannot do,
PIET does not fake — it says so plainly.

### Ⅱ. Lightness  *(specializes master ③ — Lightest)*

There is no unbearable lightness here. PIET runs as light as possible for those without the
best computers — that is the floor, never an excuse for jank. And the twist: when the machine
is capable, PIET uses it fully. It adapts to the hardware in **both directions** — never too
heavy for the weak, never wasteful of the strong.

### Ⅲ. Precision  *(new — the layer the engine never sees)*

Only what belongs is present; and it sits exactly where it belongs. Don't be an amateur. The
UI/UX is pristine — every element aligned with intent, for the user's convenience and for
design purpose (most of all in De Stijl).

---

## Two faces — orthogonal to the gates

The two display modes are two skins over the same structure. Switching mode changes nothing
about Ⅰ·Ⅱ·Ⅲ — **both modes must pass all three gates.** (Orthogonal axes: De Stijl/Boring ×
Light/Dark.)

### ◆ De Stijl — the aspiration

*If Mondrian, van Doesburg, and Rietveld had designed the app.*

Reduction to essentials; structure made visible — the line is logic, not ornament; the primary
palette as a sparing accent, never laid behind data or text; asymmetric, dynamic balance; a
strict orthogonal grid. Every element justified. The values pursued without compromise:
**simplicity, creativity, visibility, practicality.**

### ◆ Boring — the bridge

*A Stata user knows it the moment they see it.*

The same mental model — console, results log, variables, data browser — the same output shapes,
zero relearning. **Recognizability over flair.** This is the default that lowers the barrier to
adoption: the mode that gets PIET into a classroom.

---

## Order of authority

These principles bind first. The PIET technical specification — rendering, contract, command
coverage, the resource governor, prefetch, design tokens — derives from them and may never
contradict them. When a technical choice and a gate conflict, the gate wins.

## Enforcement (updated 2026-07-07, post-2.0-GA postmortem)

Conductor builds to this doctrine. Self-gates: `piet_native --selftest` (headless legs) + the
**packaged-app user journey** — `--ui-smoke` (scripted: bridge → varlist → command → rendered
block → kernel-dot GREEN) or the User launch-checklist. **Eval independently gates the package**
(piet2 corpus + random-check). Gate coverage follows the **delta**: a change to transport/UI/
packaging is never certified by engine-layer gates alone, and **"it renders" is never accepted as
"it works"** — a painted window proves nothing about the wiring behind it. A build that fails any
gate is not shipped.
