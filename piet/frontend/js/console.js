// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// console.js — append-only results console
// The critical path is formatEstimation() — verified against piet/fixtures/golden/*.json.

import { fmtG, fmtT, fmtP, fmtObs, padL, padR, escHtml } from './util.js';
import { renderGraph, renderEventStudyBlock } from './graph.js';
import { getCachedPietVersion } from './api.js';
import { cacheSummarizeStats } from './varstats.js';
import { createSpill, idbStore } from './graph_spill.js';

// ─── DOM refs (set by init) ───────────────────────────────────────────────────
let _bodyEl  = null;   // #console-body — where blocks are appended
let _wrapEl  = null;   // #console-wrap — the scrollable container (parent of _bodyEl)
let _emptyEl = null;   // the empty-state SVG overlay (z-index 1)

// Scroll state
let _userScrolledUp = false;
let _jumpPill = null;

// ─── Console scrollback cap (P7 — resource safety, COUNCIL-20260710 D5/F6) ────
// A runaway `.do` loop (e.g. `forvalues i=1/100000 { display ... }`) must not
// be able to grow the console DOM without bound: every appended block is a
// <div> that never goes away on its own, so an unbounded session is the
// "classroom-afternoon OOM" (RAW-resources.md F6) — the webview grows
// monotonically all day. Cap live console blocks by BOTH total byte size and
// block count; whichever trips first, drop the OLDEST live blocks (the
// newest output stays visible — same precedent as Stata's own Results-buffer
// truncation) and collapse everything dropped into ONE elision marker.
//
// The cap is derived from the user's configured memoryBudgetMb (Settings
// panel -> native piet::settings::Settings::memoryBudgetMb, SPEC II-2) — a
// small, generous share of it, so a normal classroom day of output never
// trips this and only a genuine runaway does. console.js has no standing
// route to the live settings value by itself (that lives behind api.js's
// settings.get bridge call, fetched by app.js's Settings panel — a different
// lane this sprint); initConsole's optional 4th argument and the exported
// setMemoryBudgetMb() are the hook a caller uses to push the real value in
// once fetched. Until one does, DEFAULT_MEMORY_BUDGET_MB mirrors index.html's
// shipped Settings slider default (#set-mem-budget value="2048"), so the
// fallback cap is exactly what a fresh install's configured budget would
// derive anyway.
export const DEFAULT_MEMORY_BUDGET_MB = 2048;

const BYTE_BUDGET_FRACTION = 0.01;               // 1% of the memory budget
const MIN_BYTE_BUDGET      = 1024;               // floor: never literally zero
const MAX_BYTE_BUDGET      = 128 * 1024 * 1024;  // ceiling: 128 MB even at an 8192 MB budget

const BLOCKS_PER_BUDGET_MB = 24;                 // ~24 retained blocks per configured MB
const MIN_BLOCK_COUNT      = 1;                  // always keep at least the newest block
const MAX_BLOCK_COUNT      = 50000;              // ceiling regardless of configured budget
                                                  // (DOM node count is a layout cost, not
                                                  // purely a memory-size one)

let _memoryBudgetMb = DEFAULT_MEMORY_BUDGET_MB;

/** Push the live memoryBudgetMb setting in (Settings panel apply, or a
 *  boot-time settings.get fetch) so the console cap tracks the user's
 *  configured budget. Ignores non-positive/non-finite input (keeps the last
 *  good value, or the shipped default if none was ever set). */
export function setMemoryBudgetMb(mb) {
  if (Number.isFinite(mb) && mb > 0) _memoryBudgetMb = mb;
}

function consoleCaps() {
  const byteBudget = Math.min(MAX_BYTE_BUDGET, Math.max(MIN_BYTE_BUDGET,
    Math.round(_memoryBudgetMb * 1024 * 1024 * BYTE_BUDGET_FRACTION)));
  const blockCount = Math.min(MAX_BLOCK_COUNT, Math.max(MIN_BLOCK_COUNT,
    Math.round(_memoryBudgetMb * BLOCKS_PER_BUDGET_MB)));
  return { byteBudget, blockCount };
}

// Rough byte-size estimate for a block: the source JSON's serialized length.
// Cheap, uniform across every block type (including graph blocks' numeric
// arrays), and correlates with both DOM footprint and bridge-payload size —
// exactly the two things this cap protects against.
function estimateBlockBytes(block) {
  try {
    return JSON.stringify(block).length;
  } catch {
    return 0;
  }
}

// Live blocks currently held in the DOM, oldest-first (mirrors _bodyEl's
// children minus the elision marker, which is tracked separately below).
let _liveBlocks  = [];     // [{ el, bytes }]
let _liveBytes   = 0;
let _elisionEl   = null;   // the single "…older output trimmed" marker node
let _elidedCount = 0;      // cumulative blocks dropped this session

// C4 (P8-S9): live-coding anchor — the single most-recent ESTIMATION block,
// tracked so the NEXT one can strip .block-active off it before handing the
// class to itself. Only ONE block ever carries .block-active at a time (the
// "say it, point at it" target during a lecture — mockup §2 `.blk.active`,
// state grammar §4 "active (tab/latest block/Run)"). Not reset by the P7 cap
// eviction above — a block only loses .block-active when a NEWER estimation
// block supersedes it, never by aging out of the byte/count budget (an
// evicted-while-still-active node is simply detached; classList ops on a
// detached node are harmless no-ops, and the next estimation block corrects
// the reference regardless).
let _activeBlockEl = null;

// BUG-1473: `display ... _continue` leaves its line open (a display block
// flagged `continue`), and the next display's text goes on that line, past any
// command echo between them -- the join doexec.cpp's pushBlock makes in the
// stored transcript. A streaming do-run has already shown the first block when
// the next one comes (streamed, not stored), so it is joined here. Any other
// block closes the line, and so does the start of a run (endDisplayLine, from
// app.js's setBusy): the transcript joins within one run only.
let _openDisplayEl = null;
/** Close an open `_continue` display line (a new run starts on its own line). */
export function endDisplayLine() { _openDisplayEl = null; }

// T3 (P8.1): output fold/unfold ("안2-lite"). An `echo` block (the
// `. <command>` line) is a fold head; its group is every flat sibling that
// follows it up to (not including) the next fold head — computed
// POSITIONALLY at click-time by _toggleFold() below, not stored on the
// blocks (see the deviation note at its call site in appendBlock). Zero DOM
// structure change either way (constraint (a)): every block stays a flat
// sibling of #console-body, so the P7 cap eviction, Save Results' direct-
// children walk, and the elision marker all keep working unmodified. Blocks
// appended before the FIRST echo of a session (e.g. a boot-time note) have
// no fold head to their left and are simply never foldable.

function updateElisionMarker() {
  if (!_elisionEl) {
    _elisionEl = document.createElement('div');
    _elisionEl.className = 'block block-elision block-echo';
    _bodyEl.insertBefore(_elisionEl, _bodyEl.firstChild);   // stays at the oldest position
  }
  _elisionEl.textContent = `…older output trimmed (${_elidedCount} blocks)`;
}

// Drop the OLDEST live blocks until both the byte and block-count budgets
// are satisfied again. Only ever removes whole blocks already in the DOM —
// never touches the content of a block that survives, so anything below the
// cap renders byte-identically to an uncapped session.
function enforceConsoleCap() {
  const { byteBudget, blockCount } = consoleCaps();
  let trimmed = 0;
  while (_liveBlocks.length > 0 &&
         (_liveBlocks.length > blockCount || _liveBytes > byteBudget)) {
    const oldest = _liveBlocks.shift();
    oldest.el.remove();
    _liveBytes -= oldest.bytes;
    trimmed++;
  }
  if (trimmed > 0) {
    _elidedCount += trimmed;
    updateElisionMarker();
  }
}

export function initConsole(consoleEl, emptyEl, jumpPill, memoryBudgetMb) {
  _bodyEl  = consoleEl;
  _wrapEl  = consoleEl.parentElement;  // #console-wrap is the overflow:auto element
  _emptyEl = emptyEl;
  _jumpPill = jumpPill;
  if (memoryBudgetMb != null) setMemoryBudgetMb(memoryBudgetMb);

  _wrapEl.addEventListener('scroll', () => {
    const atBottom =
      _wrapEl.scrollHeight - _wrapEl.scrollTop - _wrapEl.clientHeight < 4;
    _userScrolledUp = !atBottom;
    if (_jumpPill) _jumpPill.hidden = atBottom;
  });
}

// ─── Public API ───────────────────────────────────────────────────────────────

// BUG-150: `exit` is not an error — it is the executor's HALT SENTINEL, and
// its sentinel happens to be an untagged error block whose message is exactly
// "exit" or "exit <n>" (dispatcher.cpp's exit arm; the CommandErrorResult
// `code` enum is frozen, so the intended "program_exit" is coerced to
// engine_error at construction and cannot be used to tell the two apart —
// recognising the message SHAPE is the established mechanism, doexec.cpp's
// parseCleanExitMessage, BUG-139).
//
// Headless already reinterprets it twice: --do returns the requested rc, and
// handleUncapturedError() ERASES the sentinel from the stored block list. The
// GUI still painted it red because the live {kind:"block"} stream (N6) pushes
// each block as it completes — BEFORE that erase — so a clean run's last card
// was an alarming engine_error even though the batch response no longer
// carried it. Recognising it here, at the ONE append point both the streamed
// and the batch path go through, makes the two transports agree: the sentinel
// renders nothing at all, and applyDoResult() prints the single neutral
// "do-file ended" line instead (app.js).
const CLEAN_EXIT_RE = /^exit(\s+-?\d+)?$/;
export function isCleanExitSentinel(block) {
  return !!block && block.type === 'error' && !block.captured &&
         CLEAN_EXIT_RE.test(String(block.message ?? '').trim());
}

