// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// panels.js — Variable Editor grid + History panel + Properties sub-panel.
// Variable grid: 3 display columns (glyph | Name | Label) with inline editing.
// Edits dispatch 'piet:var-edit' CustomEvents — app.js listens and calls the API.

import { trunc, fmtG, fmtObs } from './util.js';
import { getVarStats } from './varstats.js';
// N7 (P8.1-W2): panels.js has no app.js import (dispatches CustomEvents
// instead, see the file-header comment) — but api.js is a leaf module with
// no app.js dependency either, so instantiating its own HTTPClient here
// (same mock/native transport-detection idiom app.js's own client uses) is
// safe and keeps that "no app.js coupling" invariant intact.
import { HTTPClient } from './api.js';

// ─── DOM refs (set by initPanels) ─────────────────────────────────────────────
let _wrapEl     = null;  // #var-list-wrap — panels.js builds grid inside this
let _tbodyEl    = null;  // <tbody> inside the grid
let _searchEl   = null;  // search/filter <input>
let _propsEl    = null;  // #properties sub-panel
let _histListEl = null;  // <ul> inside #pane-history

const _client = new HTTPClient();

// N7: lazy per-var detail cache (client.varDetail(name) results), keyed by
// var name. Populated ONLY on PIN (_pinAndInsert below) — hover stays cheap,
// no fetch. Cleared whenever the varlist itself changes (updateVariables())
// since a stale detail for a since-renamed/dropped/reloaded var would be
// actively misleading. B.3.0 (bounded transcripts, contract_B 2.5.0): this
// is now ALSO the factor-`levels` cache — the varlist wire shape dropped
// inline `levels` (n_levels-only; a 108K-row factor's levels array was
// 2.3MB per mutation), so a fetched detail's `.levels` is the only source
// for the real level strings. Same cache, same per-dataset-state
// invalidation — see _renderCard()'s factor-levels block below.
let _detailCache = new Map();

let _varlist     = [];   // VarInfo[] — source of truth for the grid
let _selectedVar = null; // VarInfo | null — CLICK-pinned (B13)
let _hoverVar    = null; // VarInfo | null — transient HOVER preview (B13); takes
                          // priority over _selectedVar while set
let _sortCol     = '';   // '' | 'name' | 'label'
let _sortAsc     = true;
let _filterText  = '';

// A9 (P8.1): history filter — case-insensitive substring filter over
// #hist-list's rows, invisible until the 🔍 toggle in the History pane
// header is clicked. _histFilterText drives both re-filtering the existing
// rows (input handler) and filtering NEWLY appended rows while active
// (appendHistory below), so a running session stays consistent whether the
// filter was on before or after a given command ran.
let _histFilterText = '';

// ─── Init ─────────────────────────────────────────────────────────────────────

export function initPanels(wrapEl, propsEl, histListEl) {
  _wrapEl     = wrapEl;
  _propsEl    = propsEl;
  _histListEl = histListEl;
  _setupHistoryFilter();

  if (!wrapEl) return;
  wrapEl.innerHTML = '';

  // ── Search / filter input ───────────────────────────────────────────────────
  const searchWrap = document.createElement('div');
  searchWrap.className = 'var-search-wrap';
  _searchEl = document.createElement('input');
  _searchEl.type = 'text';
  _searchEl.className = 'var-search';
  _searchEl.placeholder = 'filter…';
  _searchEl.setAttribute('aria-label', 'Filter variables');
  _searchEl.addEventListener('input', () => {
    _filterText = _searchEl.value.toLowerCase();
    _renderRows();
  });
  searchWrap.appendChild(_searchEl);

  // ── Scroll container ─────────────────────────────────────────────────────────
  const scrollWrap = document.createElement('div');
  scrollWrap.className = 'var-grid-scroll';

  // ── Table ────────────────────────────────────────────────────────────────────
  const table = document.createElement('table');
  table.className = 'var-grid';
  table.setAttribute('role', 'grid');
  table.setAttribute('aria-label', 'Variables');

  const thead = document.createElement('thead');
  const hrow  = document.createElement('tr');

  const thG = document.createElement('th');
  thG.className = 'vg-th-glyph';
  thG.setAttribute('aria-hidden', 'true');

  const thN = document.createElement('th');
  thN.className = 'vg-th-name';
  thN.textContent = 'Name';
  thN.title = 'Sort by name';
  thN.addEventListener('click', () => _toggleSort('name'));

  const thL = document.createElement('th');
  thL.className = 'vg-th-label';
  thL.textContent = 'Label';
  thL.title = 'Sort by label';
  thL.addEventListener('click', () => _toggleSort('label'));

  hrow.appendChild(thG);
  hrow.appendChild(thN);
  hrow.appendChild(thL);
  thead.appendChild(hrow);

  _tbodyEl = document.createElement('tbody');
  table.appendChild(thead);
  table.appendChild(_tbodyEl);
  scrollWrap.appendChild(table);

  wrapEl.appendChild(searchWrap);
  wrapEl.appendChild(scrollWrap);
}

