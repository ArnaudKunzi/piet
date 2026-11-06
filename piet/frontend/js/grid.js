// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// grid.js — Virtualized data-browser grid (the Data Browser overlay).
// Keeps ≤ ~100 DOM rows via spacer divs, REUSING those row nodes across scroll
// frames (rebuilt in place, not torn down + recreated every frame); row height
// is fixed at 24 px. Fetches 300-row windows from GET /api/browse, caches ≤ 3.

// A3-07: 24px was hardcoded here AND in piet.css's `.browser-cell { height }` —
// two sources of truth that could silently drift. This is now only the
// fallback default (used when no stylesheet is loaded, e.g. some test
// harnesses); _build() measures the real value from a live `.browser-cell` and
// overrides `this._rowH` so piet.css stays the single source of truth.
import { fmtG } from './util.js';

const ROW_H_FALLBACK = 24;   // px
const PAGE_SIZE = 300;  // rows per fetch window
// SPEC II-3 (wired 2026-09-02): the cache depth and prefetch depth follow the
// user's memory budget instead of being constants. gridCapsForBudget() is the
// pure map (pinned by the DOM runner); setGridMemoryBudgetMb() is what app.js
// pushes the live setting through, beside console.js's setMemoryBudgetMb().
// Defaults below are the pre-wiring constants, so a page that never pushes a
// budget behaves exactly as before.
let PREFETCH  = 1;    // windows to prefetch ahead
let MAX_CACHE = 3;    // windows to keep in memory (~360 KB for a few cols)

/** Pure: {maxCache, prefetch} for a memory budget in MB. One window is ~360 KB
 *  for a few columns, so 256 MB of budget per cached window is a very safe
 *  ratio; floor 3 (the historical constant), ceiling 32; prefetch deepens to 2
 *  windows once the cache can hold 8. */
export function gridCapsForBudget(mb) {
  const m = Number(mb);
  if (!Number.isFinite(m) || m <= 0) return { maxCache: 3, prefetch: 1 };
  const maxCache = Math.max(3, Math.min(32, Math.round(m / 256)));
  return { maxCache, prefetch: maxCache >= 8 ? 2 : 1 };
}

/** Push the live memory budget in; returns the caps now in force. */
export function setGridMemoryBudgetMb(mb) {
  const caps = gridCapsForBudget(mb);
  MAX_CACHE = caps.maxCache;
  PREFETCH = caps.prefetch;
  return caps;
}
const VISIBLE_BUFFER = 10; // extra rows to render above/below viewport

// P1-A2 (UX-052 back half): Stata date-family calendar decode, front-end side.
//
// The engine stores every `%t*` column as the RAW Stata family index (P1-A1
// decision Q1=(b): value-identical to Stata's own storage, no ×7 rescale), and
// A0 carries the format string through to `col.format`. So the whole render is
// a pure index→text decode here — no engine call, no column mutation.
//
// Truth source = StataNow 19.5 BE, captured verbatim into
// test/fixtures/stata_date_render.txt (RENDER|<fmt>|<raw>|<string> per case).
// Note the oracle's leading spaces (" 2020w1" vs "1959w52") are `display`'s
// right-alignment padding to the format's fixed width, NOT part of the value's
// text — grid cells are right-aligned by piet.css, so the padding is dropped
// and the fixture is compared trimmed.
const T_MONTHS = ['jan', 'feb', 'mar', 'apr', 'may', 'jun',
                  'jul', 'aug', 'sep', 'oct', 'nov', 'dec'];
const T_MONTHS_FULL = ['January', 'February', 'March', 'April', 'May', 'June',
                        'July', 'August', 'September', 'October', 'November', 'December'];
const MS_PER_DAY = 86400000;
const TD_EPOCH_MS = Date.UTC(1960, 0, 1);   // %td index 0 = 01jan1960

// Floor division/modulo. JS `%` keeps the DIVIDEND's sign, so a pre-1960 index
// (-1) would decode to "1960w0" instead of Stata's "1959w52". Every family
// below runs before its epoch too, so both halves must floor, not truncate.
function _fdiv(a, b) { return Math.floor(a / b); }
function _fmod(a, b) { return a - _fdiv(a, b) * b; }

/**
 * Decode one Stata date-family index to its default display string.
 * `family` is the letter after `%t`. Returns null when this renderer does not
 * own the family — the caller then falls through to the numeric path.
 */
