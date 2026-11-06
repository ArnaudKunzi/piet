// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// find.js — B11 ⌘F find bar (+ B12 replace in the do-file editor).
//
// Design law (UIUX-P8 §2/§3 `.find`, mockup: internal capture UIUX-P8-mockup-v3-pipboy.html):
// summoned ONLY by Cmd/Ctrl+F (R1 — not persistent chrome; W1 — not a command
// palette). Two surfaces, two behaviors:
//   Results (#console-body)      → FIND-ONLY. The results log is immutable,
//     so matches are highlighted IN PLACE (<mark class="find-hit">, current =
//     also .find-hit-current) and UNWRAPPED byte-for-byte on close — no
//     <mark> may survive close(), the log DOM must come back exactly as it was.
//   Do-file editor (#doedit-text) → FIND + REPLACE. Operates on the
//     textarea's string value directly (setSelectionRange for the current
//     match); Replace All = B12 "선택한 것 다같이 바꾸기".
//
// Active-surface detection is self-contained (checks .open/[hidden] on the
// overlay elements directly) — this module has no dependency on app.js's
// private OVERLAYS registry. app.js only needs to: call initFind() once at
// boot, wire Cmd/Ctrl+F → openFind(), and let its existing Escape handler
// check isFindOpen()/closeFind() BEFORE its own overlay-close sweep (see
// app.js's "Global keyboard shortcuts" block and its Escape handler).

const MAX_HITS = 2000;   // P7-style guard: never wrap/scan unbounded matches in a runaway log
const DEBOUNCE_MS = 120;

let _mode = null;        // 'results' | 'doedit' | null (closed)
let _query = '';
let _caseSensitive = false;
let _matches = [];       // results: <mark> elements in doc order; doedit: [start,end] pairs
let _current = -1;
let _prevFocusEl = null;
let _debounceTimer = null;

// DOM refs, filled by initFind(). Every use below is null-guarded — the DOM
// test harness (dom_runner.mjs's buildAppDom()) boots app.js's init() against
// a minimal fixture DOM that omits these ids, same as every other
// getElementById in app.js's own init().
let els = null;

export function initFind() {
  els = {
    consoleBody:   document.getElementById('console-body'),
    doeditText:    document.getElementById('doedit-text'),
    doeditOverlay: document.getElementById('doedit-overlay'),

    resBar:   document.getElementById('find-bar-results'),
    resQ:     document.getElementById('find-input-results'),
    resCnt:   document.getElementById('find-cnt-results'),
    resPrev:  document.getElementById('find-prev-results'),
    resNext:  document.getElementById('find-next-results'),
    resCase:  document.getElementById('find-case-results'),
    resClose: document.getElementById('find-close-results'),

    doBar:    document.getElementById('find-bar-doedit'),
    doQ:      document.getElementById('find-input-doedit'),
    doCnt:    document.getElementById('find-cnt-doedit'),
    doPrev:   document.getElementById('find-prev-doedit'),
    doNext:   document.getElementById('find-next-doedit'),
    doCase:   document.getElementById('find-case-doedit'),
    doRep:    document.getElementById('find-replace-doedit'),
    doRepOne: document.getElementById('find-replace-one-doedit'),
    doRepAll: document.getElementById('find-replace-all-doedit'),
    doClose:  document.getElementById('find-close-doedit'),
  };

  _wireSurface('results');
  _wireSurface('doedit');

  if (els.doRepOne) els.doRepOne.addEventListener('click', () => _replaceOne());
  if (els.doRepAll) els.doRepAll.addEventListener('click', () => _replaceAll());
  if (els.doRep) {
    els.doRep.addEventListener('keydown', e => {
      if (e.key === 'Enter') { e.preventDefault(); _replaceOne(); }
    });
  }
}

function _wireSurface(mode) {
  const q     = mode === 'results' ? els.resQ     : els.doQ;
  const prev  = mode === 'results' ? els.resPrev  : els.doPrev;
  const next  = mode === 'results' ? els.resNext  : els.doNext;
  const kase  = mode === 'results' ? els.resCase  : els.doCase;
  const close = mode === 'results' ? els.resClose : els.doClose;

  if (q) {
    q.addEventListener('input', () => {
      clearTimeout(_debounceTimer);
      _debounceTimer = setTimeout(() => _runSearch(mode, q.value), DEBOUNCE_MS);
    });
    q.addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        e.preventDefault();
        clearTimeout(_debounceTimer);
        _runSearch(mode, q.value);
        _goto(mode, e.shiftKey ? -1 : 1);
      }
      // Escape is NOT handled here — app.js's document-level Escape handler
      // checks isFindOpen()/closeFind() first (ahead of its OVERLAYS sweep),
      // so find-close works no matter which find-bar control has focus.
    });
  }
  if (prev)  prev.addEventListener('click', () => _goto(mode, -1));
  if (next)  next.addEventListener('click', () => _goto(mode, 1));
  if (kase) {
    kase.addEventListener('click', () => {
      _caseSensitive = !_caseSensitive;
      kase.setAttribute('aria-pressed', String(_caseSensitive));
      _runSearch(mode, q ? q.value : _query);
    });
  }
  if (close) close.addEventListener('click', () => closeFind());
}