/**
 * A9 (P8.1): inject the invisible-until-needed 🔍 filter toggle into the
 * History pane header, and the (hidden-by-default) filter input row above
 * #hist-list. panels.js owns the History pane's DOM; index.html is not
 * touched — same JS-injection idiom as the Variables search box above.
 *
 * Many DOM-gate fixtures mount a bare `<ul id="hist-list">` with no
 * `#pane-history` wrapper / `.pane-header` (app.js's own paste/BUG-04x
 * suites — see dom_runner.mjs's buildAppDom()-style minimal doms), so this
 * must no-op cleanly whenever that header isn't found rather than assuming
 * production's index.html shape.
 */
function _setupHistoryFilter() {
  if (!_histListEl) return;
  const pane = _histListEl.closest('#pane-history');
  const header = pane ? pane.querySelector('.pane-header') : null;
  if (!header) return;   // fixture without the real History pane chrome — no-op

  const toggleBtn = document.createElement('button');
  toggleBtn.type = 'button';
  toggleBtn.className = 'hist-filter-toggle';
  toggleBtn.textContent = '🔍';
  toggleBtn.title = 'Filter history';
  toggleBtn.setAttribute('aria-label', 'Filter history');
  header.appendChild(toggleBtn);

  // Filter row: reuses .var-search-wrap/.var-search (Variables pane's own
  // search box classes) so no new CSS is needed for the input itself —
  // only .hist-filter-row need be listed in CSS-APPEND (display toggling).
  const row = document.createElement('div');
  row.className = 'var-search-wrap hist-filter-row';
  row.hidden = true;
  const input = document.createElement('input');
  input.type = 'text';
  input.className = 'var-search';
  input.placeholder = 'filter history…';
  input.setAttribute('aria-label', 'Filter history');
  row.appendChild(input);
  _histListEl.parentElement.insertBefore(row, _histListEl);

  function collapse() {
    row.hidden = true;
    input.value = '';
    _histFilterText = '';
    _applyHistoryFilter();
  }

  toggleBtn.addEventListener('click', () => {
    if (row.hidden) {
      row.hidden = false;
      input.focus();
    } else {
      collapse();
    }
  });

  input.addEventListener('input', () => {
    _histFilterText = input.value.toLowerCase();
    _applyHistoryFilter();
  });
  input.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      e.preventDefault();
      collapse();
    }
  });
}

/** Re-apply _histFilterText to every existing .hist-row (input handler,
 *  and the collapse() clear path above). New rows appended while a filter
 *  is active are filtered inline by appendHistory() instead of calling
 *  this (avoids an O(n) re-scan per command). */
function _applyHistoryFilter() {
  if (!_histListEl) return;
  const q = _histFilterText;
  for (const li of _histListEl.querySelectorAll('.hist-row')) {
    const cmd = li.dataset.cmd ?? '';
    li.classList.toggle('hist-hidden', q !== '' && !cmd.toLowerCase().includes(q));
  }
}

// ─── Variables panel (public) ─────────────────────────────────────────────────

export function updateVariables(varlist) {
  _varlist    = varlist ?? [];
  _selectedVar = null;
  _detailCache.clear();   // N7: varlist changed — a stale fetched detail would mislead
  updateProperties(null);
  _renderRows();
}

export function getVarlist() {
  return _varlist;
}