function fmtStataDate(val, family) {
  if (!Number.isFinite(val)) return null;
  const i = Math.floor(val);   // indices are integral; floor keeps pre-epoch sign
  switch (family) {
    case 'd': {
      const d = new Date(TD_EPOCH_MS + i * MS_PER_DAY);
      if (Number.isNaN(d.getTime())) return null;   // beyond JS Date's range
      return String(d.getUTCDate()).padStart(2, '0') +
             T_MONTHS[d.getUTCMonth()] + d.getUTCFullYear();
    }
    // Fixed-52-week years (DESIGN-dates-fmt.md §3.2): every year holds exactly
    // 52 Stata-weeks — week 52 absorbs the extra 8-9 days — so the year/week
    // split is plain floor arithmetic on 52, never a 7-day grid.
    case 'w': return `${1960 + _fdiv(i, 52)}w${_fmod(i, 52) + 1}`;
    case 'm': return `${1960 + _fdiv(i, 12)}m${_fmod(i, 12) + 1}`;
    case 'q': return `${1960 + _fdiv(i,  4)}q${_fmod(i,  4) + 1}`;
    case 'h': return `${1960 + _fdiv(i,  2)}h${_fmod(i,  2) + 1}`;
    // %ty is the exception: the stored value IS the calendar year, not an
    // offset (DESIGN-dates-fmt.md §3.1). Rescaling it is the regression this
    // case exists to prevent.
    case 'y': return String(i);
    // %tc (P1-A2B) is millisecond-scale, so it is the one family whose index is
    // NOT a day count: split it into days + milliseconds-within-day by the same
    // floor arithmetic the offset families use (a truncating split would put
    // every pre-1960 instant on the wrong calendar day), then reuse the %td
    // layout for the date half. Stata's default %tc display TRUNCATES the
    // sub-second remainder — 00:00:00.500 shows as `00:00:00`, not `00:00:01`
    // — which is why the oracle carries the `…123`/`…999` cases.
    case 'c': {
      const days = _fdiv(i, MS_PER_DAY);
      const rem  = _fmod(i, MS_PER_DAY);
      const d = new Date(TD_EPOCH_MS + days * MS_PER_DAY);
      if (Number.isNaN(d.getTime())) return null;   // beyond JS Date's range
      const hh = String(_fdiv(rem, 3600000)).padStart(2, '0');
      const mm = String(_fdiv(_fmod(rem, 3600000), 60000)).padStart(2, '0');
      const ss = String(_fdiv(_fmod(rem, 60000), 1000)).padStart(2, '0');
      return String(d.getUTCDate()).padStart(2, '0') +
             T_MONTHS[d.getUTCMonth()] + d.getUTCFullYear() +
             ` ${hh}:${mm}:${ss}`;
    }
    // %tC stays out: it is %tc PLUS the leap-second table, and without that
    // table a decode would be silently wrong by an integer number of seconds
    // rather than visibly unhandled. It keeps the fmtG fallback on purpose.
    default:  return null;
  }
}

// UX-052 named residual ①: the `c` suffix on a numeric format (`%12.0gc`,
// `%12.2fc`) is Stata's thousands-separator flag. Only the integer digits ahead
// of the decimal point are grouped, and only in plain notation — fmtG can
// return `1.234568e+12`, whose mantissa digits are not a magnitude and must
// never be chunked. Applied here in grid.js rather than inside fmtG because
// fmtG is the shared %9.0g formatter for every other panel (coeftables,
// summarize), none of which asked for commas.
function _groupThousands(s) {
  if (typeof s !== 'string' || s.includes('e')) return s;
  const m = /^([+-]?)(\d*)(.*)$/.exec(s);
  return m[1] + m[2].replace(/\B(?=(\d{3})+$)/g, ',') + m[3];
}

