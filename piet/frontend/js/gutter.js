// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// gutter.js — Do-file Editor line-number gutter (T4, P8.1).
// Self-contained ES module; no index.html edit — the gutter DOM is built
// here at init time, same panes.js-style JS-injection idiom (panes.js
// builds the pane-resizer handles the same way). NOT wired into app.js by
// this lane (Lane β owns console.js/panels.js/gutter.js only, never
// app.js/index.html) — see the INTEGRATION block in the sprint report for
// the exact lines the operator adds to app.js.

const STORAGE_KEY = 'piet-editor-gutter';

let _ta       = null;   // #doedit-text
let _gutterEl = null;   // #doedit-gutter — the injected line-number column
let _lineEls  = [];     // one <span> per rendered line number, in order
let _errLine  = null;   // 1-based line number currently highlighted, or null

function _enabled() {
  // Absent = ON; only an explicit '0' turns it off.
  return localStorage.getItem(STORAGE_KEY) !== '0';
}

function _lineCount() {
  if (!_ta) return 1;
  const m = _ta.value.match(/\n/g);
  return (m ? m.length : 0) + 1;
}

function _renderLines() {
  if (!_gutterEl || !_ta) return;
  const n = _lineCount();
  if (_lineEls.length === n) return;   // cheap common case: line count unchanged mid-edit

  _gutterEl.textContent = '';
  _lineEls = [];
  const frag = document.createDocumentFragment();
  for (let i = 1; i <= n; i++) {
    const span = document.createElement('span');
    span.className = 'gutter-line';
    span.textContent = String(i);
    frag.appendChild(span);
    _lineEls.push(span);
  }
  _gutterEl.appendChild(frag);

  // A rebuild just wiped whatever span carried the error highlight — re-apply
  // it against the fresh set of spans if one was active.
  if (_errLine != null) setGutterErrorLine(_errLine);
}

/**
 * Build and wire the gutter beside #doedit-text. Idempotent (a second call
 * is a no-op if already initialized) and gracefully does nothing if the
 * editor textarea isn't in the DOM yet (e.g. called before index.html's
 * doedit-overlay markup exists) or if the user has the feature off.
 *
 * DOM approach (per the brief's own uncertainty about wrappers): #doedit-
 * overlay is `display:flex; flex-direction:column` (piet.css) with
 * #doedit-text the sole flex:1 content row — there is no existing flex-ROW
 * ancestor to lay a gutter beside the textarea. Rather than touch piet.css
 * (out of Lane β's file ownership) or editing index.html (also out of
 * scope — DOM must be built here), this wraps gutter+textarea in ONE new
 * flex-row container (#doedit-editor-row), MOVING the existing #doedit-text
 * node inside it (insertBefore + appendChild on the same node — the
 * textarea keeps its identity, value, listeners, everything; it is not
 * cloned or recreated). This is the do-file editor surface, not
 * #console-body, so the "no wrapper divs" rule (constraint (a), which is
 * scoped specifically to console.js's block bookkeeping / Save Results /
 * elision marker) does not apply here. Verified against the DOM gate: no
 * fixture in dom_runner.mjs asserts #doedit-text's parent element, so this
 * restructuring is safe. gutter.js is not wired into app.js by this lane
 * (see INTEGRATION in the report), so none of this runs yet in the shipped
 * app or the gate — it activates once the operator adds the one-line
 * initGutter() call.
 */
export function initGutter() {
  if (_gutterEl) return;               // already initialized
  if (!_enabled()) return;             // '0' in localStorage → true no-op, nothing built, nothing to hide

  const ta = document.getElementById('doedit-text');
  if (!ta) return;
  const parent = ta.parentElement;
  if (!parent) return;

  const row = document.createElement('div');
  row.id = 'doedit-editor-row';

  const gutter = document.createElement('div');
  gutter.id = 'doedit-gutter';
  gutter.setAttribute('aria-hidden', 'true');

  parent.insertBefore(row, ta);
  row.appendChild(gutter);
  row.appendChild(ta);   // moves the existing textarea node — same element, same value/listeners

  _ta = ta;
  _gutterEl = gutter;

  _ta.addEventListener('input', _renderLines);
  _ta.addEventListener('scroll', () => {
    _gutterEl.scrollTop = _ta.scrollTop;
  });

  _renderLines();
}

/** Highlight line `n` (1-based) in the gutter, clearing any previous
 *  highlight. No-op if the gutter isn't initialized (feature off, or
 *  initGutter() hasn't run/found the textarea yet). */
export function setGutterErrorLine(n) {
  _errLine = n;
  if (!_gutterEl) return;
  for (const span of _lineEls) span.classList.remove('gutter-err-line');
  const idx = n - 1;
  if (idx >= 0 && idx < _lineEls.length) {
    _lineEls[idx].classList.add('gutter-err-line');
  }
}

/** Clear the error-line highlight (e.g. once the user fixes/re-runs). */
export function clearGutterErrorLine() {
  _errLine = null;
  if (!_gutterEl) return;
  for (const span of _lineEls) span.classList.remove('gutter-err-line');
}