// ─── Grid rendering (internal) ───────────────────────────────────────────────

function _toggleSort(col) {
  if (_sortCol === col) {
    _sortAsc = !_sortAsc;
  } else {
    _sortCol = col;
    _sortAsc = true;
  }
  _renderRows();
}

function _renderRows() {
  if (!_tbodyEl) return;

  let rows = _varlist.filter(v => {
    if (!_filterText) return true;
    return v.name.toLowerCase().includes(_filterText) ||
           (v.label ?? '').toLowerCase().includes(_filterText);
  });

  if (_sortCol === 'name') {
    rows = [...rows].sort((a, b) =>
      _sortAsc ? a.name.localeCompare(b.name) : b.name.localeCompare(a.name)
    );
  } else if (_sortCol === 'label') {
    rows = [...rows].sort((a, b) => {
      const la = a.label ?? '';
      const lb = b.label ?? '';
      return _sortAsc ? la.localeCompare(lb) : lb.localeCompare(la);
    });
  }

  _tbodyEl.innerHTML = '';
  for (const v of rows) {
    _tbodyEl.appendChild(_makeRow(v));
  }
}

function _makeRow(v) {
  const tr = document.createElement('tr');
  tr.className = 'var-row';
  tr.dataset.varname = v.name;
  if (_selectedVar && _selectedVar.name === v.name) tr.classList.add('selected');

  // ── Glyph cell ───────────────────────────────────────────────────────────────
  const tdG = document.createElement('td');
  tdG.className = 'vg-glyph';
  const glyph = document.createElement('span');
  glyph.className = v.dtype === 'factor' ? 'var-glyph glyph-factor' : 'var-glyph glyph-numeric';
  glyph.textContent = v.dtype === 'factor' ? '◆' : '#';
  glyph.setAttribute('aria-hidden', 'true');
  tdG.appendChild(glyph);

  // ── Name cell (editable → rename) ────────────────────────────────────────────
  const tdN = document.createElement('td');
  tdN.className = 'vg-name';
  tdN.dataset.editable = 'rename';
  tdN.textContent = v.name;
  tdN.title = v.name + ' — click to rename';
  tdN.addEventListener('click', e => {
    e.stopPropagation();
    if (tdN.classList.contains('vg-cell-editing')) return;
    _startEdit(tdN, v.name, newVal => {
      if (newVal && newVal !== v.name) {
        _dispatch('rename', { varname: v.name, newname: newVal });
      }
    });
  });

  // ── Label cell (editable → label/variable) ───────────────────────────────────
  const tdL = document.createElement('td');
  tdL.className = 'vg-label';
  tdL.dataset.editable = 'label-variable';
  tdL.textContent = v.label ?? '';
  tdL.title = (v.label ?? '(no label)') + ' — click to edit';
  tdL.addEventListener('click', e => {
    e.stopPropagation();
    if (tdL.classList.contains('vg-cell-editing')) return;
    _startEdit(tdL, v.label ?? '', newVal => {
      if (newVal !== (v.label ?? '')) {
        _dispatch('label-variable', { varname: v.name, label: newVal });
      }
    });
  });

  // B13: hover = transient detail-card preview (does not disturb the pinned
  // selection — mouseleave just falls back to it). B8: row click (outside the
  // Name/Label edit cells, which stopPropagation their own clicks above) pins
  // the card AND inserts the bare variable name at the cursor.
  tr.addEventListener('mouseenter', () => { _hoverVar = v; _renderCard(); });
  tr.addEventListener('mouseleave', () => { _hoverVar = null; _renderCard(); });
  tr.addEventListener('click', () => _pinAndInsert(v));

  tr.appendChild(tdG);
  tr.appendChild(tdN);
  tr.appendChild(tdL);
  return tr;
}

function _dispatch(action, params) {
  document.dispatchEvent(new CustomEvent('piet:var-edit', { detail: { action, ...params } }));
}