/** Append a ResultBlock (dispatch on .type). */
export function appendBlock(block) {
  if (!_bodyEl) return;
  if (isCleanExitSentinel(block)) return;   // BUG-150 — see above
  hideEmpty();

  // BUG-1473: a display continuing an open `_continue` line joins it.
  if (block.type === 'display' && _openDisplayEl && _openDisplayEl.isConnected) {
    _openDisplayEl.textContent += block.text ?? '';
    const live = _liveBlocks.find(b => b.el === _openDisplayEl);
    const bytes = estimateBlockBytes(block);
    if (live) { live.bytes += bytes; _liveBytes += bytes; }
    if (!block.continue) _openDisplayEl = null;
    enforceConsoleCap();
    if (!_userScrolledUp) _wrapEl.scrollTop = _wrapEl.scrollHeight;
    return;
  }
  if (block.type !== 'echo') _openDisplayEl = null;

  let el;
  switch (block.type) {
    case 'echo':             el = renderEcho(block);           break;
    case 'display':          el = renderDisplay(block);        break;
    // B9 (P8-S7): synthetic, client-only block — app.js's runCommand()
    // intercepts `note <text>` / `* <text>` before it ever reaches
    // client.command(), so this block type never crosses the wire.
    case 'note':              el = renderNote(block);           break;
    case 'estimation':       el = renderEstimation(block);     break;
    case 'summarize':
      el = renderSummarize(block);
      cacheSummarizeStats(block);   // B14(a): passive per-var stats cache for panels.js's detail card
      break;
    case 'tabulate':         el = renderTabulate(block);       break;
    case 'describe':         el = renderDescribe(block);       break;
    case 'data_mutation':    el = renderMutation(block);       break;
    case 'graph':
      registerGraph(block);            // gallery always collects (even suppressed)
      if (block.suppressed) return;     // set graphics off → skip live console append
      el = renderGraph(block);
      break;
    case 'error':            el = renderError(block);          break;
    case 'estimates_table':  el = renderEstimatesTable(block); break;
    default:
      el = renderEcho({ type: 'echo', text: JSON.stringify(block) });
  }

  // T3 (P8.1): fold-group tagging — an echo block becomes a clickable fold
  // head; every following flat sibling up to (not including) the next
  // .block-fold-head is that group's membership, determined POSITIONALLY at
  // fold-time (_toggleFold below) rather than stamped into the DOM here.
  //
  // Deviation from the original brief (which specified a module-level
  // counter written to el.dataset.group at append time): that shape leaks
  // append-call-count into a block's own outerHTML, which broke the P7 cap
  // suite's byte-identical guarantee ("P7: console cap (block-count-bound)…"
  // dom_runner.mjs:730) — its two comparison legs re-render the SAME block
  // fresh (no clearConsole reset, no replay of prior siblings) and diff
  // outerHTML against the capped survivor; a counter value baked into the
  // survivor (e.g. group="200" after 200 prior echoes) can never match the
  // fresh single-append render (group="1"). Positional grouping needs no
  // value stored on the block at all, so byte-identity holds unconditionally
  // — and it's semantically equivalent (DOM order already IS the grouping
  // signal; appendBlock never reorders blocks). See report "T3 deviation".
  if (block.type === 'echo') {
    el.classList.add('block-fold-head');
    el.tabIndex = 0;
    el.setAttribute('role', 'button');
    el.setAttribute('aria-expanded', 'true');
    el.addEventListener('click', () => _toggleFold(el));
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        _toggleFold(el);
      }
    });
  }

  // A10 (P8.1): generalize esttab's "Copy table" affordance (:1447-1464
  // below) to every coeftable-bearing block type — hover-revealed action bar
  // that copies the PRE's own textContent. Scoped by block.type rather than
  // a post-hoc querySelector so a block that merely happens to contain a
  // <pre class="coeftable"> for unrelated reasons never picks this up.
  if (block.type === 'estimation' || block.type === 'summarize' ||
      block.type === 'tabulate'   || block.type === 'describe') {
    _addCopyBar(el);
  }

  _bodyEl.appendChild(el);
  if (block.type === 'display' && block.continue) _openDisplayEl = el;   // BUG-1473

  // C4 (P8-S9): live-coding anchor — hand .block-active to this block if
  // it's an estimation result (the mockup's demoed case: `regress` sits
  // active while the `summarize` above it stays plain), stripping it from
  // whichever block held it before. Scoped to 'estimation' only — echo/
  // display/mutation/etc. aren't "results" in the pointing-target sense.
  if (block.type === 'estimation') {
    if (_activeBlockEl) _activeBlockEl.classList.remove('block-active');
    el.classList.add('block-active');
    _activeBlockEl = el;
  }

  // P7: track this block against the byte/block-count cap and drop the
  // oldest live blocks if either budget is now exceeded. Pure bookkeeping —
  // the block just appended (and every surviving one) is never mutated.
  const bytes = estimateBlockBytes(block);
  _liveBlocks.push({ el, bytes });
  _liveBytes += bytes;
  enforceConsoleCap();

  if (!_userScrolledUp) {
    _wrapEl.scrollTop = _wrapEl.scrollHeight;
  }
}

// T3 (P8.1): fold/unfold a group in place — click/Enter/Space on an echo
// (fold head) toggles every flat sibling FOLLOWING it, up to (not
// including) the next .block-fold-head, between visible and
// `.block-folded` (display:none, CSS-APPEND). Positional, not a stored-id
// lookup — see the deviation note where this head's class is assigned in
// appendBlock. The head itself never hides — it flips `.fold-closed`
// (chevron pseudo-element swap) and carries `data-fold-count` (the closed-
// count pseudo-element) instead.
// Guard: an in-progress text selection means this click was a copy-drag
// over the echo text (e.g. ⌘C-by-drag of the command line), not a fold
// request — bail without touching anything.
function _toggleFold(headEl) {
  if (window.getSelection().toString() !== '') return;

  const closing = !headEl.classList.contains('fold-closed');
  let hiddenCount = 0;
  for (let node = headEl.nextElementSibling;
       node && !node.classList.contains('block-fold-head');
       node = node.nextElementSibling) {
    node.classList.toggle('block-folded', closing);
    if (closing) hiddenCount++;
  }

  headEl.classList.toggle('fold-closed', closing);
  headEl.setAttribute('aria-expanded', String(!closing));
  if (closing) {
    headEl.dataset.foldCount = String(hiddenCount);
  } else {
    delete headEl.dataset.foldCount;
  }
}

// A10 (P8.1): shared "Copy table" action bar — same clipboard idiom as the
// esttab actions bar below (:1447-1464 pre-T3 line numbers), generalized to
// any block whose rendered <pre class="coeftable"> should be copyable.
// Appended INSIDE the block, AFTER the <pre> (design constraint (b): never
// touches textContent; buttons live in their own action-bar div so a later
// integration step can strip `.console-action` nodes from saved logs).
function _addCopyBar(el) {
  const pre = el.querySelector('pre.coeftable');
  if (!pre) return;

  const bar = document.createElement('div');
  bar.className = 'esttab-actions block-copy-bar console-action';

  const copyBtn = document.createElement('button');
  copyBtn.textContent = 'Copy table';
  copyBtn.className = 'esttab-btn';
  copyBtn.addEventListener('click', () => {
    const text = pre.textContent;
    navigator.clipboard.writeText(text).catch(() => {
      // Fallback for environments without Clipboard API
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    });
    copyBtn.textContent = 'Copied!';
    setTimeout(() => { copyBtn.textContent = 'Copy table'; }, 1500);
  });
  bar.appendChild(copyBtn);

  el.appendChild(bar);
}

/** Append multiple blocks in order, stopping at the first error for .do replay. */
export function appendBlocks(blocks) {
  let stopped = false;
  for (const b of blocks) {
    if (stopped) break;
    appendBlock(b);
    // BUG-150: a clean-exit sentinel is a halt, not a failure — it renders
    // nothing and must not set the "stop the replay" flag either, or a
    // trailing `exit` would suppress blocks the engine did emit after it.
    if (b.type === 'error' && !isCleanExitSentinel(b)) stopped = true;
  }
}

/** Clear console and re-show the empty-state overlay. */
export function clearConsole() {
  if (!_bodyEl) return;
  const blocks = _bodyEl.querySelectorAll('.block');
  blocks.forEach(b => b.remove());
  // U7 (P8 Honest Cockpit): `cls` clears the console TEXT only — the graphics
  // gallery survives (Stata parity: plots die on `graph drop _all`, not on cls).
  // P7: the elision marker (class "block") was just removed above along with
  // everything else — drop the JS-side cap-tracking state to match.
  _liveBlocks = [];
  _liveBytes = 0;
  _elisionEl = null;
  _elidedCount = 0;
  // C4: the active block (if any) was just removed above too — nothing left
  // to strip .block-active from until the next estimation block arrives.
  _activeBlockEl = null;
  _openDisplayEl = null;   // BUG-1473: its line went with the blocks
  // T3: no group-counter state to reset — fold membership is computed
  // positionally at click-time (_toggleFold), and every block it could
  // touch was just removed above.
  showEmpty();
}

// ─── Graphics gallery store (WS4) ──────────────────────────────────────────────
// Every `graph` block rendered to the console is also collected here so the
// Graphics overlay can present a session gallery. Thumbnails re-render the SAME
// block through renderGraph() (no new rendering code, Doctrine §3 lightness).
const _graphGallery = [];   // [{ id, title, kind, block|null, bytes, stored, storedBytes, state }], oldest-first
let _galleryBytes  = 0;     // bytes of RESIDENT blocks only
let _galleryElided = 0;     // cumulative thumbnails dropped (not preserved) this session
let _spillBytes    = 0;     // stored (compressed) bytes on disk
let _spillSeq      = 0;
let _galleryGen    = 0;     // bumped by clearGraphGallery so late writes self-delete
const _spillPending = new Set();
// Spilled-storage cap: generous, oldest-first drop with an honest marker.
let GRAPH_SPILL_MAX_BYTES = 256 * 1024 * 1024;
/** Override the spilled-storage cap (tests); non-positive restores the default. */
export function setGraphSpillMaxBytes(n) { GRAPH_SPILL_MAX_BYTES = n > 0 ? n : 256 * 1024 * 1024; }
let _spill;                 // undefined = not yet resolved; null = unavailable
function spill() {
  if (_spill === undefined) {
    let s = null;
    try { const st = idbStore(); s = st ? createSpill({ store: st }) : null; } catch (_) { s = null; }
    _spill = s;
  }
  return _spill;
}
/** Inject a spill (tests) or null to disable spilling. */
export function setGraphSpill(s) { _spill = s; }
// WS-F2 mission 4 / audit A6-01 sibling: an unbounded session (a long .do loop
// emitting graphs) would otherwise grow this array without limit. Cap it and
// drop the oldest — the gallery is a recent-session convenience, not an archive.
const GRAPH_GALLERY_MAX = 50;

// BUG-066 (C3-F5 "gallery stores byte-bounded thumbnails" — the last component):
// GRAPH_GALLERY_MAX alone bounds the gallery by COUNT, and a graph block's size
// is not bounded by the live-view decimation the native side applies: that
// budget (graph_m4.cpp kMaxViewPoints = 5000) is per points-ARRAY, so a block's
// bytes still scale with its layer/series count. Measured on the 108K corpus
// (soccer_game_panel, post-decimation): 1-series scatter 191 KB · 4-series
// scatter 553 KB · 5-layer twoway 764 KB — so 50 slots is ~38 MB and climbs
// with every extra layer a user writes. The gallery also OUTLIVES the console's
// own P7 eviction (it holds the block objects; _liveBlocks holds only DOM
// nodes + sizes), which makes it the dominant retainer in a long session.
//
// So bound it by bytes too, reusing the console's own budget primitives
// (consoleCaps().byteBudget, estimateBlockBytes) rather than inventing a second
// scheme — the gallery may retain at most as many bytes as the live console
// may. The newest thumbnail is ALWAYS kept, even if it alone exceeds the budget
// (mirrors MIN_BLOCK_COUNT=1 for the console cap): a single huge plot must
// still be viewable.
//
// This bounds the gallery's OWN copy only. `session.lastGraph` / `namedGraphs`
// live native-side and back `graph export/save/combine` (the golden-pinned SVG
// seam) — untouched here, by construction.
// e.counted = this entry's bytes are currently included in _galleryBytes.
function uncount(e) { if (e.counted) { e.counted = false; _galleryBytes -= e.bytes; } }

function dropEntry(e, countElided) {
  const i = _graphGallery.indexOf(e);
  if (i >= 0) _graphGallery.splice(i, 1);
  uncount(e);
  if (e.stored) {
    _spillBytes -= e.storedBytes;
    const sp = spill();
    if (sp) sp.del(e.id).catch(() => {});
  }
  e.state = 'dead';
  e.block = null;
  if (countElided) _galleryElided++;
}

function spillEntry(e) {
  const sp = spill();
  const gen = _galleryGen;
  e.state = 'spilling';
  const p = sp.put(e.id, e.block).then(r => {
    if (e.state === 'dead' || gen !== _galleryGen) {
      sp.del(e.id).catch(() => {});
      return;
    }
    e.stored = true; e.storedBytes = r.stored; _spillBytes += r.stored;
    e.state = 'stored';
    e.block = null;
    enforceSpillCap();
  }).catch(() => {
    if (e.state !== 'dead' && gen === _galleryGen) dropEntry(e, true);
  }).finally(() => { _spillPending.delete(p); });
  _spillPending.add(p);
}

function enforceSpillCap() {
  while (_spillBytes > GRAPH_SPILL_MAX_BYTES) {
    const e = _graphGallery.find(g => g.stored && !g.block);
    if (!e) break;
    dropEntry(e, true);
  }
}

function enforceGalleryCap() {
  const { byteBudget } = consoleCaps();
  const resident = () => _graphGallery.filter(g => g.block && g.state !== 'spilling');
  for (;;) {
    const res = resident();
    if (!(res.length > 1 && (res.length > GRAPH_GALLERY_MAX || _galleryBytes > byteBudget))) break;
    const e = res[0];
    if (e.stored) { e.block = null; uncount(e); e.state = 'stored'; }   // already on disk
    else if (spill()) { uncount(e); spillEntry(e); }
    else dropEntry(e, true);
  }
}