// FIX:GRID-TFMT-DETAIL — strftime-like token map for a `%td` detail string
// (e.g. `%tdNN/DD/CCYY`, `%tdMonth_dd,_CCYY`). Scope is `%td` ONLY: `%tc`/`%tC`
// stay out of fmtStataDate's family decode entirely (millisecond-scale / leap
// seconds, deliberately unhandled — see the non-regression suite), so a detail
// string on those families is not reached here either. Recognized tokens
// (longest-match-first so `Month` doesn't get eaten by `Mon`, and `CCYY`
// doesn't get eaten by its own trailing `YY`): `CCYY` 4-digit year · `Month`
// full name · `Mon` 3-letter abbrev (both capitalized, matching Stata) · `DD`
// zero-padded day · `dd` day with no leading zero · `NN` zero-padded month ·
// `YY` 2-digit year · `_` literal space (format strings can't hold a real
// space). Anything else (`/`, `-`, `,`, …) passes through unchanged. Returns
// null on an out-of-JS-Date-range index, same contract as fmtStataDate.
const TD_DETAIL_TOKENS = [
  ['CCYY',  (y, mo, d) => String(y).padStart(4, '0')],
  ['Month', (y, mo, d) => T_MONTHS_FULL[mo]],
  ['Mon',   (y, mo, d) => T_MONTHS_FULL[mo].slice(0, 3)],
  ['DD',    (y, mo, d) => String(d).padStart(2, '0')],
  ['dd',    (y, mo, d) => String(d)],
  ['NN',    (y, mo, d) => String(mo + 1).padStart(2, '0')],
  ['YY',    (y, mo, d) => String(_fmod(y, 100)).padStart(2, '0')],
  ['_',     () => ' '],
];

function fmtTdDetail(val, detail) {
  if (!Number.isFinite(val)) return null;
  const i = Math.floor(val);
  const date = new Date(TD_EPOCH_MS + i * MS_PER_DAY);
  if (Number.isNaN(date.getTime())) return null;   // beyond JS Date's range
  const y = date.getUTCFullYear();
  const mo = date.getUTCMonth();
  const d = date.getUTCDate();
  let out = '';
  let idx = 0;
  scan: while (idx < detail.length) {
    for (const [tok, render] of TD_DETAIL_TOKENS) {
      if (detail.startsWith(tok, idx)) { out += render(y, mo, d); idx += tok.length; continue scan; }
    }
    out += detail[idx];
    idx += 1;
  }
  return out;
}

/**
 * A15: render a numeric browse cell through the variable's Stata display
 * format instead of the raw JS double. Handles two cases precisely —
 * `%t*` date families (P1-A2, above; `%td` additionally honors a detail
 * string via fmtTdDetail, FIX:GRID-TFMT-DETAIL) and `%w.df` (fixed decimals)
 * — and falls back to fmtG's general %g-style rendering for everything else:
 * `%w.dg` (general), unrecognized formats, or no format at all. A trailing `c`
 * on the numeric regex is Stata's thousands-comma suffix (`%12.0gc`,
 * `%12.2fc`); P1-A2B honors it on BOTH branches via _groupThousands, closing
 * UX-052 named residual ①. Never throws, never emits a raw double.
 *
 * Detail strings are honored for `%td` only. A `%tc` carrying one
 * (`%tcDD_Mon_CCYY_HH:MM:SS`) still renders the family-DEFAULT clock layout —
 * the right instant in the wrong layout, the same residual class UX-052 ③
 * named for `%td` before FIX:GRID-TFMT-DETAIL closed it there.
 */
function fmtCellValue(val, fmt) {
  if (typeof fmt === 'string') {
    const t = /^%-?t([cCdwmqhy])(.*)$/.exec(fmt);
    if (t) {
      const s = t[1] === 'd' && t[2] ? fmtTdDetail(val, t[2]) : fmtStataDate(val, t[1]);
      if (s !== null) return s;
      // %tC, or a value JS Date cannot hold — fall through, never throw.
    }
    const m = /^%-?\d*\.(\d+)(f|g)(c?)$/.exec(fmt);
    if (m && m[2] === 'f') {
      const s = val.toFixed(parseInt(m[1], 10));
      return m[3] ? _groupThousands(s) : s;
    }
    if (m && m[3]) {
      // %w.dgc — the general formatter still decides the digits; the suffix
      // only adds separators to whatever magnitude it produced.
      const s = fmtG(val);
      if (s !== null) return _groupThousands(s);
    }
    // %w.dg (general) and anything else recognized-but-not-fixed falls
    // through to the general formatter below, same as unrecognized formats.
  }
  return fmtG(val) ?? String(val);
}