function _startEdit(cell, currentValue, onCommit) {
  cell.classList.add('vg-cell-editing');

  const input = document.createElement('input');
  input.type  = 'text';
  input.value = currentValue;
  cell.textContent = '';
  cell.appendChild(input);
  input.focus();
  input.select();

  let done = false;

  function commit() {
    if (done) return;
    done = true;
    const val = input.value.trim();
    cell.classList.remove('vg-cell-editing');
    cell.textContent = val !== '' ? val : currentValue; // optimistic; updateVariables() overwrites
    onCommit(val);
  }

  function cancel() {
    if (done) return;
    done = true;
    cell.classList.remove('vg-cell-editing');
    cell.textContent = currentValue;
  }

  input.addEventListener('keydown', e => {
    if (e.key === 'Enter')  { e.preventDefault(); e.stopPropagation(); commit(); }
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); cancel(); }
    if (e.key === 'Tab')    { e.preventDefault(); commit(); }
  });
  // Design decision 7: blur = commit, Escape = cancel — mirrors grid.js's cell
  // editor (_beginEdit(): blur → finish(true) i.e. commit, Escape → finish(false)
  // i.e. cancel). commit() no-ops (no dispatch) when the value is unchanged —
  // see the onCommit callbacks in _makeRow() below.
  input.addEventListener('blur', commit);
}

/** B8 + B13: click = pin the card (the pin survives the next mouseleave) AND
 *  insert the bare variable name at the cursor. Shared by a row click and the
 *  card's own click (clicking the card inserts whichever var it's currently
 *  showing — the hover preview if hovering, else the existing pin) — "the
 *  click behavior of the card/row" is the SAME action either way. */
function _pinAndInsert(v) {
  _selectedVar = v;
  _hoverVar = null;   // a fresh pin supersedes any stale hover preview
  if (_tbodyEl) {
    for (const tr of _tbodyEl.querySelectorAll('.var-row')) {
      tr.classList.toggle('selected', tr.dataset.varname === v.name);
    }
  }
  _renderCard();
  _insertVarName(v.name);
  _maybeFetchVarDetail(v);   // N7: lazy client.varDetail() fallback — PIN only, not hover
}

/**
 * N7 (P8.1-W2): the B13 detail card falls back to a "run summarize" hint
 * when the passive summarize cache (varstats.js) has nothing for `v` yet.
 * On PIN, if there's still nothing cached AND a native bridge exists, fetch
 * a one-off client.varDetail() and re-render the card with it. Loading
 * state: the hint text stays exactly as-is until resolution (no spinner —
 * this is a quiet enhancement, not a promised feature). Error -> silent,
 * hint stays (never surfaces as an error block; a failed convenience fetch
 * is not worth interrupting the user over). Stale guard: only repaints if
 * `v` is STILL the pinned var when the fetch resolves — the user may have
 * clicked a different row while it was in flight.
 *
 * B.3.0 (bounded transcripts): also the fetch that resolves a factor var's
 * "N levels — click to load" placeholder (_renderCard() below) into real
 * level strings — `needsLevels` means the quick-stats-only skip (summarize
 * cache already covers `v`) no longer short-circuits a factor var that still
 * has no levels. One fetch, one cache, both purposes — the wire response
 * already carries whichever of stats/levels apply to `v`.
 */
async function _maybeFetchVarDetail(v) {
  const needsLevels = v.dtype === 'factor' && !v.levels?.length;
  if (getVarStats(v.name) && !needsLevels) return;   // summarize cache covers stats, and there are no levels left to chase
  if (_detailCache.has(v.name)) return;        // already fetched this session — _renderCard() above already used it
  if (!_client.native) return;                 // feature-detect: mock/browser -> no bridge to fetch from
  let detail = null;
  try {
    detail = await _client.varDetail(v.name);
  } catch {
    return;   // silent — enhancement only
  }
  if (!detail) return;
  _detailCache.set(v.name, detail);
  if (_selectedVar && _selectedVar.name === v.name) _renderCard();   // still pinned -> repaint with the fetched detail
}

// ─── Variable detail card (B13) — bottom of the Variables pane ──────────────
// Built into the existing #properties container (already the bottom,
// flex-shrink:0 sub-panel of #pane-vars — see index.html/piet.css). HOVER a
// row shows a transient preview (_hoverVar); CLICK pins it (_selectedVar) and
// survives the next mouseleave. Content: name · dtype/format (+ factor
// levels) · full label · quick-stats (B14, from the passive summarize cache
// in varstats.js, else a hint) · insert hint. Kept as the exported
// `updateProperties` entry point (unchanged name/signature — nothing outside
// panels.js calls it, but this is the one place that sets/clears the pin).

