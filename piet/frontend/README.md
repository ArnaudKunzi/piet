# PIET frontend — vanilla HTML/JS SPA (owned by the `piet-frontend` session)

Build per **`../../docs/piet/FRONTEND.md`**, consuming the frozen Boundary B in the internal `CONTRACT.md`.
**Own `piet/frontend/**` only.** Do not edit the contract; Boundary-B questions → the planner (the coordinating
session).

**Two modes**, toggled by `data-mode` (default **Boring** = plain Stata replica; **De Stijl** = workbench with a
*subtle* recognizable-Mondrian touch). See FRONTEND.md §1.4. Watercolor is out of the UI (it's the app icon).

**Start:** build `formatEstimation()` against the **REAL** `../fixtures/golden/*.json` (no kernel) — it's the
critical path. **Then** the static shell (`index.html` + `css/piet.css`) openable in a browser against the mock
`../fixtures/api/*.json`. Vanilla ES modules, **no framework, no build step**.

**Layout to create:** `index.html`, `css/piet.css`, `js/{api,app,console,grid,panels,util}.js`.