export class VirtualizedGrid {
  /**
   * @param {HTMLElement} container  — the scrollable div (full overlay)
   * @param {Function} fetchPage     — async (offset, limit, vars?) → BrowseResult
   * @param {Function} [commitCell]  — async (varname, row, value) → result; edit mode only
   */
  constructor(container, fetchPage, commitCell) {
    this._container = container;
    this._fetchPage = fetchPage;
    this._commitCell = commitCell || null;

    this._nRows    = 0;
    this._columns  = [];
    this._cache    = new Map();   // windowStart → { columns, rows }
    this._pending  = new Set();   // in-flight window starts

    this._editable = false;       // toggled by setEditable(); `edit` vs `browse`
    this._editingCell = null;     // {row, col} currently open in an input, or null
    this._windowed = false;       // `browse … in f/l` or `… if` → window fully seeded, never fetch

    this._headerEl = null;
    this._bodyEl   = null;
    this._spacerT  = null;
    this._spacerB  = null;
    this._rowEls   = [];          // pool of reused row <div>s, position-indexed
                                   // (index k in the pool = absolute row topRow+k)
    this._rowElsCols = -1;        // column count the pool was built for; the
                                   // pool is torn down and rebuilt if this changes
    this._rowH     = ROW_H_FALLBACK; // measured from a real .browser-cell in _build()
    this._modeEl   = null;        // mode badge in the bar

    this._scrollRaf = null;

    this._build();
  }

  /** Enable/disable cell editing (the `edit` command sets true; `browse` false). */
  setEditable(on) {
    this._editable = !!on && !!this._commitCell;
    this._container.classList.toggle('grid-editable', this._editable);
    if (this._modeEl) {
      this._modeEl.textContent = this._editable ? 'EDIT' : 'BROWSE';
      this._modeEl.classList.toggle('mode-edit', this._editable);
    }
  }

  // ── Public ─────────────────────────────────────────────────────────────────

  /** Load initial metadata and render the first window. */
  async load(browseResult) {
    this._nRows   = browseResult.n_rows_total;
    this._columns = browseResult.columns;
    // A row-windowed result (`browse … in f/l` or `… if`) carries the ENTIRE
    // window in this block and n_rows_total is the exact window size. GET
    // /api/browse has no in/if support, so a re-fetch would return the wrong
    // rows from the full dataset — set on every load() so a later plain browse
    // clears it and resumes normal lazy paging.
    this._windowed = !!browseResult.windowed;
    this._cache.clear();
    this._pending.clear();
    // The `browse`/`edit` command embeds only a preview (the first 50 rows) in
    // this block, whereas window 0 spans up to PAGE_SIZE rows. Seeding the cache
    // with a partial preview would make _ensureWindow(0) treat window 0 as fully
    // cached and never fetch the rest — so rows past the preview render as
    // missing "." forever. Only seed the cache when the preview actually fills
    // the first window; otherwise let _syncScroll → _ensureWindow(0) fetch the
    // full window from GET /api/browse.
    const firstWindowRows = Math.min(PAGE_SIZE, this._nRows);
    if ((browseResult.rows?.length ?? 0) >= firstWindowRows) {
      this._cacheWindow(0, browseResult);
    }
    this._renderHeader();
    this._syncScroll();
  }

  /** Show the overlay (make container visible). */
  show() {
    this._container.hidden = false;
    this._bodyEl.scrollTop = 0;
    this._syncScroll();
  }

  /** Hide the overlay. */
  hide() {
    this._container.hidden = true;
  }

  // ── DOM setup ──────────────────────────────────────────────────────────────