// B21-2 (User dogfood 07-16): the wire `dtype` string is PIET's internal
// storage vocabulary, not Stata's — a User reading "f64" in the card has no
// idiom to map it to. Verified against the actual emitters (there are only
// two): piet/native/src/datastore.cpp's `dtype()`/to_json always write
// either "f64" or "factor" (the engine-side type is literally `enum class
// DType { F64, Factor }` — src/io/columnar.h), and every mock fixture in
// api.js agrees (MOCK_VARLIST/MOCK_SUMMARIZE only ever use those two
// strings). So the real set is exactly {f64, factor} today — this table
// maps just those, Stata-idiom ("f64" is Stata's 8-byte `double`; PIET's
// "factor" is a numeric column with attached value labels, i.e. Stata's
// own "labeled variable" idiom). Declarative table, not a branch per type
// (IDIOMS.md "declarative tables over bespoke branches" — console.js's
// ERROR_HINTS_BY_CODE is the same idiom in this codebase). A dtype the
// engine adds later and this table hasn't caught up to falls back to its
// own raw string in _dtypeLabel() below, rather than showing nothing.
const DTYPE_DISPLAY = {
  f64:    'double',
  factor: 'factor (labeled)',
};

/** Stata-idiom label for a wire `dtype` string — DTYPE_DISPLAY hit, else the
 *  raw string itself (an unmapped future dtype is still visible, just
 *  unglossed) — B21-2. */
function _dtypeLabel(dtype) {
  return DTYPE_DISPLAY[dtype] ?? dtype ?? 'double';
}

export function updateProperties(v) {
  _selectedVar = v;
  _hoverVar = null;
  _renderCard();
}