/** Collect a graph block for the Graphics gallery (called from appendBlock). */
export function registerGraph(block) {
  const bytes = estimateBlockBytes(block);
  _graphGallery.push({
    id: `g${++_spillSeq}`, title: block && block.title, kind: block && (block.kind || block.type),
    block, bytes, counted: true, stored: false, storedBytes: 0, state: 'resident',
  });
  _galleryBytes += bytes;
  enforceGalleryCap();
}

/** Resident graph blocks, in render order (sync; spilled entries are skipped). */
export function getGraphs() {
  return _graphGallery.filter(g => g.block).map(g => g.block);
}

/** Every gallery entry in order (sync, tiny): { id, title, kind, bytes, storedBytes, resident }. */
export function getGraphEntries() {
  return _graphGallery.map(g => ({
    id: g.id, title: g.title, kind: g.kind, bytes: g.bytes,
    storedBytes: g.storedBytes, resident: !!g.block,
  }));
}

async function rehydrate(e) {
  if (e.block) return e.block;
  const sp = spill();
  if (!sp || !e.stored) return null;
  let block = null;
  try { block = await sp.get(e.id); } catch (_) { block = null; }
  if (!block) { if (e.state !== 'dead') dropEntry(e, true); return null; }
  if (e.state === 'dead') return block;
  if (!e.block) { e.block = block; e.counted = true; _galleryBytes += e.bytes; }
  enforceGalleryCap();
  return block;
}

/** Block for entry id, rehydrating from disk when spilled (null if lost). */
export async function getGraphById(id) {
  const e = _graphGallery.find(g => g.id === id);
  return e ? rehydrate(e) : null;
}

/** Block for entry id if it is resident now (sync), else null: a spilled entry
 *  is loaded only when asked for, through getGraphById. */
export function getResidentGraph(id) {
  const e = _graphGallery.find(g => g.id === id);
  return e && e.block ? e.block : null;
}

/** All blocks in order, rehydrating spilled ones one at a time (lost ones
 *  skipped). An async iterator, so the caller holds one rehydrated block at a
 *  time: collecting them all in one array kept every spilled plot in memory
 *  at once, past the gallery's byte budget (BUG-1472). */
export async function* getGraphsAsync() {
  for (const e of _graphGallery.slice()) {
    const b = await rehydrate(e);
    if (b) yield b;
  }
}

/** Entries preserved on disk right now (the gallery's "N stored on disk"). */
export function getGraphSpilledCount() {
  return _graphGallery.filter(g => g.stored).length;
}

/** Resolves once all in-flight spill writes settle. */
export async function flushGraphSpill() {
  while (_spillPending.size) await Promise.all([..._spillPending]);
}

/** Thumbnails dropped for good (storage unavailable/failed/over its cap). */
export function getGraphDroppedCount() {
  return _galleryElided;
}

/** Plots the sync getGraphs() does not return: dropped for good plus those
 *  preserved on disk. Keeps the legacy "N older plots trimmed" note honest for
 *  callers that only render getGraphs(). */
export function getGraphGalleryElidedCount() {
  return _galleryElided + _graphGallery.filter(g => !g.block).length;
}

/** U7 (P8 Honest Cockpit): explicit gallery clear. `cls` no longer wipes the
 *  gallery (Stata parity), so this is the dedicated primitive for dropping
 *  collected plots — the Stata semantic owner is `graph drop _all`. (Wiring
 *  that command to this is a small follow-up.) */
export function clearGraphGallery() {
  for (const e of _graphGallery) e.state = 'dead';
  _graphGallery.length = 0;
  _galleryBytes  = 0;
  _galleryElided = 0;
  _spillBytes    = 0;
  _galleryGen++;
  const sp = _spill;
  if (sp) sp.clear().catch(() => {});
}

// ─── Empty-state management ──────────────────────────────────────────────────

function hideEmpty() {
  if (_emptyEl) _emptyEl.classList.add('hidden');
}

function showEmpty() {
  if (_emptyEl) _emptyEl.classList.remove('hidden');
}

// ─── Block renderers ─────────────────────────────────────────────────────────

function renderEcho(block) {
  const div = document.createElement('div');
  div.className = 'block block-echo';
  div.textContent = '. ' + block.text;
  return div;
}

function renderDisplay(block) {
  // `display` output is plain result text, NOT a command echo — no '. ' prefix.
  const div = document.createElement('div');
  div.className = 'block block-display';
  div.textContent = block.text ?? '';
  return div;
}

/** B9 (P8-S7): a plain-text annotation — NOT a command, NOT engine output —
 *  rendered italic with NO '. ' prompt (SPEC III-6 label honesty: it visibly
 *  reads as "not a command" at a glance). Extends renderDisplay()'s plain-
 *  text idiom with one added class (.block-note, piet.css) rather than a
 *  second render path. */
function renderNote(block) {
  const div = document.createElement('div');
  div.className = 'block block-display block-note';
  div.textContent = block.text ?? '';
  return div;
}

// ─── Correlation matrix (correlate / pwcorr) ───────────────────────────────────
// Stata BE 19.5 lower-triangular layout. The block (estimator==='corr') carries
// the full matrix forwarded by the dispatcher: r (2D), variables (names),
// and — for pwcorr,sig — p (2D p-values) and nobs (2D per-pair N).
export function formatCorrelate(block) {
  const names = block.variables || [];
  const r = block.r || [];
  if (!names.length || !r.length) return '(no correlation matrix)';

  const isPwcorr = block.method === 'pwcorr' || block.pairwise === true;
  const showSig  = block.sig === true && Array.isArray(block.p);
  // BS-028 companion (BETA C2+C3): `pwcorr ..., obs` — real Stata prints the
  // per-pair N beneath each coefficient (verified against a live
  // `StataBE -b do` run of `pwcorr income beertax spirits, obs` on the same
  // fatalities data this arm's other fixtures use). Gated on block.obs===true
  // (only pwcorr sets this; correlate,obs is rejected upstream by the
  // dispatcher) rather than mere nobs presence — nobs already flows on every
  // pairwise corr block regardless of the obs flag (dispatcher.cpp:2863-2867),
  // so keying off nobs alone would turn the row on unrequested.
  const showObs  = block.obs === true && Array.isArray(block.nobs);

  const fmt4 = (x) => (x === null || x === undefined || Number.isNaN(x))
    ? '' : Number(x).toFixed(4);
  const fmtNobs = (x) => (x === null || x === undefined || Number.isNaN(x))
    ? '' : String(Math.round(Number(x)));

  const COL = 9;                                  // each numeric column is %9.4f wide
  const stubW = Math.max(12, ...names.map(n => n.length));
  const ncol = names.length;

  const lines = [];
  // correlate (listwise) prints "(obs=N)"; pwcorr does not.
  if (!isPwcorr) {
    const nObs = block.n_obs ?? block.nobs ?? '';
    lines.push(`(obs=${nObs})`);
    lines.push('');
  }

  // Header: blank stub + ' │' + each var name right-aligned in COL.
  lines.push(' '.repeat(stubW) + ' │' + names.map(n => padL(n, COL)).join(''));
  // Rule: stubW+1 dashes + ┼ + COL*ncol dashes.
  lines.push('─'.repeat(stubW + 1) + '┼' + '─'.repeat(COL * ncol));

  for (let i = 0; i < ncol; i++) {
    // Coefficient row: lower-triangular (cols 0..i).
    const coefCells = [];
    for (let j = 0; j <= i; j++) coefCells.push(padL(fmt4(r[i][j]), COL));
    // Stata adds a trailing space after the last coef on pwcorr rows.
    lines.push(padL(names[i], stubW) + ' │' + coefCells.join('') + (isPwcorr ? ' ' : ''));

    if (showSig) {
      // Significance row: p-values for strictly-lower cells (cols 0..i-1);
      // the diagonal/self cell is blank.
      const sigCells = [];
      for (let j = 0; j < i; j++) sigCells.push(padL(fmt4(block.p[i][j]), COL));
      lines.push(' '.repeat(stubW) + ' │' + sigCells.join(''));
    }

    if (showObs) {
      // pwcorr,obs: per-pair N beneath the coefficient row, cols 0..i
      // INCLUDING the diagonal (nobs[i][i] is var i's own N — unlike the sig
      // row above, a pair's own obs count is well-defined, so real Stata
      // prints it; verified: the "income" row of a live `pwcorr ..., obs`
      // run shows a lone "336" under its single 1.0000 diagonal cell).
      const obsCells = [];
      for (let j = 0; j <= i; j++) obsCells.push(padL(fmtNobs(block.nobs[i][j]), COL));
      lines.push(' '.repeat(stubW) + ' │' + obsCells.join(''));
    }

    if (showSig || showObs) {
      // Blank spacer row (just the stub pipe) before the next var's row.
      lines.push(' '.repeat(stubW) + ' │');
    }
  }
  return lines.join('\n');
}

function renderEstimation(block) {
  const div = document.createElement('div');
  div.className = 'block block-estimation';

  // BUG-112(8): non-fatal model-spec notes from the engine, rendered ABOVE the
  // table because that is where Stata prints them. One `note: ` line each, the
  // engine's own wording — this layer does not rephrase what the engine said.
  if (Array.isArray(block.warnings) && block.warnings.length) {
    const notes = document.createElement('pre');
    notes.className = 'coeftable est-notes';
    notes.textContent = block.warnings.map(w => `note: ${w}`).join('\n');
    div.appendChild(notes);
  }

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  // corr/correlate/pwcorr → dedicated matrix renderer (Stata lower-triangular).
  pre.textContent = block.estimator === 'corr'
    ? formatCorrelate(block)
    : formatEstimation(block);
  div.appendChild(pre);

  // csdid `estat event`: draw the event-study plot below the coefficient table.
  if (block.estat === 'event' && Array.isArray(block.event_study) && block.event_study.length) {
    div.appendChild(renderEventStudyBlock(block));
  }
  return div;
}

function renderSummarize(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatSummarize(block);
  div.appendChild(pre);
  return div;
}

function renderDescribe(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatDescribe(block);
  div.appendChild(pre);
  return div;
}

function renderMutation(block) {
  const div = document.createElement('div');
  div.className = 'block block-mutation';

  const { action, new_vars, dropped_vars, n_rows_after, varlist_after, renamed } = block;
  let msg = '';
  if (action === 'open') {
    const nCols = varlist_after ? varlist_after.length : '?';
    msg = `(${nCols} vars, ${fmtObs(n_rows_after)} obs loaded)`;
  } else if (action === 'gen' || action === 'egen') {
    if (new_vars && new_vars.length) {
      msg = new_vars.map(v => `(variable ${v} created)`).join('\n');
    }
  } else if (action === 'drop') {
    if (dropped_vars && dropped_vars.length) {
      msg = `(${dropped_vars.length} variable${dropped_vars.length > 1 ? 's' : ''} dropped)`;
    } else {
      msg = `(obs dropped, ${fmtObs(n_rows_after)} obs remaining)`;
    }
  } else if (action === 'keep' || action === 'keepif' || action === 'replace') {
    msg = `(${fmtObs(n_rows_after)} obs remaining)`;
  } else if (action === 'rename' && renamed) {
    msg = `(${renamed.from} renamed to ${renamed.to})`;
  } else if (action === 'recode' && new_vars && new_vars.length) {
    msg = `(recode: new variable ${new_vars[0]} created)`;
  } else {
    msg = `(${action} completed)`;
  }
  div.textContent = msg;
  return div;
}

// A18: `factor_misuse`'s engine message tells the user to "re-run to_hbin
// WITHOUT --factor on 'col'" — to_hbin is an internal dev CLI tool end users
// never touch. Extract the column name out of that message (both emission
// sites end it `on 'NAME'` — apps/cmds/common.cpp:448 (structured,
// code=factor_misuse) and apps/cmds/cmds_ts.cpp:147 (plain stderr text,
// code defaults to engine_error) — so the message-substring fallback below
// covers the second site too) and translate to the Stata-idiom fix.
function toHbinHint(message) {
  const m = /on '([^']+)'/.exec(message || '');
  const col = m ? m[1] : 'this column';
  return `'${col}' is a text/factor variable — make a numeric id first: ` +
    `\`egen id = group(${col})\`, then use \`id\`.`;
}

