// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// panes.js — drag-resizable pane handles (col 2 = left rule, col 4 = right rule).
// Updates --col-vars / --col-history on #app so the Mondrian grid adapts live.

const MIN_VARS    = 80;
const MIN_HISTORY = 80;

export function initPanes() {
  const app = document.getElementById('app');
  if (!app) return;

  // Restore persisted widths
  const sv = localStorage.getItem('piet-col-vars');
  const sh = localStorage.getItem('piet-col-history');
  if (sv) app.style.setProperty('--col-vars',    sv);
  if (sh) app.style.setProperty('--col-history', sh);

  // Left resizer: between vars (col 1) and console (col 3).
  // BUG-094 (P8.1-T1): gridRow '3' — the grab zone exists ONLY alongside the
  // content row. '1 / 6' made the invisible strip span the toolbar (row 1)
  // and statusbar (row 5), flipping the cursor to col-resize over them and
  // swallowing clicks on toolbar controls near the divider.
  const rL = _makeResizer('pane-resizer-left');
  rL.style.gridColumn = '2';
  rL.style.gridRow    = '3';
  app.appendChild(rL);

  // Right resizer: between console (col 3) and history (col 5)
  const rR = _makeResizer('pane-resizer-right');
  rR.style.gridColumn = '4';
  rR.style.gridRow    = '3';
  app.appendChild(rR);

  _bindDrag(rL, app, '--col-vars',    +1);
  _bindDrag(rR, app, '--col-history', -1);
}

function _makeResizer(id) {
  const el = document.createElement('div');
  el.className = 'pane-resizer';
  el.id = id;
  el.setAttribute('aria-hidden', 'true');
  el.setAttribute('role', 'separator');
  return el;
}

function _bindDrag(handle, app, prop, dir) {
  handle.addEventListener('pointerdown', e => {
    e.preventDefault();
    handle.setPointerCapture(e.pointerId);

    const startX   = e.clientX;
    const startVal = _getPx(app, prop);
    // A6-05: coalesce the style write to one per animation frame (mirrors
    // grid.js's own scroll-handler RAF pattern) — pointermove can fire far
    // more often than layout needs to recompute the CSS-grid track sizes.
    let raf = null;
    let pendingVal = null;

    function onMove(ev) {
      const delta = dir * (ev.clientX - startX);
      const min   = prop === '--col-vars' ? MIN_VARS : MIN_HISTORY;
      pendingVal  = Math.max(min, startVal + delta);
      if (raf) return;
      raf = requestAnimationFrame(() => {
        raf = null;
        app.style.setProperty(prop, `${pendingVal}px`);
      });
    }

    function onUp() {
      if (raf) { cancelAnimationFrame(raf); raf = null; }
      if (pendingVal != null) app.style.setProperty(prop, `${pendingVal}px`);
      const val = app.style.getPropertyValue(prop);
      const key = prop === '--col-vars' ? 'piet-col-vars' : 'piet-col-history';
      if (val) localStorage.setItem(key, val);
      handle.removeEventListener('pointermove', onMove);
      handle.removeEventListener('pointerup',   onUp);
    }

    handle.addEventListener('pointermove', onMove);
    handle.addEventListener('pointerup',   onUp);
  });
}

function _getPx(app, prop) {
  const inline = app.style.getPropertyValue(prop);
  if (inline) return parseFloat(inline);
  return parseFloat(getComputedStyle(app).getPropertyValue(prop));
}