  _build() {
    this._container.innerHTML = '';
    this._container.className = 'browser-overlay';

    // Close button
    const closeBar = document.createElement('div');
    closeBar.className = 'browser-bar';

    const backBtn = document.createElement('button');
    backBtn.className = 'browser-back';
    backBtn.textContent = '← Back to Results';
    backBtn.addEventListener('click', () => this.hide());
    closeBar.appendChild(backBtn);

    // A2-06: title span, matching the doedit/graphics/about overlay bar layout
    // (reuses .doedit-title — a generic class, not doedit-scoped — so no CSS change needed).
    const title = document.createElement('span');
    title.className = 'doedit-title';
    title.textContent = 'Data Browser';
    closeBar.appendChild(title);

    // Mode badge (BROWSE / EDIT)
    this._modeEl = document.createElement('span');
    this._modeEl.className = 'browser-mode';
    this._modeEl.textContent = this._editable ? 'EDIT' : 'BROWSE';
    closeBar.appendChild(this._modeEl);

    // Header
    this._headerEl = document.createElement('div');
    this._headerEl.className = 'browser-header';

    // Body (scrollable)
    this._bodyEl = document.createElement('div');
    this._bodyEl.className = 'browser-body';
    this._bodyEl.setAttribute('role', 'grid');

    this._spacerT = document.createElement('div');
    this._spacerT.className = 'browser-spacer';

    this._spacerB = document.createElement('div');
    this._spacerB.className = 'browser-spacer';

    this._bodyEl.appendChild(this._spacerT);
    this._bodyEl.appendChild(this._spacerB);

    this._container.appendChild(closeBar);
    this._container.appendChild(this._headerEl);
    this._container.appendChild(this._bodyEl);

    // A3-07: measure the real row height from a live `.browser-cell` instead of
    // trusting the hardcoded ROW_H_FALLBACK to stay in sync with piet.css. An
    // explicit px `height` resolves via getComputedStyle without needing layout,
    // so this works whether or not the probe is actually laid out/visible; it
    // only falls back to ROW_H_FALLBACK when no matching stylesheet rule is
    // loaded at all (e.g. headless tests that don't link piet.css).
    const probe = document.createElement('div');
    probe.className = 'browser-cell';
    this._bodyEl.appendChild(probe);
    const measured = parseFloat(getComputedStyle(probe).height);
    this._bodyEl.removeChild(probe);
    this._rowH = Number.isFinite(measured) && measured > 0 ? measured : ROW_H_FALLBACK;

    // Throttled scroll listener: sync vertical virtualization AND horizontal header.
    this._bodyEl.addEventListener('scroll', () => {
      // Mirror horizontal scroll to the header immediately (no RAF needed — it's
      // a simple property set; deferring it causes a visible 1-frame desync).
      this._headerEl.scrollLeft = this._bodyEl.scrollLeft;

      if (this._scrollRaf) return;
      this._scrollRaf = requestAnimationFrame(() => {
        this._scrollRaf = null;
        this._syncScroll();
      });
    });

    // Delegated cell click → begin editing (edit mode only).
    this._bodyEl.addEventListener('click', e => {
      if (!this._editable) return;
      const cell = e.target.closest('.browser-cell');
      if (!cell || cell.classList.contains('browser-gutter')) return;
      if (cell.querySelector('input')) return;  // already editing
      const row = Number(cell.dataset.row);
      const col = Number(cell.dataset.col);
      if (Number.isNaN(row) || Number.isNaN(col)) return;
      this._beginEdit(cell, row, col);
    });

    // Keyboard: Escape to close (when not editing a cell)
    this._container.addEventListener('keydown', e => {
      if (e.key === 'Escape' && !this._editingCell) { this.hide(); e.stopPropagation(); }
    });
  }

  // ── Cell editing (edit mode) ─────────────────────────────────────────────────

  _beginEdit(cell, row, col) {
    const column = this._columns[col];
    if (!column) return;
    this._editingCell = { row, col };

    const winStart = Math.floor(row / PAGE_SIZE) * PAGE_SIZE;
    const win = this._cache.get(winStart);
    const raw = win?.rows?.[row - winStart]?.[col];

    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'cell-input';
    // factor cells show the level string; f64 the number; missing → empty
    input.value = (raw === null || raw === undefined) ? '' : String(raw);

    cell.textContent = '';
    cell.appendChild(input);
    input.focus();
    input.select();

    let done = false;
    const finish = (commit) => {
      if (done) return;
      done = true;
      this._editingCell = null;
      const newText = input.value;
      if (commit) {
        this._commit(row, col, column, newText);
      } else {
        // revert: re-render the row region to restore the original cell
        this._syncScroll();
      }
    };

    input.addEventListener('keydown', ev => {
      ev.stopPropagation();
      if (ev.key === 'Enter')  { ev.preventDefault(); finish(true); }
      if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
    });
    input.addEventListener('blur', () => finish(true));
  }