// ─── Open / close ───────────────────────────────────────────────────────────

/** Which surface Cmd/Ctrl+F should target. null = no sensible target — some
 *  other overlay (graphics/about/settings/data-browser/request-dialog) is
 *  covering both Results and the editor; out of B11/B12's defined scope. */
function _activeSurface() {
  if (els.doeditOverlay?.classList.contains('open')) return 'doedit';
  const blockedIds = ['graphics-overlay', 'about-overlay', 'settings-overlay', 'request-overlay'];
  if (blockedIds.some(id => document.getElementById(id)?.classList.contains('open'))) return null;
  const browser = document.getElementById('browser-overlay');
  if (browser && !browser.hidden) return null;
  return els.consoleBody ? 'results' : null;
}

export function openFind() {
  if (!els) return;
  const mode = _activeSurface();
  if (!mode) return;
  if (_mode && _mode !== mode) closeFind();   // switching surface: close the stale bar first

  const bar = mode === 'results' ? els.resBar : els.doBar;
  const q   = mode === 'results' ? els.resQ   : els.doQ;
  if (!bar || !q) return;

  // Capture "where focus was" only if it wasn't already inside a find bar
  // (re-pressing ⌘F while already typing in it must not clobber the anchor).
  const activeEl = document.activeElement;
  const insideFind = els.resBar?.contains(activeEl) || els.doBar?.contains(activeEl);
  if (!insideFind) _prevFocusEl = activeEl;

  _mode = mode;
  bar.hidden = false;
  bar.classList.add('open');

  const sel = _selectionText(mode);
  if (sel) q.value = sel;
  q.focus();
  q.select();
  _runSearch(mode, q.value);
}

export function closeFind() {
  if (!_mode) return;
  const mode = _mode;
  const bar = mode === 'results' ? els.resBar : els.doBar;
  if (mode === 'results') _unwrapResults();
  if (bar) { bar.classList.remove('open'); bar.hidden = true; }
  _matches = [];
  _current = -1;
  _mode = null;

  const restore = (_prevFocusEl && document.contains(_prevFocusEl) && typeof _prevFocusEl.focus === 'function')
    ? _prevFocusEl
    : (mode === 'doedit' ? els.doeditText : document.getElementById('cmd-input'));
  restore?.focus();
  _prevFocusEl = null;
}

export function isFindOpen() { return _mode !== null; }

function _selectionText(mode) {
  if (mode === 'doedit') {
    const ta = els.doeditText;
    if (ta && ta.selectionEnd > ta.selectionStart) return ta.value.slice(ta.selectionStart, ta.selectionEnd);
    return '';
  }
  const sel = typeof window !== 'undefined' && window.getSelection ? window.getSelection() : null;
  const text = sel ? sel.toString() : '';
  return text && els.consoleBody?.contains(sel.anchorNode) ? text : '';
}

// ─── Search ─────────────────────────────────────────────────────────────────

function _runSearch(mode, query) {
  _query = query || '';
  if (mode === 'results') _searchResults(); else _searchDoedit();
}

function _searchResults() {
  _unwrapResults();
  _matches = [];
  if (_query && els.consoleBody) {
    const nodes = [];
    _collectTextNodes(els.consoleBody, nodes);
    for (const node of nodes) {
      if (_matches.length >= MAX_HITS) break;
      _wrapTextNode(node);
    }
  }
  _current = _matches.length ? 0 : -1;
  _applyResultsCurrent();
  _updateCount('results');
}

function _collectTextNodes(root, out) {
  for (const node of root.childNodes) {
    if (node.nodeType === 3) out.push(node);         // Text
    else if (node.nodeType === 1) _collectTextNodes(node, out);  // Element
  }
}

/** Split one text node around every match, wrapping each in <mark
 *  class="find-hit">, and splice the fragment in place of the original node. */