// B1 (SEED, hard-capped ≤20 entries): curated code/pattern → ONE
// plain-language why/fix line, appended BELOW the real code+message (never
// replacing it — two-tier: honest machine line + one plain line). Keyed by
// `code` first (the engine's stable vocabulary — src/core/error.h — plus
// the PIET-layer parse/dispatch codes — piet/native/include/piet/eval/
// errors.h); message-substring patterns below catch the same failure when
// it surfaces under a different/generic code. Table, not branches
// (IDIOMS.md "declarative tables over bespoke branches").
const ERROR_HINTS_BY_CODE = {
  factor_misuse:       (b) => toHbinHint(b.message),
  missing_column:      () => "Check the variable name for typos — it isn't in the currently loaded dataset (`describe` lists what's loaded).",
  unsupported_option:  () => 'This option isn’t recognized/supported for this command — check the spelling, or drop it and re-run without it.',
  unsupported_command: () => "This Stata command isn't implemented in PIET yet.",
  no_data_loaded:      () => 'No dataset is loaded — run `use "yourfile.dta"` (or `sysuse`) first.',
  syntax_error:        () => "PIET couldn't parse this line — check for a missing comma before options, an unmatched quote, or a typo in the command name.",
  collinear:           () => 'The regressors are perfectly collinear (one is an exact combination of the others) — drop or combine the redundant variable(s).',
  under_identified:    () => 'Too few instruments for the endogenous regressors — add another excluded instrument or drop an endogenous variable.',
  no_obs:              () => 'Too few usable observations after listwise deletion — check for missing values in the variables you’re using.',
  no_convergence:      () => 'The iterative estimator didn’t converge — try dropping variables, rescaling, or simplifying the model.',
  common_sample:       () => "The outcomes in this multi-equation command aren't jointly observed — check for missing values across all outcome variables.",
  bad_input:           () => 'One of the option/argument values isn’t valid for this command — double-check the syntax.',
  data_io:             () => "The dataset file couldn't be read — check the path and that it's a valid .dta file.",
};
const ERROR_HINTS_BY_MESSAGE = [
  { test: /to_hbin/i,        hint: (msg) => toHbinHint(msg) },
  // BUG-128: "file not found" (joinby/merge/append using, do, confirm file, …)
  // was falling through to the generic /not found/i entry below and getting
  // the variable-name hint — misdirects a missing-FILE error toward variable
  // typos. Must come first in the list: Array.find() takes the first match.
  { test: /file not found/i, hint: () => "Check the file path — PIET couldn't find that file on disk (relative paths resolve from the current working directory)." },
  { test: /not found/i,      hint: () => "Check the variable name for typos — it isn't in the currently loaded dataset (`describe` lists what's loaded)." },
];
export function noviceErrorHint(block) {
  const byCode = ERROR_HINTS_BY_CODE[block.code];
  if (byCode) return byCode(block);
  const msg = block.message || '';
  const entry = ERROR_HINTS_BY_MESSAGE.find(e => e.test.test(msg));
  return entry ? entry.hint(msg) : null;
}

function renderError(block) {
  const div = document.createElement('div');
  div.className = 'block block-error';

  const { code, message, token, line_number, context, reason } = block;
  // Show error code + token so users can identify what was rejected
  // (e.g. "unsupported_option: aweights" alongside the human-readable message)
  const codeLabel = token
    ? `${escHtml(code)}: ${escHtml(token)}`
    : escHtml(code);
  let html = `<span class="err-code">${codeLabel}</span>\n<span class="err-msg">${escHtml(message)}</span>`;
  if (line_number !== undefined && context) {
    html += `\n<span class="err-stop">line ${line_number}: ${escHtml(context)}</span>`;
  }

  // Contract B v1.8.0: optional `reason` field with a user-facing hint.
  // Also surface the cached PIET version (fetched from GET /api/version on connect).
  if (reason) {
    const pietVer = getCachedPietVersion();
    const verStr  = pietVer ? `PIET ${escHtml(pietVer)}` : 'PIET';
    let hint = '';
    if (reason === 'piet_gap') {
      hint = `(not yet wired in ${verStr} — a future update will add it)`;
    } else if (reason === 'engine_gap') {
      hint = '(the cobra engine does not support this yet)';
    }
    if (hint) {
      html += `\n<span class="err-hint">${hint}</span>`;
    }
  }

  // A18/B1: curated plain-language fix line for known novice failures —
  // reuses .err-hint styling (same muted supplementary-note treatment as
  // the reason banner above) rather than adding a new CSS class, since this
  // fix is JS-only scope. Never replaces the real code/message above it.
  const novice = noviceErrorHint(block);
  if (novice) {
    html += `\n<span class="err-hint">${escHtml(novice)}</span>`;
  }

  div.innerHTML = html;

  // T4 (P8.1): error-click hook — a block carrying line_number becomes
  // clickable, dispatching a CustomEvent for the Do-file Editor / gutter to
  // react to (see INTEGRATION in the sprint report for the app.js listener
  // that calls openDoEditor()/selectEditorLine()/setGutterErrorLine()). No
  // app.js import here — mirrors panels.js's own cross-module CustomEvent
  // idiom ('piet:var-edit', 'piet:hist-send-doedit'). textContent is
  // untouched (constraint (b)) — only a class, a title, and a listener.
  if (line_number !== undefined) {
    div.classList.add('err-jump');
    div.title = `Click to jump to line ${line_number} in the Do-file Editor`;
    div.addEventListener('click', () => {
      document.dispatchEvent(new CustomEvent('piet:error-line-click', {
        detail: { line: line_number },
      }));
    });
  }

  return div;
}

// ─── THE CRITICAL PATH: formatEstimation() ───────────────────────────────────
// Verified against piet/fixtures/golden/{feols_plain,feols_robust,feols_cluster,
//                                         feols_factor,feols_fe,feols_multi}.json

/**
 * Given an EstimationResult (CONTRACT.md shape), return a multi-line monospace
 * string that looks like Stata's regress/feols output.
 */
export function formatEstimation(block) {
  const { estimator, outcomes } = block;

  // BUG-015: `test` (post-estimation Wald) has NO coeftable — render the
  // constraint list + test statistic straight from the block fields, before
  // the coeftable path turns it into "(no results)".
  if (estimator === 'test') {
    return formatTestBlock(block);
  }

  // BS-026 (BETA Phase C batch C1 estimation_m4.cpp:161-179): `tabstat` has NO
  // coeftable — it carries `vars`/`stats`/`total` (+ optional `by`) instead.
  // Render straight from those fields, before the coeftable path turns it
  // into "(no results)".
  if (estimator === 'tabstat') {
    return formatTabstatBlock(block);
  }

  // A19: `estat vif`/`estat hettest`/`estat ovtest` have NO Coefficient/Std.
  // err./t/P>|t| coeftable in the Stata sense — vif's coeftable rows carry
  // `b` only (se/t/p/ci null), and hettest/ovtest carry no coeftable at all.
  // Render each from its own dedicated fields before the coeftable path
  // below turns them into a mislabeled shell or "(no results)".
  if (estimator === 'vif') {
    return formatVifBlock(block);
  }
  if (estimator === 'hettest') {
    return formatHettestBlock(block);
  }
  if (estimator === 'ovtest') {
    return formatOvtestBlock(block);
  }

  // Multi-equation: render each outcome sub-table
  if (outcomes && outcomes.length > 0) {
    return outcomes.map((oc, i) => {
      const sub = {
        estimator,
        depvar:     oc.depvar,
        n_obs:      oc.n_obs    || block.n_obs,
        coeftable:  oc.coeftable,
        r2:         oc.r2,
        r2_within:  oc.r2_within,
        vcov_type:  oc.vcov_type || block.vcov_type,
        n_clusters: oc.n_clusters != null ? oc.n_clusters : block.n_clusters,
        dof_resid:  oc.dof_resid,
        absorbed:   {},
        sigma2:     oc.sigma2,
      };
      const label = `── ${oc.depvar} `;
      return label + '\n' + formatOneTable(sub);
    }).join('\n\n');
  }

  if (!block.coeftable || !block.coeftable.length) return formatGenericBlock(block);
  return formatOneTable(block);
}

/**
 * BUG-015: render a `test` (post-estimation Wald) block. It carries no
 * coeftable — only the joint-restriction fields. Mirrors Stata's `test`:
 *   ( 1)  mpg = 0
 *   ( 2)  weight = 0
 *
 *        F(  2,    71) =   14.74
 *             Prob > F =   0.0000
 * iid vcov → F form (F / Prob > F); robust/cluster vcov → chi2 form
 * (chi2 / Prob > chi2) — same OLS-vs-chi2 split used for the model-fit header.
 */
function formatTestBlock(block) {
  const lines = [];

  const restrictions = block.restrictions || [];
  restrictions.forEach((r, i) => {
    lines.push(`( ${padL(String(i + 1), 1)})  ${r}`);
  });

  if (restrictions.length) lines.push('');

  // BUG-1057: a statistic the engine could not form (R V R' exactly 0 after an
  // exact fit) arrives null, which Stata prints as `.` (StataBE 19.5 `test x` after
  // `regress y2 x` on y2 = 3 + 2*x: `F(  1,    48) =       .`, `Prob > F =
  // .`); Number(null) printed 0.00 and fmtP(null) nothing.
  const missing = (x) => x == null || !Number.isFinite(Number(x));
  const stat = (x) => (missing(x) ? '.' : Number(x).toFixed(2));
  const prob = (x) => (missing(x) ? '.' : fmtP(Number(x)));

  // iid → F statistic; robust/cluster → Wald chi2 (matches buildStats footer).
  if (block.vcov_type !== 'iid') {
    lines.push(`chi2(${block.df1}) = ${stat(block.chi2)}`);
    lines.push(`Prob > chi2 = ${prob(block.p_chi2)}`);
  } else {
    lines.push(`F(${block.df1}, ${Math.round(block.df2)}) = ${stat(block.F)}`);
    lines.push(`Prob > F = ${prob(block.p_F)}`);
  }

  return lines.join('\n');
}

/**
 * BS-026 (BETA Phase C batch C1): render a `tabstat` block — one row per
 * `vars[]` entry, one column per `stats[]` entry, values from `total`
 * (`{var:{stat:value}}`). Mirrors formatSummarize()'s stub+data-column layout.
 * Numbers go through fmtG (the same %9.0g-ish general formatter every other
 * table in this file uses) so tabstat reads consistently with summarize/
 * regress output.
 *
 * `by(...)` support: the engine currently echoes back the by-var name but
 * does NOT (yet) compute per-group breakdowns — `total` is always the
 * grand-total table regardless of `by`. Rather than silently drop the
 * by() request or fabricate group rows, render the honest total table plus
 * a note that per-group rows are pending engine support.
 */
function formatTabstatBlock(block) {
  const { vars, stats, total, by } = block;
  if (!vars || !vars.length || !stats || !stats.length || !total) return '(no results)';

  const stubHdr = 'Variable';
  const dataHdrs = stats;

  const dataRows = vars.map(v => {
    const row = total[v] || {};
    return stats.map(s => fmtG(row[s]) ?? '');
  });

  const stubW = Math.max(stubHdr.length, ...vars.map(v => v.length));
  const dataW = dataHdrs.map((h, i) =>
    Math.max(h.length, ...dataRows.map(r => r[i].length))
  );

  const hdr = padL(stubHdr, stubW) + ' │' +
    dataHdrs.map((h, i) => padL(h, dataW[i] + 3)).join('');
  const dataRegionW = dataW.reduce((a, w) => a + w + 3, 0);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(dataRegionW);

  const lines = [hdr, rule];
  for (let r = 0; r < vars.length; r++) {
    lines.push(padL(vars[r], stubW) + ' │' +
      dataRows[r].map((v, i) => padL(v, dataW[i] + 3)).join(''));
  }

  // `total` never carries by-group rows today (see doc comment above) — the
  // check is written against that fact rather than assuming it forever, so
  // a future engine change that adds real per-group data isn't silently
  // masked by this note.
  const hasPerGroupData = by && Array.isArray(block.by_groups) && block.by_groups.length > 0;
  if (by && !hasPerGroupData) {
    lines.push('');
    lines.push(`(per-group breakdown by ${by} is engine-pending — showing overall totals only)`);
  }

  return lines.join('\n');
}