  async _commit(row, col, column, text) {
    // Parse the typed value to the wire form: f64 → number|null, factor → string|null.
    let value;
    const trimmed = text.trim();
    if (trimmed === '' || trimmed === '.') {
      value = null;                       // missing
    } else if (column.dtype === 'factor') {
      value = trimmed;                    // level label (backend resolves/creates the code)
    } else {
      const num = Number(trimmed);
      if (Number.isNaN(num)) {
        // invalid numeric — revert without calling the kernel
        this._syncScroll();
        return;
      }
      value = num;
    }

    try {
      const res = await this._commitCell(column.name, row, value);
      if (res && res.type === 'error') {
        this._syncScroll();
        return;
      }
      // Optimistically update the cached cell, then re-render.
      const winStart = Math.floor(row / PAGE_SIZE) * PAGE_SIZE;
      const win = this._cache.get(winStart);
      if (win && win.rows[row - winStart]) {
        win.rows[row - winStart][col] = value;
      }
      // A new factor level may have been added — refresh the column's levels.
      if (res && res.new_level && column.dtype === 'factor') {
        if (!Array.isArray(column.levels)) column.levels = [];
        if (!column.levels.includes(res.new_level)) column.levels.push(res.new_level);
        if (win && win.columns?.[col]) {
          win.columns[col].levels = column.levels;
        }
      }
    } catch {
      // network/kernel error — fall through to a clean re-render
    }
    this._syncScroll();
  }

  // ── Header rendering ───────────────────────────────────────────────────────

  _renderHeader() {
    this._headerEl.innerHTML = '';
    const row = document.createElement('div');
    row.className = 'browser-header-row';

    // Row number gutter cell
    const gutter = document.createElement('div');
    gutter.className = 'browser-cell browser-gutter browser-header-cell';
    gutter.textContent = '#';
    row.appendChild(gutter);

    for (const col of this._columns) {
      const cell = document.createElement('div');
      cell.className = 'browser-cell browser-header-cell';
      cell.textContent = col.name;
      if (col.dtype === 'factor') cell.classList.add('col-factor');
      row.appendChild(cell);
    }

    this._headerEl.appendChild(row);
  }

  // ── Scroll sync ────────────────────────────────────────────────────────────

  _syncScroll() {
    if (!this._nRows || !this._columns.length) return;

    const scrollTop = this._bodyEl.scrollTop;
    const clientH   = this._bodyEl.clientHeight;
    const visibleRows = Math.ceil(clientH / this._rowH);
    const totalH      = this._nRows * this._rowH;

    const topRow    = Math.floor(scrollTop / this._rowH);
    const renderTop = Math.max(0, topRow - VISIBLE_BUFFER);
    const renderBot = Math.min(this._nRows - 1, topRow + visibleRows + VISIBLE_BUFFER);

    // Update spacers
    this._spacerT.style.height = `${renderTop * this._rowH}px`;
    this._spacerB.style.height = `${(this._nRows - renderBot - 1) * this._rowH}px`;

    // Fetch required window(s)
    const winStart = Math.floor(renderTop / PAGE_SIZE) * PAGE_SIZE;
    this._ensureWindow(winStart);
    if (winStart + PAGE_SIZE <= this._nRows) {
      // prefetch depth = PREFETCH windows ahead (each one, not only the farthest)
      for (let k = 1; k <= PREFETCH; k++) {
        const ws = winStart + PAGE_SIZE * k;
        if (ws < this._nRows) this._ensureWindow(ws);
      }
    }

    // Render rows in [renderTop, renderBot]
    this._renderRows(renderTop, renderBot);
  }

  _ensureWindow(winStart) {
    if (winStart < 0 || winStart >= this._nRows) return;
    if (this._cache.has(winStart) || this._pending.has(winStart)) return;
    // A windowed result (`browse … in f/l` or `… if`) has its complete window
    // already seeded in window 0; GET /api/browse can't honor in/if and would
    // return wrong rows from the full dataset. Never fetch — any uncached row
    // renders blank/"." (explicit-missing beats silent-wrong, Doctrine ①).
    if (this._windowed) return;

    this._pending.add(winStart);
    // Request exactly the browsed columns, in the browsed order — otherwise the
    // backend returns every column in dataset order and the rows misalign with
    // this._columns (e.g. `beertax`/`fatal` would read the wrong cells).
    const vars = this._columns.map(c => c.name);
    this._fetchPage(winStart, PAGE_SIZE, vars).then(result => {
      this._pending.delete(winStart);
      this._cacheWindow(winStart, result);
      this._evictOldWindows();
      this._syncScroll();
    }).catch(() => {
      this._pending.delete(winStart);
    });
  }

  _cacheWindow(winStart, result) {
    this._cache.set(winStart, {
      columns: result.columns ?? this._columns,
      rows:    result.rows ?? [],
    });
  }