function _wrapTextNode(node) {
  const text = node.nodeValue;
  const hay    = _caseSensitive ? text : text.toLowerCase();
  const needle = _caseSensitive ? _query : _query.toLowerCase();
  if (!needle || !hay.includes(needle)) return;

  const frag = document.createDocumentFragment();
  let last = 0, idx;
  while (_matches.length < MAX_HITS && (idx = hay.indexOf(needle, last)) !== -1) {
    if (idx > last) frag.appendChild(document.createTextNode(text.slice(last, idx)));
    const mark = document.createElement('mark');
    mark.className = 'find-hit';
    mark.textContent = text.slice(idx, idx + needle.length);
    frag.appendChild(mark);
    _matches.push(mark);
    last = idx + needle.length;
  }
  if (last < text.length) frag.appendChild(document.createTextNode(text.slice(last)));
  node.parentNode.replaceChild(frag, node);
}

/** Byte-restore #console-body: unwrap every <mark class="find-hit"> back to
 *  plain text, then normalize() so the split text nodes re-merge. The
 *  results log is immutable — no <mark> may survive close() or a re-search. */
function _unwrapResults() {
  const body = els?.consoleBody;
  if (!body) return;
  body.querySelectorAll('mark.find-hit').forEach(mark => {
    mark.replaceWith(document.createTextNode(mark.textContent));
  });
  body.normalize();
}

function _computeDoeditMatches() {
  const ta = els.doeditText;
  const matches = [];
  if (ta && _query) {
    const text = ta.value;
    const hay    = _caseSensitive ? text : text.toLowerCase();
    const needle = _caseSensitive ? _query : _query.toLowerCase();
    let last = 0, idx;
    while (matches.length < MAX_HITS && (idx = hay.indexOf(needle, last)) !== -1) {
      matches.push([idx, idx + needle.length]);
      last = idx + needle.length;
    }
  }
  return matches;
}

function _searchDoedit() {
  _matches = _computeDoeditMatches();
  _current = _matches.length ? 0 : -1;
  _applyDoeditCurrent();
  _updateCount('doedit');
}

// ─── Navigation / emphasis ──────────────────────────────────────────────────

function _goto(mode, delta) {
  if (!_matches.length) return;
  _current = (_current + delta + _matches.length) % _matches.length;
  if (mode === 'results') _applyResultsCurrent(); else _applyDoeditCurrent();
  _updateCount(mode);
}

function _applyResultsCurrent() {
  _matches.forEach(m => m.classList.remove('find-hit-current'));
  if (_current >= 0) {
    const cur = _matches[_current];
    cur.classList.add('find-hit-current');
    cur.scrollIntoView?.({ block: 'center' });
  }
}

/** Select the current match in the textarea and approximate-scroll it into
 *  view (textareas have no native "scroll to selection" API — same technique
 *  as app.js's selectEditorLine()). */
function _applyDoeditCurrent() {
  const ta = els.doeditText;
  if (!ta || _current < 0 || !_matches[_current]) return;
  const [start, end] = _matches[_current];
  ta.focus();
  ta.setSelectionRange(start, end);
  const lineHeight = parseFloat(getComputedStyle(ta).lineHeight) || 16;
  const lineIdx = ta.value.slice(0, start).split('\n').length - 1;
  ta.scrollTop = Math.max(0, (lineIdx - 3) * lineHeight);
}

function _updateCount(mode) {
  const cnt = mode === 'results' ? els.resCnt : els.doCnt;
  if (!cnt) return;
  cnt.textContent = _matches.length ? `${_current + 1}/${_matches.length}` : '0/0';
}

// ─── Replace (B12, do-file editor only — "선택한 것 다같이 바꾸기") ────────

function _replaceOne() {
  if (_mode !== 'doedit' || _current < 0 || !_matches[_current]) return;
  const ta = els.doeditText;
  const rep = els.doRep?.value ?? '';
  const [start, end] = _matches[_current];
  ta.value = ta.value.slice(0, start) + rep + ta.value.slice(end);
  const caretAt = start + rep.length;

  _matches = _computeDoeditMatches();
  const nextIdx = _matches.findIndex(([s]) => s >= caretAt);
  _current = nextIdx !== -1 ? nextIdx : (_matches.length ? 0 : -1);
  if (_current < 0) { ta.focus(); ta.setSelectionRange(caretAt, caretAt); }
  else _applyDoeditCurrent();
  _updateCount('doedit');
}

function _replaceAll() {
  if (_mode !== 'doedit' || !_matches.length) return;
  const ta = els.doeditText;
  const rep = els.doRep?.value ?? '';
  let out = '', last = 0;
  for (const [start, end] of _matches) {
    out += ta.value.slice(last, start) + rep;
    last = end;
  }
  out += ta.value.slice(last);
  ta.value = out;

  _matches = _computeDoeditMatches();
  _current = _matches.length ? 0 : -1;
  _applyDoeditCurrent();
  _updateCount('doedit');
}