function _fmtClock(ms) {
  const d = new Date(ms);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

/** `<span class="vc-k">key</span> value` as a DocumentFragment (textContent-
 *  built, no innerHTML — matches this module's existing DOM style). */
function _kvSpan(key, val) {
  const frag = document.createDocumentFragment();
  const k = document.createElement('span');
  k.className = 'vc-k';
  k.textContent = key;
  frag.appendChild(k);
  frag.appendChild(document.createTextNode(' ' + val));
  return frag;
}

/** One stats line: pairs joined by ' · ', e.g. "n 336 · mean .5133 · sd .4778". */
function _statLine(pairs) {
  const div = document.createElement('div');
  pairs.forEach(([key, val], i) => {
    if (i > 0) div.appendChild(document.createTextNode(' · '));
    div.appendChild(_kvSpan(key, val));
  });
  return div;
}

function _renderCard() {
  if (!_propsEl) return;
  const v = _hoverVar ?? _selectedVar;
  _propsEl.replaceChildren();
  _propsEl.onclick = null;
  _propsEl.title = '';

  if (!v) {
    _propsEl.classList.remove('vcard');
    return;
  }
  _propsEl.classList.add('vcard');

  // ── name · dtype/format ─────────────────────────────────────────────────
  const head = document.createElement('div');
  head.className = 'vc-head';
  const nm = document.createElement('span');
  nm.className = 'vc-nm';
  nm.textContent = v.name;
  const ty = document.createElement('span');
  ty.className = 'vc-ty';
  // B21-2: Stata-idiom name (DTYPE_DISPLAY) in place of the raw wire dtype —
  // "double" not "f64", "factor (labeled)" not "factor" — level count stays
  // alongside it, same ' · ' join the format string already used.
  ty.textContent = v.dtype === 'factor'
    ? `${_dtypeLabel(v.dtype)} · ${v.n_levels ?? '?'} levels`
    : `${_dtypeLabel(v.dtype)} · ${v.format ?? '%9.0g'}`;
  head.appendChild(nm);
  head.appendChild(ty);
  _propsEl.appendChild(head);

  // B.3.0 (bounded transcripts, contract_B 2.5.0): the varlist wire shape
  // dropped inline `levels` (n_levels-only — a 108K-row factor's levels
  // array was 2.3MB per mutation). v.levels?.length still wins when present
  // (back-compat with any path that still ships it inline); otherwise fall
  // back to the N7 detail cache's fetched `.levels` (the SAME cache
  // _maybeFetchVarDetail populates, above); with neither yet available, show
  // a compact placeholder — the card is already one big click target
  // (_propsEl.onclick, bottom of this function, and the row's own click —
  // both funnel through _pinAndInsert -> _maybeFetchVarDetail) so no
  // dedicated click handler is needed just for this.
  if (v.dtype === 'factor') {
    const fetchedLevels = _detailCache.get(v.name)?.levels;
    const levels = v.levels?.length ? v.levels : fetchedLevels;
    if (levels?.length) {
      const lv = document.createElement('div');
      lv.className = 'vc-levels';
      lv.textContent = levels.slice(0, 4).join(', ') + (levels.length > 4 ? ', …' : '');
      _propsEl.appendChild(lv);
    } else if (v.n_levels != null) {
      const lv = document.createElement('div');
      lv.className = 'vc-levels';
      lv.textContent = `${v.n_levels} levels — click to load`;
      _propsEl.appendChild(lv);
    }
  }

  // ── full label (never truncated — A20 precedent: the card is the place
  //    the varlist grid's own ellipsis blind spot gets resolved) ──────────
  if (v.label) {
    const lb = document.createElement('div');
    lb.className = 'vc-lb';
    lb.textContent = v.label;
    _propsEl.appendChild(lb);
  }

  // ── quick-stats (B14): from the passive summarize cache, else a lazy
  //    client.varDetail() fetch (N7, PIN-only — see _maybeFetchVarDetail),
  //    else the hint ────────────────────────────────────────────────────
  const stats = getVarStats(v.name);
  const detail = _detailCache.get(v.name);   // N7
  const statsEl = document.createElement('div');
  statsEl.className = 'vc-stats';
  if (stats) {
    statsEl.appendChild(_statLine([
      ['n', fmtObs(stats.N)],
      ['mean', fmtG(stats.mean) ?? '—'],
      ['sd', fmtG(stats.sd) ?? '—'],
    ]));
    statsEl.appendChild(_statLine([
      ['min', fmtG(stats.min) ?? '—'],
      ['max', fmtG(stats.max) ?? '—'],
    ]));
    _propsEl.appendChild(statsEl);

    const src = document.createElement('span');
    src.className = 'vc-src';
    src.textContent = `summarize 캐시 · ${_fmtClock(stats.cachedAt)}`;
    _propsEl.appendChild(src);
  } else if (detail && (detail.mean != null || detail.sd != null || detail.min != null || detail.max != null)) {
    // N7: same two-line shape as the summarize-cache branch, sourced from
    // var_detail's numeric moments instead (string columns omit these on
    // the wire, so this branch is skipped for them — falls to the hint below).
    statsEl.appendChild(_statLine([
      ['n', fmtObs(detail.N)],
      ['mean', fmtG(detail.mean) ?? '—'],
      ['sd', fmtG(detail.sd) ?? '—'],
    ]));
    statsEl.appendChild(_statLine([
      ['min', fmtG(detail.min) ?? '—'],
      ['max', fmtG(detail.max) ?? '—'],
    ]));
    _propsEl.appendChild(statsEl);

    const src = document.createElement('span');
    src.className = 'vc-src';
    src.textContent = 'var_detail';
    _propsEl.appendChild(src);
  } else {
    statsEl.classList.add('vc-stats-hint');
    statsEl.textContent = 'summarize를 실행하거나 설정에서 자동요약을 켜세요';
    _propsEl.appendChild(statsEl);
  }

  // N7: var_detail's own n_missing (fresher — a live bridge fetch) wins over
  // the varlist snapshot's when both exist; falls back to the varlist's
  // exactly as before when no detail was fetched (byte-identical to pre-N7).
  const nMissing = (detail && detail.n_missing != null) ? detail.n_missing : v.n_missing;
  if (nMissing) {
    const miss = document.createElement('div');
    miss.className = 'vc-missing';
    miss.textContent = `${nMissing} missing`;
    _propsEl.appendChild(miss);
  }

  // ── insert hint + click-to-insert on the card itself (mirrors row click) ─
  const hint = document.createElement('div');
  hint.className = 'vc-hint';
  hint.textContent = '클릭 = 커서 위치에 이름 삽입';
  _propsEl.appendChild(hint);

  _propsEl.title = `${v.name} — click to insert at cursor`;
  _propsEl.onclick = () => _pinAndInsert(v);
}

/**
 * B8: insert a variable's bare NAME (never verbs/options) at the caret —
 * into the do-file editor textarea if it's open AND focused, else the
 * command input (the default). panels.js already reaches into #cmd-input
 * directly elsewhere (appendHistory's history-row click, below) — this
 * follows that same established cross-module DOM idiom rather than adding a
 * new event/callback layer. Mirrors app.js's own caret-insert pattern (its
 * Tab varname-completion: stemBefore + match.name + after, setSelectionRange).
 */
function _insertVarName(name) {
  const doOverlay = document.getElementById('doedit-overlay');
  const doText    = document.getElementById('doedit-text');
  const useDoEditor = !!doOverlay?.classList.contains('open') && document.activeElement === doText;

  const target = useDoEditor ? doText : document.getElementById('cmd-input');
  if (!target) return;

  if (typeof target.setRangeText === 'function' && target.selectionStart != null) {
    const start = target.selectionStart;
    const end   = target.selectionEnd ?? start;
    target.setRangeText(name, start, end, 'end');
  } else {
    target.value = (target.value ?? '') + name;
  }
  target.focus();
}

// ─── History panel ───────────────────────────────────────────────────────────

export function appendHistory(cmd, success) {
  if (!_histListEl) return;

  const li   = document.createElement('li');
  li.className = 'hist-row';
  // A9: full (untruncated) command text, read back by _applyHistoryFilter()
  // and the inline filter-check below — .hist-cmd's own textContent is
  // trunc()'d to 30 chars, which would silently mismatch substrings past
  // the ellipsis.
  li.dataset.cmd = cmd;

  const mark = document.createElement('span');
  mark.className = success ? 'hist-mark mark-ok' : 'hist-mark mark-err';
  mark.textContent = success ? '✓' : '✕';
  mark.setAttribute('aria-label', success ? 'success' : 'failed');

  const text = document.createElement('span');
  text.className = 'hist-cmd';
  text.textContent = trunc(cmd, 30);
  text.title = cmd;

  // B6 (P8-S7): second verb, hover-revealed (piet.css's .hist-send-btn) —
  // "Send to Do-file Editor". Verb 1 (row click, below — unchanged) stays
  // insert-to-input; this is purely additive so it can never shadow that.
  // Max two verbs per row (brief). panels.js has no app.js import (mirrors
  // the existing piet:var-edit CustomEvent idiom just below in this file),
  // so this dispatches rather than calling into app.js directly.
  const sendBtn = document.createElement('button');
  sendBtn.type = 'button';
  sendBtn.className = 'hist-send-btn';
  sendBtn.textContent = '→ Do-file';
  sendBtn.title = 'Send to Do-file Editor';
  sendBtn.setAttribute('aria-label', 'Send to Do-file Editor');
  sendBtn.addEventListener('click', e => {
    e.stopPropagation();   // don't ALSO fire the row's insert-to-input click
    document.dispatchEvent(new CustomEvent('piet:hist-send-doedit', { detail: { cmd } }));
  });

  li.appendChild(mark);
  li.appendChild(text);
  li.appendChild(sendBtn);
  li.addEventListener('click', () => {
    const inputEl = document.getElementById('cmd-input');
    if (inputEl) { inputEl.value = cmd; inputEl.focus(); }
  });

  // A9: a new row respects the currently active filter immediately (rather
  // than briefly flashing visible until the next keystroke re-filters).
  if (_histFilterText !== '' && !cmd.toLowerCase().includes(_histFilterText)) {
    li.classList.add('hist-hidden');
  }

  _histListEl.appendChild(li);
  while (_histListEl.children.length > 50) {
    _histListEl.removeChild(_histListEl.firstChild);
  }
}

export function loadHistory(history) {
  if (!_histListEl) return;
  _histListEl.innerHTML = '';
  for (const h of history) appendHistory(h.cmd, h.success);
}