/**
 * A19: render `estat vif` as its own Variable/VIF/1-over-VIF table — NOT
 * the Coefficient/Std.err./t/P>|t| coeftable shell (vif's coeftable rows
 * carry `b` only; se/t/p/ci are null, which is what produced the
 * mislabeled empty-column render this fixes). Diagnosed native shape
 * (piet_native --do, 2026-07-16): the block carries VIF values twice — once
 * folded into `coeftable[].b`/`.name` (compat shape shared with other
 * estimators) and once in a clean `coefs: [{name, b}]` array. We read
 * `coefs` first and fall back to `coeftable` so either shape renders.
 * No native `tol` (1/VIF) field is emitted — computed client-side from `b`.
 * Row order is left exactly as the native block emits it (no client-side
 * re-sort) rather than guessing at a descending-VIF convention.
 */
function formatVifBlock(block) {
  const rows = (block.coefs || block.coeftable || [])
    .map(r => ({ name: r.name, vif: r.b }))
    .filter(r => r.vif != null);
  if (!rows.length) return '(no results)';

  const stubHdr = 'Variable', vifHdr = 'VIF', invHdr = '1/VIF';
  const fVif = rows.map(r => r.vif.toFixed(2));
  const fInv = rows.map(r => (1 / r.vif).toFixed(6));

  const stubW = Math.max(stubHdr.length, ...rows.map(r => r.name.length));
  const vifW  = Math.max(vifHdr.length, ...fVif.map(s => s.length));
  const invW  = Math.max(invHdr.length, ...fInv.map(s => s.length));

  const hdr  = padL(stubHdr, stubW) + ' │' + padL(vifHdr, vifW + 3) + padL(invHdr, invW + 3);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(vifW + 3 + invW + 3);

  const lines = [hdr, rule];
  rows.forEach((r, i) => {
    lines.push(padL(r.name, stubW) + ' │' + padL(fVif[i], vifW + 3) + padL(fInv[i], invW + 3));
  });

  const meanVif = rows.reduce((a, r) => a + r.vif, 0) / rows.length;
  lines.push(rule);
  lines.push(padL('Mean VIF', stubW) + ' │' + padL(meanVif.toFixed(2), vifW + 3));

  return lines.join('\n');
}

/**
 * A19: `estat hettest` (Breusch-Pagan / Cook-Weisberg) has NO coeftable —
 * diagnosed native shape carries `chi2`/`df`/`p` (plus an unused `F`/`p_F`
 * pair the native block also emits; Stata's default hettest report is the
 * single chi2(df) statistic, so only that pair is surfaced here) straight
 * on the estimation block, same no-coeftable family as `test`
 * (formatTestBlock above).
 */
function formatHettestBlock(block) {
  const { chi2, df, p } = block;
  if (chi2 == null) return '(no results)';
  return [
    'Breusch-Pagan / Cook-Weisberg test for heteroskedasticity',
    '    Ho: Constant variance',
    '',
    `    chi2(${df}) = ${Number(chi2).toFixed(2)}`,
    `    Prob > chi2 = ${fmtP(p)}`,
  ].join('\n');
}

/**
 * A19: `estat ovtest` (Ramsey RESET) has NO coeftable — diagnosed native
 * shape carries `F`/`df_m`/`df_r`/`p` straight on the estimation block,
 * same no-coeftable family as `test`/`hettest`.
 */
function formatOvtestBlock(block) {
  const { F, df_m, df_r, p } = block;
  if (F == null) return '(no results)';
  return [
    'Ramsey RESET test using powers of the fitted values',
    '    Ho: model has no omitted variables',
    '',
    `    F(${df_m}, ${Math.round(df_r)}) = ${Number(F).toFixed(2)}`,
    `    Prob > F = ${fmtP(p)}`,
  ].join('\n');
}

/** Render estimation payloads that have useful scalars/rows but no coeftable. */
function formatGenericBlock(block) {
  const mappedTitle = estimatorTitle(block.estimator, []);
  const title = mappedTitle === 'Estimation' && block.estimator
    ? String(block.estimator)
    : mappedTitle;
  const lines = [title];
  const skip = new Set(['type', 'estimator', 'captured', 'coeftable']);

  const valueText = (value) => {
    if (typeof value === 'number') return fmtG(value) ?? String(value);
    if (typeof value === 'string' || typeof value === 'boolean') return String(value);
    if (value === null || value === undefined) return '';
    try { return JSON.stringify(value); }
    catch (_) { return String(value); }
  };

  const scalars = Object.entries(block).filter(([key, value]) => {
    if (skip.has(key) || (key === 'depvar' && !value)) return false;
    return ['number', 'string', 'boolean'].includes(typeof value);
  });
  if (scalars.length) {
    const nameW = Math.max(...scalars.map(([key]) => key.length));
    lines.push('');
    for (const [key, value] of scalars) {
      lines.push(`${padL(key, nameW)} = ${valueText(value)}`);
    }
  }

  for (const [name, rows] of Object.entries(block)) {
    if (!Array.isArray(rows) || rows.length === 0 || skip.has(name)) continue;
    const objectRows = rows.every(row => row && typeof row === 'object' && !Array.isArray(row));
    if (!objectRows) {
      lines.push('', `${name} = ${valueText(rows)}`);
      continue;
    }

    const keys = [];
    for (const row of rows) {
      for (const key of Object.keys(row)) {
        if (!keys.includes(key)) keys.push(key);
      }
    }
    if (!keys.length) continue;
    const cells = rows.map(row => keys.map(key => valueText(row[key])));
    const widths = keys.map((key, i) => Math.max(key.length, ...cells.map(row => row[i].length)));
    lines.push('', `${name}:`);
    lines.push(keys.map((key, i) => padR(key, widths[i])).join('   '));
    lines.push(widths.map(width => '─'.repeat(width)).join('───'));
    for (const row of cells) {
      lines.push(row.map((cell, i) => padR(cell, widths[i])).join('   '));
    }
  }

  return lines.join('\n');
}