  _evictOldWindows() {
    if (this._cache.size <= MAX_CACHE) return;
    const oldestKey = this._cache.keys().next().value;
    this._cache.delete(oldestKey);
  }

  // ── Row rendering ──────────────────────────────────────────────────────────
  // Row <div>s are POOLED and REUSED across scroll frames (this._rowEls, position-
  // indexed: pool slot k holds absolute row topRow+k) rather than torn down and
  // recreated on every call — matching the header comment's "~100 reused rows"
  // promise. The pool is only rebuilt from scratch when the column count changes
  // (a fresh load() with a different browse), since the per-row cell count is
  // baked into each pooled node.

  /** Create one row <div> shaped for `nCols` data columns (content filled in by _updateRowEl). */
  _buildRowEl(nCols) {
    const rowEl = document.createElement('div');
    rowEl.className = 'browser-row';
    rowEl.style.height = `${this._rowH}px`;

    const gutter = document.createElement('div');
    gutter.className = 'browser-cell browser-gutter';
    rowEl.appendChild(gutter);
    rowEl._gutterEl = gutter;

    const cellEls = [];
    for (let c = 0; c < nCols; c++) {
      const cell = document.createElement('div');
      cell.className = 'browser-cell';
      rowEl.appendChild(cell);
      cellEls.push(cell);
    }
    rowEl._cellEls = cellEls;
    return rowEl;
  }

  /** Refresh a pooled row <div> in place to show absolute row index `i`. */
  _updateRowEl(rowEl, i) {
    rowEl._gutterEl.textContent = String(i + 1);

    const winStart = Math.floor(i / PAGE_SIZE) * PAGE_SIZE;
    const win = this._cache.get(winStart);
    const rowData = win ? win.rows[i - winStart] : undefined;

    for (let c = 0; c < this._columns.length; c++) {
      const cell = rowEl._cellEls[c];
      cell.className = 'browser-cell';

      if (!win) {
        // Loading placeholder — leave dataset.row/col UNSET so a stray click
        // during this window can't start an edit against not-yet-fetched data
        // (the delegated click handler bails when row/col aren't numbers).
        delete cell.dataset.row;
        delete cell.dataset.col;
        cell.textContent = '…';
        cell.classList.add('cell-loading');
        continue;
      }

      cell.dataset.row = String(i);
      cell.dataset.col = String(c);
      const col = this._columns[c];
      if (col.dtype === 'factor') cell.classList.add('col-factor');

      const val = rowData?.[c];
      if (val === null || val === undefined || (typeof val === 'number' && Number.isNaN(val))) {
        cell.textContent = '.';
        cell.classList.add('cell-missing');
      } else if (col.dtype === 'factor') {
        const levels = col.levels ?? win.columns[c]?.levels ?? [];
        cell.textContent = levels[val] ?? String(val);
      } else {
        // A15: render through the variable's Stata display format instead of
        // the raw JS double (e.g. "1.3700000000...") — never crash, never
        // show a 17-digit double; unrecognized/absent formats fall back to
        // fmtG (same general formatter used everywhere else in the UI).
        cell.textContent = typeof val === 'number' ? fmtCellValue(val, col.format) : val;
        cell.classList.add('cell-numeric');
      }
    }
  }

  _renderRows(topRow, botRow) {
    const nCols = this._columns.length;

    // Column set changed since the pool was built — the reused shape (nCols
    // cells/row) is stale, so rebuild the pool fresh rather than patch cells
    // that no longer line up with the new columns.
    if (this._rowElsCols !== nCols) {
      for (const rowEl of this._rowEls) rowEl.remove();
      this._rowEls = [];
      this._rowElsCols = nCols;
    }

    const wantCount = botRow - topRow + 1;

    // Grow the pool if the current window needs more row nodes than exist.
    while (this._rowEls.length < wantCount) {
      const rowEl = this._buildRowEl(nCols);
      this._bodyEl.insertBefore(rowEl, this._spacerB);
      this._rowEls.push(rowEl);
    }
    // Shrink it if fewer are needed (near the start/end of the dataset, or a
    // resize that reduced the visible row count).
    while (this._rowEls.length > wantCount) {
      this._rowEls.pop().remove();
    }

    // Refresh each reused node in place for its (possibly new) absolute row.
    for (let k = 0; k < wantCount; k++) {
      this._updateRowEl(this._rowEls[k], topRow + k);
    }
  }
}