function formatOneTable(block) {
  const {
    depvar, n_obs, coeftable, r2, r2_within, sigma2,
    vcov_type, n_clusters, absorbed, dof_resid,
    first_stage_F, kp_wald_F, hansen_j, hansen_j_p,
    // MI-081 / Contract B v1.8.0 model-fit scalars (all optional):
    adj_r2, F, p_F, rmse, mss, rss, df_m, df_r,          // OLS family
    df_b,                                                  // xtreg FE header numerator
    ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,           // logit/probit / xtreg RE
    r2_between, r2_overall,                                // xtreg RE
  } = block;

  if (!coeftable || coeftable.length === 0) return '(no results)';

  // Rename (intercept) → _cons (Stata convention)
  const renamedRows = coeftable.map(r => ({
    ...r,
    displayName: r.name === '(intercept)' ? '_cons' : (r.stub || r.name),  // BUG-397: value-label display rides on `stub`; `name` stays the e(b) colname
  }));

  // A14: Stata renders the intercept/_cons row LAST, but the engine emits it
  // FIRST. Display-layer-only reorder: stable move-intercept-to-end.
  const isIntercept = r => r.name === '(intercept)' || r.name === '_cons';
  const rows = [
    ...renamedRows.filter(r => !isIntercept(r)),
    ...renamedRows.filter(r => isIntercept(r)),
  ];

  // ── Format all cell values ──────────────────────────────────────────────────
  const fB   = rows.map(r => r.b  === null ? '(omitted)' : (fmtG(r.b)  ?? ''));
  const fSE  = rows.map(r => r.se === null ? ''           : (fmtG(r.se) ?? ''));
  const fT   = rows.map(r => r.t  === null ? ''           : fmtT(r.t));
  const fPv  = rows.map(r => r.p  === null ? ''           : fmtP(r.p));
  const fCLo = rows.map(r => r.ci_lo === null ? '' : (fmtG(r.ci_lo) ?? ''));
  const fCHi = rows.map(r => r.ci_hi === null ? '' : (fmtG(r.ci_hi) ?? ''));

  // ── Column widths = max(header, all formatted values) ──────────────────────
  const nameW = Math.max(
    depvar ? depvar.length : 0,
    ...rows.map(r => r.displayName.length),
    4
  );
  const bW   = Math.max(11, ...fB.map(s => s.length));   // "Coefficient"
  const seW  = Math.max(9,  ...fSE.map(s => s.length));  // "Std. err."
  const tW   = Math.max(6,  ...fT.map(s => s.length));   // "t" → 6 for spacing
  const pW   = Math.max(5,  ...fPv.map(s => s.length));  // "P>|t|"
  const cLoW = Math.max(9,  ...fCLo.map(s => s.length));
  const cHiW = Math.max(9,  ...fCHi.map(s => s.length));

  // The column data region (after the ` │`; note rule uses nameW+1 to align ┬ with │)
  const dataRegionW = 1 + bW + 3 + seW + 3 + tW + 3 + pW + 5 + cLoW + 3 + cHiW;

  // ── Header section ─────────────────────────────────────────────────────────
  const absKeys = absorbed ? Object.keys(absorbed) : [];
  let title = estimatorTitle(block.estimator, absKeys);

  // BUG-017: MLE estimators (logit/probit/poisson/nbreg/...) report z / P>|z|;
  // OLS-family estimators report t / P>|t|. estimator is on the block (Boundary B).
  // BUG-205(1): `lincom`'s own estimator label never changes with what it was
  // computed FROM — pass vcov_type too so statLetter can key `lincom` on the
  // fit (oim -> z) rather than on the verb (which would always say t).
  const sl   = statLetter(block.estimator, vcov_type);
  const tHdr = sl;                 // 't' or 'z'
  const pHdr = 'P>|' + sl + '|';   // 'P>|t|' or 'P>|z|'

  const statsArr = buildStats({
    n_obs, r2, r2_within, sigma2, dof_resid, absKeys,
    // MI-081 new scalars
    adj_r2, F, p_F, rmse, mss, rss, df_m, df_r, df_b,
    ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,
    r2_between, r2_overall,
  });
  const statLines = formatStatLines(title, statsArr, nameW + 1 + dataRegionW);

  // ── Rules ──────────────────────────────────────────────────────────────────
  // nameW+1 so that ┬/┼/┴ aligns with the │ that follows the 1-char space after name
  const ruleN  = '─'.repeat(nameW + 1);
  const ruleD  = '─'.repeat(dataRegionW);

  // ── Column header row ──────────────────────────────────────────────────────
  // "[95% Conf. Interval]" spans both CI columns (cLoW + 3 sep + cHiW wide)
  const ciLabel  = '[95% conf. interval]';
  const ciTotalW = cLoW + 3 + cHiW;
  const ciHeaderSection = padR(ciLabel, ciTotalW); // left-align within CI region

  // WS1 #1: Stata flips the SE label to lowercase under a non-iid VCE.
  //   iid → "Std. err." ; robust/hc1/cluster(...) → "std. err."
  const _vc = (vcov_type || '').toLowerCase();
  const seLabel = (_vc === 'robust' || _vc === 'hc1' || _vc.startsWith('cluster('))
    ? 'std. err.' : 'Std. err.';
  // WS1 #2: margins reports marginal effects → the coef column header is "dy/dx".
  // BS-022 companion (BETA Phase C batch C1 dispatcher.cpp:2706): `logistic`
  // exponentiates the coeftable's b/se/ci to odds ratios (log-odds for `logit`
  // is untouched) — the coef column header must say so, or the values read as
  // a mislabeled "Coefficient" column.
  const bLabel = block.estimator === 'margins' ? 'dy/dx'
    : block.estimator === 'logistic' ? 'Odds ratio'
    : 'Coefficient';

  const headerRow = [
    padL(depvar || '', nameW), ' │',
    padL(bLabel,   bW  + 1),   // 1 leading space + bW ("Coefficient" / "dy/dx")
    padL(seLabel,  seW + 3),   // 3 leading spaces + seW ("Std. err." / "std. err.")
    padL(tHdr,        tW  + 3),   // 3 leading spaces + tW ('t' or 'z')
    padL(pHdr,        pW  + 3),   // 3 leading spaces + pW ('P>|t|' or 'P>|z|')
    '     ',                       // 5-char gap before CI columns
    ciHeaderSection,
  ].join('');

  // WS1 #3: margins prepends a "Delta-method" banner line above the column names,
  //   right-aligned over the SE column (Stata layout).
  const marginsBanner = block.estimator === 'margins'
    ? [padL('', nameW), ' │',
       padL('', bW + 1),
       padL('Delta-method', seW + 3)].join('')
    : null;

  // ── Data rows ──────────────────────────────────────────────────────────────
  const dataRows = rows.map((r, i) => {
    const omitted = r.b === null;
    return [
      padL(r.displayName, nameW), ' │',
      padL(fB[i],                  bW  + 1),
      padL(omitted ? '' : fSE[i],  seW + 3),
      padL(omitted ? '' : fT[i],   tW  + 3),
      padL(omitted ? '' : fPv[i],  pW  + 3),
      '     ',                                          // 5-char gap
      padL(omitted ? '' : fCLo[i], cLoW),              // CI lo right-aligned
      '   ',                                            // 3-char separator
      padL(omitted ? '' : fCHi[i], cHiW),              // CI hi right-aligned
    ].join('');
  });

  // ── Footers ────────────────────────────────────────────────────────────────
  const footerLines = buildFooter({
    vcov_type, n_clusters, absKeys, absorbed,
    first_stage_F, kp_wald_F, hansen_j, hansen_j_p,
  });

  // ── Assemble ───────────────────────────────────────────────────────────────
  const lines = [
    ...statLines,
    '',
    ruleN + '┬' + ruleD,
    ...(marginsBanner ? [marginsBanner] : []),   // WS1 #3: Delta-method banner (margins only)
    headerRow,
    ruleN + '┼' + ruleD,
    ...dataRows,
    ruleN + '┴' + ruleD,
    ...footerLines,
  ];

  return lines.join('\n');
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// BUG-017: test-statistic letter is z (P>|z|) for large-sample-normal estimators
// — MLE, GMM-2SLS, SUR/3SLS, GLS random-effects, and TS-ML — and t (P>|t|) only
// for least-squares / small-sample-normal estimators. We invert to a small OLS_T
// allow-list and DEFAULT everything else to z, so a newly-supported estimator
// (which is almost always large-sample) is never silently mislabeled t.
//
// t / P>|t| — least-squares family (must list every OLS-ish estimatorTitle key):
//   feols, feols_multi, ols, reg, r, regress, areg, reghdfe, xtreg_fe, xtreg_be,
//   newey, prais, qreg, sqreg, iqreg.
// z / P>|z| — EVERYTHING ELSE estimatorTitle knows, i.e. the MLE / 2SLS / SUR /
//   RE-GLS / TS-ML estimators: logit, logistic, probit, cloglog, poisson, fepois,
//   ppmlhdfe, nbreg, tobit, heckman, glm, mlogit, ologit, oprobit, truncreg,
//   xtlogit, xtprobit, xtpoisson, xtnbreg, xtreg / xtreg_re, iv, iv2sls,
//   iv2sls_multi, ivregress, reg3, sureg, arima, arch, var, vec.
const OLS_T_ESTIMATORS = new Set([
  'feols','feols_multi','ols','reg','r','regress','areg',
  // BS-026 companion: `reghdfe` IS feols --fe under the hood (cobra has no
  // separate reghdfe estimator — handlers.cpp:1022 label-only override), so
  // it belongs in the same least-squares t-stat class as feols/areg.
  'reghdfe',
  'xtreg_fe','xtreg_be','newey','prais','qreg','sqreg','iqreg',
  // NOTE: 'lincom' deliberately does NOT live in this allow-list (BUG-205(1)
  // reverted WS1 #10's addition of it) — `lincom`'s own estimator label never
  // changes with what it was computed FROM, so keying off the VERB always
  // said `t` even after logit/probit/poisson. See the `lincom` branch below,
  // which keys off the fit instead.
]);
// BUG-205(1): `lincom` is the one estimator whose OWN label ('lincom') tells
// you nothing about the model that produced it — Stata prints `z` for
// `lincom` after logit/probit/poisson and `t` after `regress`. The engine
// already carries the discriminator on the wire: `vcov_type` is `oim` on the
// ML (post-estimation covariance) refit and `iid`/`cluster(...)`/robust/... on
// the least-squares one (cmds_postest.cpp `cmd_lincom`). `nlcom` needs no such
// branch — it is already outside OLS_T_ESTIMATORS, i.e. always `z`, which is
// correct for BOTH its OLS and ML paths (Stata's nlcom is always a delta-
// method z, even after `regress`).
function statLetter(est, vcov_type) {
  if (est === 'lincom') return vcov_type === 'oim' ? 'z' : 't';
  return OLS_T_ESTIMATORS.has(est) ? 't' : 'z';
}

function estimatorTitle(est, absKeys) {
  const base = {
    feols:        'Linear regression',
    feols_multi:  'Linear regression (multiple outcomes)',
    ols:          'Linear regression',
    reg:          'Linear regression',
    // BS-026 companion: `reghdfe` IS feols --fe under the hood (see
    // OLS_T_ESTIMATORS comment above) — same title family as feols/regress.
    reghdfe:      'Linear regression',
    iv:           'IV (2SLS) regression',
    iv2sls:       'IV (2SLS) regression',
    iv2sls_multi: 'IV (2SLS) regression (multiple outcomes)',
    ivregress:    'IV regression',
    logit:        'Logistic regression',
    logistic:     'Logistic regression',
    probit:       'Probit regression',
    cloglog:      'Complementary log\u2013log regression',
    fepois:       'Poisson regression',
    poisson:      'Poisson regression',
    nbreg:        'Negative binomial regression',
    tobit:        'Tobit regression',
    heckman:      'Heckman selection model',
    xtreg:        'Random-effects GLS regression',
    xtreg_re:     'Random-effects GLS regression',
    xtreg_fe:     'Fixed-effects regression',
    xtreg_be:     'Between-effects GLS regression',
    xtlogit:      'Random-effects logistic regression',
    xtlogit_fe:   'Conditional fixed-effects logistic regression',
    xtprobit:     'Random-effects probit regression',
    xtpoisson:    'Random-effects Poisson regression',
    xtpoisson_fe: 'Conditional fixed-effects Poisson regression',
    qreg:         'Quantile regression',
    sqreg:        'Simultaneous quantile regression',
    iqreg:        'Interquantile range regression',
    reg3:         'Three-stage least squares',
    sureg:        'Seemingly unrelated regression',
    arima:        'ARIMA regression',
    arch:         'ARCH/GARCH regression',
    newey:        'Newey-West regression',
    prais:        'Prais-Winsten AR(1) regression',
    var:          'Vector autoregression (VAR)',
    vec:          'Vector error-correction model',
    margins:      'Average marginal effects',   // WS1 #4
    lincom:       'Linear combination',          // WS1 #10 (companion title)
    nlcom:        'Nonlinear combination',
  }[est] ?? 'Estimation';

  if (absKeys && absKeys.length > 0) {
    return base + ' with absorbed FEs';
  }
  return base;
}

/**
 * Build the right-column stats array for the estimation header.
 * Each element is [label, value_string].
 * Emits Stata-style model-fit rows from Contract B v1.8.0 scalars (all optional).
 *
 * Priority order (mirrors Stata output):
 *   OLS family   : Number of obs / F(df_m,df_r) / Prob>F / R² / Adj R² / Root MSE
 *   logit/probit : Number of obs / LR chi2(df) / Prob>chi2 / Log likelihood / Pseudo R²
 *   xtreg RE     : Number of obs / R-sq within/between/overall / Wald chi2(df) / Prob>chi2
 *   fallback     : Number of obs / R² / Root MSE (legacy path)
 */
function buildStats({
  n_obs, r2, r2_within, sigma2, dof_resid, absKeys,
  // OLS family
  adj_r2, F, p_F, rmse, mss, rss, df_m, df_r,
  // BUG-1291: xtreg, fe prints its model F as F(e(df_b), e(df_r))
  // (xtreg,fe's header), not over e(df_m), which counts the panel means
  // (StataBE "F(2, 148) = 1191.52" where e(df_m) is 31). Only that block carries it.
  df_b,
  // logit/probit / xtreg RE
  ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,
  // xtreg RE
  r2_between, r2_overall,
}) {
  const stats = [];

  // WS1 #8/#9: model-fit header scalars follow Stata's fixed-decimal conventions
  //   (not fmtG/fmtP): chi2 & F test statistics use %9.2f (keep the leading zero,
  //   e.g. 0.31 not .31); header probabilities (Prob > chi2 / Prob > F) use 4 dp.
  const fmt2f  = (x) => (x == null || !Number.isFinite(x)) ? '?' : Number(x).toFixed(2);
  const fmtP4h = (x) => {
    if (x == null || !Number.isFinite(x)) return '?';
    if (x < 0.00005) return '0.0000';
    if (x > 0.99995) return '1.0000';
    return Number(x).toFixed(4);
  };

  // Always show obs count first.
  if (n_obs != null) stats.push(['Number of obs', fmtObs(n_obs)]);

  const hasAbsorbed = absKeys && absKeys.length > 0;

  // ── Panel R² path (xtreg): has r2_within + r2_between + r2_overall ──────────
  // Covers both xtreg,RE (Wald chi2 test) and xtreg,FE / single-FE feols
  // (F test — cobra 0.15.1 emits F/p_F/df_m/df_r and NO chi2 for the FE route).
  if (r2_between != null || r2_overall != null) {
    // Show whichever of the three R² types are present.
    if (r2_within  != null) stats.push(['R-sq: within',  r2_within.toFixed(4)]);
    if (r2_between != null) stats.push(['R-sq: between', r2_between.toFixed(4)]);
    if (r2_overall != null) stats.push(['R-sq: overall', r2_overall.toFixed(4)]);
    if (chi2 != null) {
      // RE case: Wald chi2 test.
      const dfStr = df_chi2 != null ? `(${df_chi2})` : '';
      stats.push([`Wald chi2${dfStr}`, fmt2f(chi2)]);
      if (p_chi2 != null) stats.push(['Prob > chi2', fmtP4h(p_chi2)]);
    } else if (F != null) {
      // FE case (xtreg,fe / areg / single-FE feols): F test, not Wald chi2.
      const dfNum = df_b != null ? df_b : df_m;
      const dfStr = (dfNum != null && df_r != null) ? `(${dfNum}, ${df_r})` : '';
      stats.push([`F${dfStr}`, fmt2f(F)]);
      if (p_F != null) stats.push(['Prob > F', fmtP4h(p_F)]);
    }
    return stats;
  }

  // ── logit/probit path: has ll or r2_pseudo ───────────────────────────────
  if (ll != null || r2_pseudo != null) {
    if (chi2 != null) {
      const dfStr = df_chi2 != null ? `(${df_chi2})` : '';
      stats.push([`LR chi2${dfStr}`, fmt2f(chi2)]);
    }
    if (p_chi2 != null) stats.push(['Prob > chi2', fmtP4h(p_chi2)]);
    if (ll     != null) stats.push(['Log likelihood', fmtG(ll) ?? '?']);
    if (r2_pseudo != null) stats.push(['Pseudo R2', r2_pseudo.toFixed(4)]);
    return stats;
  }

  // ── OLS family path: has F or adj_r2 or rmse ────────────────────────────
  if (F != null || adj_r2 != null || rmse != null) {
    if (F != null) {
      // a weighted xtreg, fe block (no between/overall R-squared) lands here
      const dfNum = df_b != null ? df_b : df_m;
      const dfStr = (dfNum != null && df_r != null) ? `(${dfNum}, ${df_r})` : '';
      stats.push([`F${dfStr}`, fmt2f(F)]);
    }
    if (p_F     != null) stats.push(['Prob > F', fmtP4h(p_F)]);
    if (r2      != null) stats.push(['R-squared', r2.toFixed(4)]);
    if (adj_r2  != null) stats.push(['Adj R-squared', adj_r2.toFixed(4)]);
    if (rmse    != null) stats.push(['Root MSE', fmtG(rmse) ?? '?']);
    return stats;
  }

  // ── Legacy / fallback path ───────────────────────────────────────────────
  if (r2_within != null) {
    stats.push([hasAbsorbed ? 'Within R-sq.' : 'R-squared', r2_within.toFixed(4)]);
  } else if (r2 != null) {
    stats.push(['R-squared', r2.toFixed(4)]);
  }
  if (sigma2 != null && sigma2 > 0) {
    const rootMse = Math.sqrt(sigma2);
    stats.push(['Root MSE', fmtG(rootMse) ?? '?']);
  }
  return stats;
}

function formatStatLines(title, stats, tableWidth) {
  if (stats.length === 0) return [title];

  const maxLabelW = Math.max(...stats.map(([k]) => k.length));
  const maxValW   = Math.max(...stats.map(([, v]) => String(v).length));
  const statLineW = maxLabelW + 3 + maxValW + 2; // "label   =  value"

  const lines = [];
  for (let i = 0; i < Math.max(1, stats.length); i++) {
    const left  = i === 0 ? title : '';
    const right = i < stats.length
      ? padR(stats[i][0], maxLabelW) + '   = ' + padL(String(stats[i][1]), maxValW + 1)
      : '';
    const gap = Math.max(2, tableWidth - left.length - right.length);
    lines.push(left + ' '.repeat(gap) + right);
  }
  return lines;
}

function buildFooter({ vcov_type, n_clusters, absKeys, absorbed,
                        first_stage_F, kp_wald_F, hansen_j, hansen_j_p }) {
  const lines = [];

  if (vcov_type) {
    const lc = vcov_type.toLowerCase();
    if (lc.startsWith('cluster(')) {
      const clusterVar = vcov_type.slice(8, -1);
      const nc = n_clusters ?? '?';
      lines.push(`(Std. err. adjusted for ${nc} clusters in ${clusterVar})`);
    } else if (lc === 'hc1' || lc === 'robust') {
      lines.push('(Std. err. adjusted for heteroskedasticity)');
    }
  }

  if (absKeys && absKeys.length > 0 && absorbed) {
    const feStr = Object.entries(absorbed)
      .map(([k, v]) => `${k} (${fmtObs(v)} levels)`)
      .join(', ');
    lines.push(`Absorbed: ${feStr}`);
  }

  if (first_stage_F) {
    for (const [endog, fval] of Object.entries(first_stage_F)) {
      lines.push(`First-stage F (${endog}): ${fmtG(fval) ?? '?'}`);
    }
  }
  if (kp_wald_F != null) {
    lines.push(`Kleibergen-Paap Wald F: ${fmtG(kp_wald_F) ?? '?'}`);
  }
  if (hansen_j != null) {
    const pStr = hansen_j_p != null ? ` (p = ${fmtP(hansen_j_p)})` : '';
    lines.push(`Hansen J: ${fmtG(hansen_j) ?? '?'}${pStr}`);
  }

  return lines;
}

// ─── Summary stats ────────────────────────────────────────────────────────────

export function formatSummarize(block) {
  const { stats } = block;
  if (!stats) return '(no summary)';

  const vars    = Object.keys(stats);
  // Stata BE 19.5 summarize: stub column ` |`-separated, then Obs/Mean/Std. dev./Min/Max.
  const stubHdr = 'Variable';
  const dataHdrs = ['Obs', 'Mean', 'Std. dev.', 'Min', 'Max'];

  // Pre-format the 5 data columns so widths can be computed dynamically.
  const dataRows = vars.map(v => {
    const s = stats[v];
    return [
      fmtObs(s.N),
      fmtG(s.mean) ?? '',
      fmtG(s.sd)   ?? '',
      fmtG(s.min)  ?? '',
      fmtG(s.max)  ?? '',
    ];
  });

  // Stub width = max(var-name length, "Variable"); data widths = max(header, values).
  const stubW = Math.max(stubHdr.length, ...vars.map(v => v.length));
  const dataW = dataHdrs.map((h, i) =>
    Math.max(h.length, ...dataRows.map(r => r[i].length))
  );

  // Header: right-aligned stub, ` │`, then each data header right-aligned (3-space sep).
  const hdr  = padL(stubHdr, stubW) + ' │' +
    dataHdrs.map((h, i) => padL(h, dataW[i] + 3)).join('');
  // Rule: stubW+1 dashes (align ┼ with the │ after the 1-space gap) + ┼ + data region.
  const dataRegionW = dataW.reduce((a, w) => a + w + 3, 0);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(dataRegionW);

  const lines = [hdr, rule];
  // BUG-333: `summarize, separator(#)` draws a rule every # variables. The key is
  // present only when the user asked for it, so the default rendering is
  // untouched; Stata's default is 5 and 0 means "no rules at all".
  const sep = (block.separator === undefined || block.separator === null)
    ? 5 : Number(block.separator);
  for (let r = 0; r < vars.length; r++) {
    if (sep > 0 && r > 0 && r % sep === 0) lines.push(rule);
    const row = padL(vars[r], stubW) + ' │' +
      dataRows[r].map((v, i) => padL(v, dataW[i] + 3)).join('');
    lines.push(row);
  }
  return lines.join('\n');
}

// ─── Tabulate ────────────────────────────────────────────────────────────────

/** Format a one-way frequency table block (vars.length === 1). */
function formatTabulateOneway(vars, table, chi2) {
  const lines = [];
  const [v] = vars;
  const values = table.values;
  const total = table.margins?.total ?? Object.values(values).reduce((a, b) => a + b, 0);
  const keys = Object.keys(values);
  const keyW = Math.max(v.length, ...keys.map(k => k.length));
  const numW = Math.max(5, String(total).length);

  lines.push(padR(v, keyW) + ' │ ' + padL('Freq.', numW) + '   Percent   Cum.');
  lines.push('─'.repeat(keyW) + '─┼─' + '─'.repeat(numW + 22));

  let cum = 0;
  for (const k of keys) {
    const n = values[k];
    const pct = total > 0 ? (n / total * 100) : 0;
    cum += pct;
    lines.push(
      padL(k, keyW) + ' │ ' +
      padL(String(n), numW) + '   ' +
      padL(pct.toFixed(2), 7) + '   ' +
      padL(cum.toFixed(2), 7)
    );
  }
  lines.push('─'.repeat(keyW) + '─┼─' + '─'.repeat(numW + 22));
  lines.push(padL('Total', keyW) + ' │ ' + padL(String(total), numW) + '   100.00');

  if (chi2) {
    lines.push(`χ²(${chi2.df}) = ${fmtG(chi2.statistic) ?? '?'}   Pr = ${fmtP(chi2.p_value)}`);
  }
  return lines.join('\n');
}

/**
 * BUG-1514: the statistic lines Stata prints under a two-way table, in its layout
 * (StataBE 19.5, `tabulate c d, chi2 lrchi2 V gamma taub`): a blank line, then the
 * label right-aligned in 25 columns, ` = `, the statistic as %8.4f, and `   Pr = `
 * or `  ASE = ` with %5.3f; Fisher's exact p (2 x 2 tables) ends in the Pr column.
 *           Pearson chi2(6) =   1.4977   Pr = 0.960
 *  Likelihood-ratio chi2(6) =   1.5348   Pr = 0.957
 *                Cramér's V =   0.1117
 *                     gamma =  -0.0311  ASE = 0.175
 *           Kendall's tau-b =  -0.0208  ASE = 0.117
 *            Fisher's exact =                 0.800
 *    1-sided Fisher's exact =                 0.486
 */
function formatTabulateStats(block) {
  const lab = s => s.padStart(25);
  const f4 = x => Number(x).toFixed(4).padStart(8);
  const f3 = x => Number(x).toFixed(3);
  const lines = [];
  const { chi2, lrchi2, cramers_v: v, gamma, taub } = block;
  if (chi2) lines.push(`${lab(`Pearson chi2(${chi2.df})`)} = ${f4(chi2.statistic)}   Pr = ${f3(chi2.p_value)}`);
  if (lrchi2) lines.push(`${lab(`Likelihood-ratio chi2(${lrchi2.df})`)} = ${f4(lrchi2.statistic)}   Pr = ${f3(lrchi2.p_value)}`);
  if (v != null) lines.push(`${lab('Cramér\'s V')} = ${f4(v)}`);
  if (gamma) lines.push(`${lab('gamma')} = ${f4(gamma.value)}  ASE = ${f3(gamma.ase)}`);
  if (taub) lines.push(`${lab('Kendall\'s tau-b')} = ${f4(taub.value)}  ASE = ${f3(taub.ase)}`);
  if (block.exact) {
    lines.push(`${lab('Fisher\'s exact')} = ${' '.repeat(16)}${f3(block.exact.p)}`);
    if (block.exact.p1 != null) lines.push(`${lab('1-sided Fisher\'s exact')} = ${' '.repeat(16)}${f3(block.exact.p1)}`);
  }
  return lines.length ? ['', ...lines] : [];
}

/** Format a two-way contingency table block (vars.length === 2). */
function formatTabulateTwoway(vars, table, block) {
  const lines = [];
  const [rowVar, colVar] = vars;
  const { rows, cols, values, margins } = table;
  const grandTotal = margins?.total ?? 0;

  // Compute column widths: each col cell is max(colLabel.length, max count width, total width)
  const colLabelW = cols.map(c => String(c).length);
  const colCountW = cols.map((c, ci) => {
    const maxCount = Math.max(
      ...rows.map(r => String(values[r]?.[c] ?? 0).length),
      String(margins?.col?.[c] ?? 0).length,
      colLabelW[ci]
    );
    return Math.max(5, maxCount);
  });
  const totalColW = Math.max(5, String(grandTotal).length, 'Total'.length);

  // Row stub width: max of rowVar name and row labels
  const stubW = Math.max(
    rowVar.length,
    ...rows.map(r => String(r).length),
    'Total'.length
  );

  // Banner line: colVar name centered above the column block
  const colBlockW = colCountW.reduce((a, w) => a + w, 0) + (colCountW.length - 1) * 3;
  const totalBlockW = colBlockW + 3 + totalColW;
  const bannerPad = Math.max(0, Math.floor((totalBlockW - colVar.length) / 2));
  lines.push(' '.repeat(stubW + 3) + ' '.repeat(bannerPad) + colVar);

  // Header row: stub col | col1   col2   ...   Total
  const hdrCells = cols.map((c, ci) => padL(String(c), colCountW[ci]));
  hdrCells.push(padL('Total', totalColW));
  lines.push(padR(rowVar, stubW) + ' │ ' + hdrCells.join('   '));

  // Separator rule
  const ruleRight = colCountW.map(w => '─'.repeat(w)).join('───') + '───' + '─'.repeat(totalColW);
  lines.push('─'.repeat(stubW) + '─┼─' + ruleRight);

  // Data rows
  for (const r of rows) {
    const rowTotal = margins?.row?.[r] ?? 0;
    const cells = cols.map((c, ci) => padL(String(values[r]?.[c] ?? 0), colCountW[ci]));
    cells.push(padL(String(rowTotal), totalColW));
    lines.push(padL(String(r), stubW) + ' │ ' + cells.join('   '));
  }

  // Total row
  lines.push('─'.repeat(stubW) + '─┼─' + ruleRight);
  const totCells = cols.map((c, ci) => padL(String(margins?.col?.[c] ?? 0), colCountW[ci]));
  totCells.push(padL(String(grandTotal), totalColW));
  lines.push(padL('Total', stubW) + ' │ ' + totCells.join('   '));

  lines.push(...formatTabulateStats(block));
  return lines.join('\n');
}

// WS1 #5: Stata's `tabulate, sum()` mean/sd cells use ~8 significant digits
// (%10.0g) — fmtG's 7-sig-fig cap drops the 8th digit. Local %.8g formatter.
function fmtG8(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return '';
  if (x === 0) return '0';
  let s = Number(x).toPrecision(8);
  if (s.includes('.')) s = s.replace(/0+$/, '').replace(/\.$/, '');
  return s.replace(/^(-?)0\./, '$1.');   // Stata: drop leading zero (.5 not 0.5)
}

/** Format a tabulate VAR, sum(SUMVAR) block (mode:"summarize"). */
function formatTabulateSummarize(block) {
  const { var: v, sumvar: s, rows, overall } = block;
  const lines = [];

  const colHdrs = [v ?? 'Category', 'Mean', 'Std. dev.', 'Freq.'];
  const fmtRows = (rows ?? []).map(r => [
    String(r.level),
    fmtG8(r.mean),
    fmtG8(r.sd),
    String(r.N),
  ]);
  if (overall) {
    fmtRows.push(['Total', fmtG8(overall.mean), fmtG8(overall.sd), String(overall.N)]);
  }

  const widths = colHdrs.map((h, i) =>
    Math.max(h.length, ...fmtRows.map(r => r[i].length))
  );

  const hdr  = colHdrs.map((h, i) => (i === 0 ? padR(h, widths[i]) : padL(h, widths[i]))).join('   ');
  const rule = widths.map(w => '─'.repeat(w)).join('───');

  lines.push(hdr, rule);
  for (let i = 0; i < fmtRows.length; i++) {
    const row = fmtRows[i];
    // Insert rule before the Total row
    if (overall && i === fmtRows.length - 1) lines.push(rule);
    lines.push(row.map((v, ci) => (ci === 0 ? padR(v, widths[ci]) : padL(v, widths[ci]))).join('   '));
  }
  lines.push(rule);
  return lines.join('\n');
}

export function formatTabulate(block) {
  // A1: summarize mode — tabulate VAR, sum(SUMVAR)
  if (block.mode === 'summarize') {
    return formatTabulateSummarize(block);
  }

  // A2: multi-table modes (tab1 / tab2) — delegate to per-subtable formatters
  if (block.mode === 'oneway_multi' || block.mode === 'pairwise') {
    return (block.tables ?? []).map(t => formatTabulate(t)).join('\n\n');
  }

  const { vars, table, chi2 } = block;
  if (!table) return '(no tabulation)';

  if (vars.length === 1) {
    return formatTabulateOneway(vars, table, chi2);
  } else {
    // A5: two-way contingency table (was a literal-string stub)
    return formatTabulateTwoway(vars, table, block);
  }
}

function renderTabulate(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatTabulate(block);
  div.appendChild(pre);
  return div;
}

// ─── Describe ────────────────────────────────────────────────────────────────

export function formatDescribe(block) {
  const { variables } = block;
  if (!variables || variables.length === 0) return '(no variables)';

  // BUG-371: `codebook, compact`. The block is the same one plain codebook emits
  // (this arm has always carried obs/unique/mean/min/max/label per variable) —
  // `compact` only chooses which of those get printed, which is why it is a flag
  // on the existing block rather than a new type. A consumer that ignores the
  // flag still gets the describe layout, so this is additive.
  if (block.compact) {
    const num = v => (v === undefined || v === null || Number.isNaN(v)) ? '.' : fmtG(v);
    const nameW = Math.max(8, ...variables.map(v => v.name.length));
    const obsW = Math.max(4, ...variables.map(v => String(v.n_obs ?? '').length));
    const uniqW = Math.max(6, ...variables.map(v => String(v.n_unique ?? '').length));
    const meanW = Math.max(8, ...variables.map(v => num(v.mean).length));
    const minW = Math.max(5, ...variables.map(v => num(v.min).length));
    const maxW = Math.max(5, ...variables.map(v => num(v.max).length));
    const sep = '  ';
    const hdr = [padR('Variable', nameW), padL('Obs', obsW), padL('Unique', uniqW),
                 padL('Mean', meanW), padL('Min', minW), padL('Max', maxW)].join(sep)
                + sep + 'Label';
    const rule = '─'.repeat(hdr.length);
    const rows = variables.map(v => [
      padR(v.name, nameW),
      padL(String(v.n_obs ?? ''), obsW),
      padL(String(v.n_unique ?? ''), uniqW),
      padL(num(v.mean), meanW),
      padL(num(v.min), minW),
      padL(num(v.max), maxW),
    ].join(sep) + sep + (v.label || ''));
    return [hdr, rule, ...rows].join('\n');
  }

  // Stata BE 19.5 describe: two-row header.
  //   row 1:  Variable      Storage   Display    Value
  //   row 2:      name         type    format    label      Variable label
  // PIET fields: name, dtype (Storage type), format (Display format),
  //   value label (PIET tracks none → blank), label (Variable label).
  const nameW  = Math.max(13, ...variables.map(v => v.name.length));   // "Variable"/"name" col
  const typeW  = Math.max(7,  'Storage'.length, ...variables.map(v => v.dtype.length));
  const fmtW   = Math.max(7,  'Display'.length, ...variables.map(v => (v.format || '').length));
  const vlblW  = Math.max(5,  'Value'.length);                          // value-label col (blank data)
  const sep    = '   ';

  // Header row 1: top words right-aligned over their columns.
  const hdr1 = [
    padR('Variable', nameW),
    padL('Storage',  typeW),
    padL('Display',  fmtW),
    padL('Value',    vlblW),
  ].join(sep);
  // Header row 2: sub-words ("name" right-aligned under Variable) + the wide Variable-label header.
  const hdr2 = [
    padL('name',  nameW),
    padL('type',  typeW),
    padL('format', fmtW),
    padL('label', vlblW),
  ].join(sep) + sep + 'Variable label';

  const totalW = nameW + typeW + fmtW + vlblW + sep.length * 3 + sep.length + 'Variable label'.length;
  const rule = '─'.repeat(totalW);

  const rows = variables.map(v => [
    padR(v.name,         nameW),
    padL(v.dtype,        typeW),
    padL(v.format || '', fmtW),
    padL('',             vlblW),   // value label (none tracked)
  ].join(sep) + sep + (v.label || ''));

  return [hdr1, hdr2, rule, ...rows].join('\n');
}

// ─── Estimates Table (esttab) ─────────────────────────────────────────────────

/**
 * Render an EstimatesTableResult block as a multi-column regression table.
 * Mirrors the 1.4.0 two-way tab render pattern: <pre> inside a .block-table,
 * with Copy and Export-CSV buttons.
 */
function renderEstimatesTable(block) {
  const div = document.createElement('div');
  div.className = 'block block-table block-estimates-table';

  // ── Formatted text rendering ──────────────────────────────────────────────
  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatEstimatesTable(block);
  div.appendChild(pre);

  // ── Action bar: Copy + Export CSV ─────────────────────────────────────────
  // A10 (P8.1): + console-action so the save-log strip (a later integration
  // step) covers this bar the same way it covers the T3 generalized one.
  const bar = document.createElement('div');
  bar.className = 'esttab-actions console-action';

  const copyBtn = document.createElement('button');
  copyBtn.textContent = 'Copy table';
  copyBtn.className = 'esttab-btn';
  copyBtn.addEventListener('click', () => {
    const text = pre.textContent;
    navigator.clipboard.writeText(text).catch(() => {
      // Fallback for environments without Clipboard API
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    });
    copyBtn.textContent = 'Copied!';
    setTimeout(() => { copyBtn.textContent = 'Copy table'; }, 1500);
  });
  bar.appendChild(copyBtn);

  const csvBtn = document.createElement('button');
  csvBtn.textContent = 'Export CSV';
  csvBtn.className = 'esttab-btn';
  csvBtn.addEventListener('click', () => {
    const csv = block.csv || '';
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'esttab.csv';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  });
  bar.appendChild(csvBtn);

  div.appendChild(bar);
  return div;
}

/**
 * Format an EstimatesTableResult as a monospace multi-column table.
 * Layout:
 *   col 0  = coef name (left-aligned)
 *   col 1…N = model columns (right-aligned within each)
 * Each coef has two lines: coefficient (with stars) and SE/t in parentheses.
 * Footer: N and R² rows.
 */
function formatEstimatesTable(block) {
  const { names = [], coef_rows = [], footers = [], options = {} } = block;
  const b_fmt = options.b_fmt || '%9.3f';
  const show_se = options.se !== false;
  const nostar = !!options.nostar;

  // ── Format a numeric value with Stata-style %fmt ─────────────────────────
  function fmtVal(v, fmt) {
    if (v === null || v === undefined) return '';
    const m = fmt.match(/%[-]?(?:\d+)?\.(\d+)([fg])/);
    if (m) {
      const prec = parseInt(m[1], 10);
      const kind = m[2];
      if (kind === 'f') return v.toFixed(prec);
      if (kind === 'g') return v.toPrecision(prec).replace(/\.?0+$/, '');
    }
    return v.toFixed(3);
  }

  // ── Stars ─────────────────────────────────────────────────────────────────
  function stars(cell) {
    if (nostar || !cell) return '';
    return cell.stars || '';
  }

  // ── Pre-format all cells ──────────────────────────────────────────────────
  // For each coef_row, produce {b_str, paren_str} per model.
  const formatted = coef_rows.map(cr => cr.cells.map(cell => {
    if (!cell) return { b: '', p: '' };
    if (cell.omitted) return { b: '(omitted)', p: '' };
    const bVal = cell.b;
    const parenVal = show_se ? cell.se : cell.t;
    const bStr = bVal !== null ? fmtVal(bVal, b_fmt) + stars(cell) : '';
    const pStr = parenVal !== null ? `(${fmtVal(parenVal, b_fmt)})` : '';
    return { b: bStr, p: pStr };
  }));

  // ── Column widths ─────────────────────────────────────────────────────────
  const nameW = Math.max(8, ...coef_rows.map(cr => cr.name.length));
  const colWs = names.map((name, mi) => {
    let w = name.length;
    for (const row of formatted) {
      w = Math.max(w, row[mi].b.length, row[mi].p.length);
    }
    for (const f of footers) {
      const v = f.values[mi] || '';
      w = Math.max(w, v.length, f.label.length);
    }
    return Math.max(w, 9);
  });

  const sep = '─';

  // ── Header row ────────────────────────────────────────────────────────────
  const hdrCells = names.map((name, i) => padL(name, colWs[i]));
  const hdr = padR('', nameW) + '  ' + hdrCells.join('  ');

  // ── Rules ─────────────────────────────────────────────────────────────────
  const ruleTop = sep.repeat(nameW) + '─┬─' + colWs.map(w => sep.repeat(w)).join('─┬─');
  const ruleBot = sep.repeat(nameW) + '─┴─' + colWs.map(w => sep.repeat(w)).join('─┴─');

  // ── Data rows ─────────────────────────────────────────────────────────────
  const dataLines = [];
  coef_rows.forEach((cr, ri) => {
    // coefficient line
    const bCells = formatted[ri].map((c, mi) => padL(c.b, colWs[mi]));
    dataLines.push(padR(cr.name, nameW) + ' │ ' + bCells.join(' │ '));
    // parenthesised line
    const pCells = formatted[ri].map((c, mi) => padL(c.p, colWs[mi]));
    dataLines.push(padR('', nameW) + ' │ ' + pCells.join(' │ '));
  });

  // ── Footer rows ───────────────────────────────────────────────────────────
  const footLines = footers.map(f => {
    const vals = names.map((_, mi) => padL(f.values[mi] || '', colWs[mi]));
    return padR(f.label, nameW) + '  ' + vals.join('  ');
  });

  const lines = [
    hdr,
    ruleTop,
    ...dataLines,
    ruleBot,
    ...footLines,
  ];
  return lines.join('\n');
}
