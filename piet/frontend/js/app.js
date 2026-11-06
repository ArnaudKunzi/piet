// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// app.js — central state, event orchestration, keyboard, boot.
// Single source of truth; panels/console/grid subscribe via CustomEvents.

import { HTTPClient }  from './api.js';
import { initConsole, appendBlock, appendBlocks, clearConsole, endDisplayLine, getGraphEntries, getGraphById, getResidentGraph, getGraphSpilledCount, getGraphDroppedCount, setMemoryBudgetMb, clearGraphGallery } from './console.js';
import { renderGraph } from './graph.js';
import { initPanels, updateVariables, appendHistory, loadHistory, getVarlist } from './panels.js';
import { VirtualizedGrid, setGridMemoryBudgetMb } from './grid.js';
import { initPanes } from './panes.js';
import { initFind, openFind, closeFind, isFindOpen } from './find.js';
import { initGutter, setGutterErrorLine } from './gutter.js';

// ─── App state ────────────────────────────────────────────────────────────────

const state = {
  currentFile:     null,   // string | null
  varlist:         [],     // VarInfo[]
  nRows:           0,
  nCols:           0,
  busy:            false,
  kernelReady:     false,
  commandHistory:  [],     // { cmd, success }[]
  historyIndex:    -1,
};

let client    = null;
let grid      = null;
let _tabCycle = { prefix: '', stemBefore: '', matches: [], idx: -1 };

// 2.2.0 item C: split-view layout. When ON, #pane-console is a flex row of
// surface columns (Results is always the base column; Do-file Editor /
// Graphics dock/undock as extra columns). Persisted in localStorage
// 'piet-split' = {on:bool, docked:[overlayId…]}; Reset layout clears it.
let splitOn = false;
const SPLIT_KEY = 'piet-split';
const DOCKABLE  = ['doedit-overlay', 'graphics-overlay'];  // Results is the always-present base column

// B4 (P8-S7): split-column ratio (percentage of #pane-console given to the
// Results column; the rest goes to whichever docked surface — or, absent
// one, F10's placeholder — follows it). Same "JS mirror of a CSS custom
// property" idiom as panes.js's --col-vars/--col-history; persisted as a
// `ratio` field on the SAME piet-split blob rather than a new key.
let splitRatio = 50;
const SPLIT_RATIO_MIN = 20;
const SPLIT_RATIO_MAX = 80;

// BUG-113: doedit↔graphics weighted grow, for the two-docked-columns case
// only (splitRatio above still owns Results↔second-column). Same JS-mirrors-
// CSS-custom-property idiom as splitRatio; persisted as `ratio2` on the SAME
// piet-split blob. 1:1 = equal columns (the CSS fallback default too).
let splitW1 = 1;   // #doedit-overlay.docked weight
let splitW2 = 1;   // #graphics-overlay.docked weight
const SPLIT_W_MIN = 0.25;
const SPLIT_W_MAX = 4;

// B10 (P8-S7): Graphics gallery full-view index. -1 = gallery grid (the
// default); >=0 = single-graph full view, indexing getGraphEntries().
let _graphicsFullIndex = -1;

// B3 (P8-S6): Recent files — last RECENT_MAX opened data/do files, persisted
// in localStorage (same "JSON blob under one key" idiom as SPLIT_KEY above).
// Entries: { path, kind: 'data'|'do', ts }, newest first, deduped by path.
const RECENT_KEY = 'piet-recent-files';
const RECENT_MAX = 8;

// N5 (P8.1-W2): split-view layout VARIANT — 'columns' (default, current
// side-by-side geometry) vs 'stack' (both docked surfaces stacked vertically
// inside #docked-stack, Results staying a full-height left column). Rides
// the SAME persistSplit()/restoreSplit() blob as splitOn/splitRatio above
// (field `layout`), plus `order` (the DOCKABLE ids in their current DOM
// order — click-swap, N5.2). Only meaningful (and only surfaced in the
// toolbar) once BOTH DOCKABLE surfaces are docked — see syncSplitLayout().
let _splitVariant = 'columns';   // 'columns' | 'stack'
// N5.2: click-swap arm/disarm — the id of the docked overlay whose
// .doedit-bar title was just clicked (armed), or null. A second click on the
// OTHER docked pane's title performs the swap; Esc/clicking elsewhere/re-
// clicking the SAME title disarms without swapping.
let _swapArmedId = null;

// N2 (P8.1-W2): settings cache — populated at boot (hydrateUiState()) and
// refreshed by populateSettings()/a successful applySettings(), so
// composeDefaultPath() (N1) can read the configured export folder
// SYNCHRONOUSLY from a save-button click handler without an extra bridge
// round trip on every export. Native-transport only; mock/no-bridge sessions
// never populate this (feature-detected at each read site instead).
let _settingsCache = null;

// ─── Init ─────────────────────────────────────────────────────────────────────

export async function init() {
  // Transport: the native bridge when the shell installed it, mock fixtures
  // otherwise (a plain browser tab / the DOM test harness). The 1.x
  // ?kernel=/PIET_KERNEL_URL fetch leg is gone with the Python backend
  // (I-8 cutover sweep, 2026-07-09).
  const hasNativeBridge = typeof window !== 'undefined' && !!window.pietNative;
  const useMock = !hasNativeBridge;

  client = new HTTPClient(useMock);
  initPietEventsBridge();

  // N2 (P8.1-W2): pull the native ui_state blob into localStorage BEFORE any
  // module below (initPanes()/initGutter() a few lines down, restoreSplit()
  // later in this function) reads its own localStorage key — see
  // hydrateUiState()'s doc comment for the write-through/fallback contract.
  // No-op (instant) in mock/browser sessions (no native bridge to hydrate
  // FROM) so this await never meaningfully delays boot there.
  await hydrateUiState();

  // Wire up DOM
  const varListEl  = document.getElementById('var-list-wrap');
  const propsEl    = document.getElementById('properties');
  const histListEl = document.getElementById('hist-list');
  const consoleEl  = document.getElementById('console-body');
  const emptyEl    = document.getElementById('console-empty');
  const jumpPill   = document.getElementById('jump-pill');
  const gridEl     = document.getElementById('browser-overlay');

  initPanes();
  initPanels(varListEl, propsEl, histListEl);
  // T4 (P8.1): do-file editor line-number gutter (gutter.js — self-injecting;
  // honors the 'piet-editor-gutter' localStorage toggle, absent = ON).
  initGutter();
  initConsole(consoleEl, emptyEl, jumpPill);
  // B11/B12: ⌘F find bar — Results find-only, do-file editor find+replace
  // (piet/frontend/js/find.js). Keyboard wiring (Cmd/Ctrl+F, Escape) below.
  initFind();

  if (gridEl) {
    grid = new VirtualizedGrid(
      gridEl,
      (offset, limit, vars) => client.browse(offset, limit, vars),
      // Cell-commit callback (edit mode): write value at (row, varname) to the kernel.
      async (varname, row, value) => {
        // A1-02: don't race a concurrent command/mutation already in flight —
        // decline honestly; grid.js treats a {type:'error'} return as "revert
        // this edit" (its existing error path), so no grid.js change needed.
        if (state.busy) {
          return { type: 'error', code: 'busy', message: 'Kernel is busy — try again in a moment.' };
        }
        const res = await client.cell(varname, row, value);
        if (res && res.type !== 'error' && res.varlist_after) {
          state.varlist = res.varlist_after;
          state.nCols   = res.varlist_after.length;
          updateVariables(res.varlist_after);
          if (res.n_rows_after != null) state.nRows = res.n_rows_after;
          updateStatus();
        }
        return res;
      },
    );
    gridEl.hidden = true;
  }

  // Command input
  const inputEl = document.getElementById('cmd-input');
  if (inputEl) {
    inputEl.addEventListener('keydown', onInputKeydown);
    inputEl.addEventListener('paste', onInputPaste);
  }

  // Run button — same path as Enter (Stata-like explicit submit)
  const runBtn = document.getElementById('btn-run');
  if (runBtn) {
    runBtn.addEventListener('click', () => {
      submitCommand();
      document.getElementById('cmd-input')?.focus();
    });
  }

  // S6 (OVERHEAD-ZERO, drag-and-drop file open): preventDefault() on BOTH
  // dragover AND drop — WKWebView's default action for an un-prevented drop
  // is to navigate the whole window to the dropped file (dragover alone is
  // NOT enough to stop it; see shell.cpp's installFileDropHandler() doc
  // comment for the matching native half). Deliberately does nothing else:
  // the real filesystem path never reaches JS (WKWebView only ever exposes
  // a sandboxed/opaque File reference to page script) — it arrives via the
  // 'file_dropped' pietEvents push handled in initPietEventsBridge() below,
  // pushed by the native drag-destination override in shell.cpp.
  document.addEventListener('dragover', (e) => e.preventDefault());
  document.addEventListener('drop', (e) => e.preventDefault());

  // Jump-to-latest pill
  if (jumpPill) {
    jumpPill.addEventListener('click', () => {
      // #console-body never overflows (min-height:100%) — the real scroll
      // container is the parent #console-wrap (console.js's _wrapEl convention).
      const wrapEl = consoleEl.parentElement;
      wrapEl.scrollTop = wrapEl.scrollHeight;
    });
    jumpPill.hidden = true;
  }

  // Clear button
  const clearBtn = document.getElementById('btn-clear');
  if (clearBtn) clearBtn.addEventListener('click', onClear);

  // B2 (P8-S6): Save Results — serialize the results log to a .log download.
  const saveResultsBtn = document.getElementById('btn-save-results');
  if (saveResultsBtn) saveResultsBtn.addEventListener('click', onSaveResults);

  // Mode toggle (De Stijl ↔ Boring)
  const modeToggle = document.getElementById('mode-toggle');
  if (modeToggle) modeToggle.addEventListener('click', toggleMode);

  // Theme toggle (light ↔ dark)
  const themeToggle = document.getElementById('theme-toggle');
  if (themeToggle) themeToggle.addEventListener('click', toggleTheme);

  // BUG-114: one early resync so both labels are correct from the first
  // paint onward — index.html's boot script used to do this manually (now
  // relocated here; see its own comment) and init() runs from that SAME
  // boot script, so the resync guarantee is preserved.
  syncThemeButtons();

  // D5: live "Follow system appearance" — re-syncs <html data-theme> whenever
  // the OS setting changes, but only while the Settings toggle is on
  // (checked fresh from localStorage each firing, not captured at boot).
  // Registered once; index.html's pre-paint script already applied the
  // correct initial state before this module ran.
  if (window.matchMedia) {
    const systemThemeQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const onSystemThemeChange = () => {
      if (localStorage.getItem('piet-theme-follow-system') === '1') applySystemTheme();
    };
    if (systemThemeQuery.addEventListener) systemThemeQuery.addEventListener('change', onSystemThemeChange);
    else if (systemThemeQuery.addListener) systemThemeQuery.addListener(onSystemThemeChange); // older WebKit
  }

  // File open
  const openLink = document.querySelector('[data-action="open"]');
  if (openLink) openLink.addEventListener('click', e => { e.preventDefault(); onOpenFile(); });

  // Run .do
  const doLink = document.querySelector('[data-action="do"]');
  if (doLink) doLink.addEventListener('click', e => { e.preventDefault(); onRunDo(); });

  // File ▾ > Open do-file… (BUG-046): load a .do file into the editor
  // WITHOUT running it — onRunDo's pre-BUG-045 behavior, relocated here.
  const doOpenLink = document.querySelector('[data-action="do-open"]');
  if (doOpenLink) doOpenLink.addEventListener('click', e => { e.preventDefault(); onOpenDoFile(); });

  // B3 (P8-S6): File ▾ > Recent — populate from localStorage at boot (each
  // open thereafter re-renders itself via trackRecentFile()).
  renderRecentFiles();

  // Do-file editor (in-app) — 2.2.0 item C: the view tab toggles the docked
  // column in split-view, opens the exclusive overlay in single-view.
  const doEditLink = document.querySelector('[data-action="doedit"]');
  if (doEditLink) doEditLink.addEventListener('click', e => { e.preventDefault(); onViewTabClick('doedit-overlay', openDoEditor, closeDoEditor); });
  // UX-047: #doedit-close (the "← Back to Results" button) was removed from
  // index.html — Escape + the Results view tab generalize it (III-6). Its
  // wiring is gone too, not just left as a guarded no-op.
  const doEditClear = document.getElementById('doedit-clear');
  if (doEditClear) doEditClear.addEventListener('click', () => {
    const ta = document.getElementById('doedit-text');
    if (ta) { ta.value = ''; ta.focus(); }
    markDoEditorClean();   // dirty dot (P8.1-T5): Clear is itself a no-unsaved-changes state
  });
  const doEditRun = document.getElementById('doedit-run');
  if (doEditRun) doEditRun.addEventListener('click', runDoEditor);
  // A6 (P8.1-T2): explicit Run-selection button — same runDoEditorSelection()
  // path as the ⇧⌘D keybinding (doedit-text's keydown handler above).
  const doEditRunSelection = document.getElementById('doedit-run-selection');
  if (doEditRunSelection) doEditRunSelection.addEventListener('click', runDoEditorSelection);
  const doEditText = document.getElementById('doedit-text');
  if (doEditText) {
    doEditText.addEventListener('keydown', e => {
      // 2.2.0 item B: Execute selection (do) — Stata's Do-button keybinding
      // (⇧⌘D on macOS / Ctrl+Shift+D elsewhere; verified against [GSM] 13's
      // "If text is highlighted, the button becomes Execute selection (do)").
      // Runs the highlighted range alone — or the whole buffer if nothing is
      // highlighted (Stata parity) — through the SAME do_string path Run .do
      // uses. Checked before the whole-file Cmd/Ctrl+Enter binding (distinct key).
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && (e.key === 'd' || e.key === 'D')) {
        e.preventDefault();
        runDoEditorSelection();
        return;
      }
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); runDoEditor(); return; }
      // Escape is handled by the single top-level overlay handler below (A2-05).
      // Hygiene (design 8): Tab inserts 4 spaces instead of moving focus off
      // the textarea. setRangeText repositions the caret after the inserted
      // text ('end' mode); it does not participate in the native undo stack
      // (a known, accepted trade-off — execCommand('insertText') would, but
      // is deprecated and inconsistently supported across engines).
      if (e.key === 'Tab') {
        e.preventDefault();
        const ta = e.target;
        ta.setRangeText('    ', ta.selectionStart, ta.selectionEnd, 'end');
        markDoEditorDirty();   // setRangeText is programmatic — no native 'input' event
      }
    });
    // A11 (P8.1-T5): same smart-quote normalization as #cmd-input's paste
    // handler — the do-editor is exactly where a pasted `use "curly file"`
    // do-block would otherwise carry the un-normalized quotes straight in.
    doEditText.addEventListener('paste', e => {
      const raw = e.clipboardData?.getData('text');
      if (!raw) return;
      const normalized = normalizeSmartQuotes(raw);
      if (normalized === raw) return;   // nothing to convert: native insert, untouched
      e.preventDefault();
      const start = doEditText.selectionStart ?? doEditText.value.length;
      const end   = doEditText.selectionEnd ?? doEditText.value.length;
      doEditText.setRangeText(normalized, start, end, 'end');
      markDoEditorDirty();   // setRangeText is programmatic — no native 'input' event
      flashStatus('Smart quotes converted to straight quotes');
    });
    // Dirty dot (P8.1-T5): any normal (native) edit — typing, native paste
    // insert, cut, IME composition, … — fires 'input'; the two programmatic
    // setRangeText() paths above (Tab, smart-quote paste) mark dirty inline
    // since setRangeText does NOT dispatch 'input' itself.
    doEditText.addEventListener('input', markDoEditorDirty);
  }

  // Paste auto-run preference (opt-in follow-up to design decision 4).
  // Checkbox in the do-editor header reflects + writes localStorage
  // 'piet-paste-autorun' ('1'/'0'; absent = OFF) — same persistence idiom as
  // piet-mode/piet-theme. The behavior switch lives in handleMultilineBlock().
  const pasteToggle = document.getElementById('paste-autorun-toggle');
  if (pasteToggle) {
    pasteToggle.checked = localStorage.getItem('piet-paste-autorun') === '1';
    // 2.2.0 item D: mirror to the Settings panel checkbox via setPasteAutorun.
    pasteToggle.addEventListener('change', () => setPasteAutorun(pasteToggle.checked));
  }

  // B1 (1.16.0): run-progress strip cancel button.
  const rpCancel = document.getElementById('rp-cancel');
  if (rpCancel) rpCancel.addEventListener('click', onCancelRun);

  // D9 (BETA Phase F): request-dialog overlay — Submit posts the answer;
  // Cancel run reuses the same /api/run_cancel route as #rp-cancel above. No
  // Escape/outside-click dismissal is wired here on purpose (see
  // openRequestDialog()'s doc comment) — the overlay itself swallows Escape
  // (below) so the document-level Escape handler (OVERLAYS registry) never
  // sees it, instead of relying on #request-overlay's absence from that
  // registry alone.
  const requestForm = document.getElementById('request-form');
  if (requestForm) requestForm.addEventListener('submit', onRequestSubmit);
  const requestCancelBtn = document.getElementById('request-cancel-run');
  if (requestCancelBtn) requestCancelBtn.addEventListener('click', onRequestCancelRun);
  const requestOverlay = document.getElementById('request-overlay');
  if (requestOverlay) {
    requestOverlay.addEventListener('keydown', e => {
      if (e.key === 'Escape') e.stopPropagation();
    });
  }

  // Graphics gallery overlay (WS4) — 2.2.0 item C: same toggle-in-split rule.
  const graphicsLink = document.querySelector('[data-action="graphics"]');
  if (graphicsLink) graphicsLink.addEventListener('click', e => { e.preventDefault(); onViewTabClick('graphics-overlay', openGraphics, closeGraphics); });
  // UX-047: #graphics-close removed from index.html (III-6) — see doedit's note above.

  // About / Legal overlay (WI-5 acknowledgements + WI-6 disclaimer)
  // BUG-095 (P8.1-T1): the entry link is the exit too — re-click toggles the
  // panel closed. Applied to About as well (same trap class: neither is a
  // view tab, so without this the only exits were the undiscoverable Escape
  // or knowing to click a view tab).
  const aboutLink = document.querySelector('[data-action="about"]');
  if (aboutLink) aboutLink.addEventListener('click', e => {
    e.preventDefault();
    if (_isOverlayOpen('about-overlay')) closeAbout(); else openAbout();
  });

  // 2.2.0 item D: Settings panel entry.
  const settingsLink = document.querySelector('[data-action="settings"]');
  if (settingsLink) settingsLink.addEventListener('click', e => {
    e.preventDefault();
    if (_isOverlayOpen('settings-overlay')) closeSettings(); else openSettings();
  });
  // BUG-095: visible ✕ in each panel header (reintroduced deliberately —
  // UX-047/III-6 removed per-overlay closes on the theory that view tabs
  // generalize them, but Settings/About light no view tab, so the theory
  // never covered these two; User dogfooding 07-12 confirmed the trap).
  document.getElementById('settings-close')?.addEventListener('click', closeSettings);
  document.getElementById('about-close')?.addEventListener('click', closeAbout);
  const settingsApply = document.getElementById('settings-apply');
  if (settingsApply) settingsApply.addEventListener('click', applySettings);
  // WS-B (extcompat): the first-run consent dialog's only control. Deliberately
  // NOT wired into the OVERLAYS registry / Escape sweep — see index.html.
  document.getElementById('consent-continue')?.addEventListener('click', acceptConsent);
  const memSlider = document.getElementById('set-mem-budget');
  const memOut    = document.getElementById('set-mem-value');
  if (memSlider && memOut) memSlider.addEventListener('input', () => { memOut.textContent = `${memSlider.value} MB`; });
  const fsSel = document.getElementById('set-fontsize');
  if (fsSel) fsSel.addEventListener('change', () => applyFontSize(fsSel.value));
  const pasteMirror = document.getElementById('set-paste-autorun');
  if (pasteMirror) pasteMirror.addEventListener('change', () => setPasteAutorun(pasteMirror.checked));
  // B14(b): "Auto-summarize on load" — same localStorage-only display-setting
  // idiom as paste-autorun above (see setAutoSummarize()'s doc comment).
  const autoSummarizeToggle = document.getElementById('set-auto-summarize');
  if (autoSummarizeToggle) autoSummarizeToggle.addEventListener('change', () => setAutoSummarize(autoSummarizeToggle.checked));
  // D5: "Follow system appearance" — same localStorage-only display-setting
  // idiom (see setFollowSystemTheme()'s doc comment above).
  const followSystemToggle = document.getElementById('set-follow-system-theme');
  if (followSystemToggle) followSystemToggle.addEventListener('change', () => setFollowSystemTheme(followSystemToggle.checked));

  // 2.2.0 item C: split-view toggle + one-click Reset layout.
  const splitToggle = document.getElementById('split-toggle');
  if (splitToggle) splitToggle.addEventListener('click', e => { e.preventDefault(); setSplit(!splitOn); persistSplit(); });
  const resetLayoutBtn = document.getElementById('reset-layout');
  if (resetLayoutBtn) resetLayoutBtn.addEventListener('click', e => { e.preventDefault(); resetLayout(); });
  // B4 (P8-S7): split-column ratio drag — reuses panes.js's pointerdown/
  // pointermove/pointerup + requestAnimationFrame-coalesced idiom, adapted to
  // a percentage flex-basis instead of a pixel grid-track width.
  initSplitResizer();
  // BUG-113: second divider — doedit↔graphics, when both are docked columns.
  initSplitResizer2();

  // N5.1: layout-variant toggle (columns ↔ stack) — hidden until both
  // dockable surfaces are docked (syncSplitLayout() owns [hidden]).
  const layoutToggle = document.getElementById('layout-variant-toggle');
  if (layoutToggle) layoutToggle.addEventListener('click', e => {
    e.preventDefault();
    setSplitVariant(_splitVariant === 'stack' ? 'columns' : 'stack');
    persistSplit();
  });

  // N5.2: click-swap — ONLY the title span arms/resolves it (buttons in the
  // same .doedit-bar, e.g. ▶ Run/Clear, must keep their own click behavior
  // untouched — they're siblings of the title, never inside it, so this
  // listener never sees those clicks at all).
  const doeditTitleEl   = document.getElementById('doedit-title');
  const graphicsTitleEl = document.getElementById('graphics-title');
  if (doeditTitleEl)   doeditTitleEl.addEventListener('click',   e => _onDockedTitleClick('doedit-overlay', e));
  if (graphicsTitleEl) graphicsTitleEl.addEventListener('click', e => _onDockedTitleClick('graphics-overlay', e));
  // "Esc or clicking elsewhere disarms" (N5.2) — the elsewhere half. The
  // title handlers above stopPropagation() their own clicks, so this never
  // fires for the arm/swap/self-disarm gesture itself, only genuinely
  // outside clicks. Esc is handled inline in the existing document keydown
  // handler further below (checked before the OVERLAYS sweep).
  document.addEventListener('click', () => { if (_swapArmedId) _disarmSwap(); });

  // N2: panes.js writes --col-vars/--col-history directly onto #app's inline
  // style (RAF-coalesced during a drag, one more write on pointerup) — it is
  // NOT this lane's file, so rather than touch it, observe the attribute it
  // already mutates and mirror into ui_state from here. Debounced by
  // markUiStateDirty() itself, so the RAF-coalesced drag-time writes collapse
  // into one flush same as everywhere else.
  const appEl = document.getElementById('app');
  if (appEl) {
    new MutationObserver(markUiStateDirty)
      .observe(appEl, { attributes: true, attributeFilter: ['style'] });
  }

  // B10 (P8-S7): Graphics gallery full-view prev/next/back.
  const gfPrev = document.getElementById('graphics-full-prev');
  if (gfPrev) gfPrev.addEventListener('click', () => graphicsFullStep(-1));
  const gfNext = document.getElementById('graphics-full-next');
  if (gfNext) gfNext.addEventListener('click', () => graphicsFullStep(1));
  const gfBack = document.getElementById('graphics-full-back');
  if (gfBack) gfBack.addEventListener('click', closeGraphicsFull);
  // Arrow-key nav while the full view is showing — scoped to the graphics
  // overlay (only fires while focus is inside it, e.g. on one of the buttons
  // above) so it can't collide with #cmd-input's own ArrowUp/Down history nav.
  const graphicsOv = document.getElementById('graphics-overlay');
  if (graphicsOv) {
    graphicsOv.addEventListener('keydown', e => {
      const full = document.getElementById('graphics-full');
      if (!full || full.hidden) return;
      if (e.key === 'ArrowLeft')  { e.preventDefault(); graphicsFullStep(-1); }
      if (e.key === 'ArrowRight') { e.preventDefault(); graphicsFullStep(1); }
    });
  }

  // B6 (P8-S7): History row's second verb (panels.js's hover-revealed "Send
  // to Do-file Editor" button) — panels.js has no app.js import (it already
  // dispatches piet:var-edit for the same reason), so this mirrors that
  // CustomEvent idiom rather than introducing a new cross-module call.
  document.addEventListener('piet:hist-send-doedit', e => {
    const sendCmd = e.detail?.cmd;
    if (!sendCmd) return;
    const ta = document.getElementById('doedit-text');
    if (ta) ta.value = ta.value ? `${ta.value}\n${sendCmd}` : sendCmd;
    openDoEditor();
  });

  // T4 (P8.1): error-block click → jump to the failing line in the Do-file
  // Editor (console.js's renderError dispatches this when the block carries
  // line_number — same CustomEvent idiom as piet:hist-send-doedit above).
  document.addEventListener('piet:error-line-click', e => {
    const line = e.detail?.line;
    if (line == null) return;
    openDoEditor();
    selectEditorLine(line);
    setGutterErrorLine(line);
  });

  // UX-047: Results view tab — generalizes "Back to Results" across every
  // overlay. closeAllOverlays() with no exceptId closes all four and calls
  // syncViewTabs() itself at its end (see the registry section below).
  // F10 (P8-S7): in split mode a plain closeAllOverlays() here used to ALSO
  // undock the Do-file Editor / Graphics columns (closeDoEditor/closeGraphics
  // both undock when splitOn — see below) — silently contradicting the
  // still-ON Split toggle (the reported "button reads on, nothing is visibly
  // split" symptom: clicking Results while split flattened it back to
  // single-column). Results is the always-present base column in split mode,
  // so its tab has no exclusive-select meaning there; only the non-dockable,
  // exclusive-cover overlays (Data Browser / About / Settings) should close —
  // the docked columns stay put, matching the "view tabs become additive
  // column toggles" design this file already documents for the other two tabs.
  const resultsTab = document.querySelector('.view-tab[data-view="results"]');
  if (resultsTab) resultsTab.addEventListener('click', e => {
    e.preventDefault();
    if (splitOn) {
      for (const o of OVERLAYS) { if (!DOCKABLE.includes(o.id)) o.close(); }
      syncViewTabs();
    } else {
      closeAllOverlays();
    }
  });

  // UX-047: #browser-overlay is grid.js's own domain (frozen to WS-F1/F2) —
  // its Back button and internal Escape handler both call this.hide()
  // DIRECTLY (grid.js:117,195), bypassing the OVERLAYS registry's close()
  // path entirely. Rather than touch grid.js, observe the `hidden` attribute
  // it toggles: this IS the "grid show/hide adapter" the tab-sync design
  // calls for — one hook that catches every path (registry close(), the
  // direct grid.show()/hide() calls in runCommand(), AND grid's own internal
  // Back/Escape) uniformly, with no grid.js changes.
  const browserOverlayEl = document.getElementById('browser-overlay');
  if (browserOverlayEl) {
    new MutationObserver(syncViewTabs)
      .observe(browserOverlayEl, { attributes: true, attributeFilter: ['hidden'] });
  }

  // File ▾ / Data ▾ dropdowns: click-to-toggle .open class on each
  // .menu-dropdown (A2-04: click-toggle ONLY, app.js is the one owner — see
  // piet.css's comment on .menu-dropdown-content). BUG-046 added a second
  // .menu-dropdown (File ▾) alongside the original Data ▾, so this now
  // iterates every .menu-dropdown instead of assuming there is exactly one
  // (a bare querySelector() here would silently wire up only the first).
  const menuDropdowns = document.querySelectorAll('.menu-dropdown');
  menuDropdowns.forEach(dropdown => {
    const trigger = dropdown.querySelector(':scope > a');
    if (!trigger) return;
    trigger.addEventListener('click', e => {
      e.preventDefault();
      e.stopPropagation();
      // Single-menu-open: opening one closes any other already-open dropdown.
      menuDropdowns.forEach(d => { if (d !== dropdown) d.classList.remove('open'); });
      dropdown.classList.toggle('open');
    });
    // A12: a real item click (Open Data…, sysuse fatalities, browse, …) is
    // INSIDE the dropdown, so the outside-click handler below never sees it
    // and the menu stayed open after a selection. Close it here too — the
    // item's own [data-action]/[data-run] handler (wired elsewhere) still
    // runs; this only ALSO closes .open, deferred so it doesn't race that
    // handler's own work.
    const content = dropdown.querySelector(':scope > .menu-dropdown-content');
    if (content) {
      content.addEventListener('click', () => {
        setTimeout(() => menuDropdowns.forEach(d => d.classList.remove('open')), 0);
      });
    }
  });
  // Close any open dropdown on outside click
  document.addEventListener('click', e => {
    menuDropdowns.forEach(dropdown => {
      if (!dropdown.contains(e.target)) dropdown.classList.remove('open');
    });
  });

  // Menu items that RUN a command immediately (e.g. Data Browser / Data Editor)
  document.querySelectorAll('[data-run]').forEach(el => {
    el.addEventListener('click', e => {
      e.preventDefault();
      runCommand(el.dataset.run);
    });
  });

  // Global keyboard shortcuts
  document.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'l') {
      e.preventDefault();
      document.getElementById('cmd-input')?.focus();
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'End') {
      e.preventDefault();
      const cb = document.getElementById('console-body');
      if (cb) cb.scrollTop = cb.scrollHeight;
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
      e.preventDefault();
      onClear();
    }
    // B11: Cmd/Ctrl+F summons the find bar, scoped to whichever console
    // surface is active (find.js's own detection — see its _activeSurface()).
    // Not a persistent-chrome toggle (R1) and not a palette (W1) — this is
    // the ONLY way it opens (plus an Edit-menu Find item, if one is wired).
    if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
      e.preventDefault();
      openFind();
    }
    // BUG-097 (P8.1-T2): Cmd/Ctrl+A inside an editable field (cmd-input,
    // doedit-text, find inputs, settings inputs, …) stays native select-all —
    // only OUTSIDE one of those does it scope to the Results log instead of
    // the whole app chrome (toolbar/panes text nodes had no sane "select
    // all" meaning before this).
    if ((e.ctrlKey || e.metaKey) && (e.key === 'a' || e.key === 'A')) {
      const ae = document.activeElement;
      const isEditable = !!ae && (ae.tagName === 'INPUT' || ae.tagName === 'TEXTAREA' || ae.isContentEditable);
      if (!isEditable) {
        e.preventDefault();
        const cb = document.getElementById('console-body');
        if (cb) window.getSelection().selectAllChildren(cb);
      }
    }
    // ⌘, / ⌘O (P8.1-T2): standard-idiom Settings + Open Data shortcuts.
    // Skipped while the request-dialog is up — that modal must stay modal
    // (mirrors the Escape-swallowing on #request-overlay above).
    if (!_isOverlayOpen('request-overlay')) {
      if ((e.ctrlKey || e.metaKey) && e.key === ',') {
        e.preventDefault();
        if (_isOverlayOpen('settings-overlay')) closeSettings(); else openSettings();
      }
      if ((e.ctrlKey || e.metaKey) && (e.key === 'o' || e.key === 'O')) {
        e.preventDefault();
        onOpenFile();   // self-guards on state.busy
      }
    }
    // T3 (P8.1, UX-054 안2-lite): ⌥↑/↓ — jump between command groups in the
    // Results log (console.js's .block-fold-head echoes are the group heads).
    // Deliberately its OWN anchor class (.block-nav-anchor), NOT C4's
    // .block-active — that class has a single-owner invariant (console.js's
    // _activeBlockEl tracks the newest ESTIMATION block; navigation stealing
    // it would desync the tracker). Skipped inside editable fields — macOS
    // Option+arrows are text-navigation there.
    if (e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      const ae = document.activeElement;
      const inEditable = !!ae && (ae.tagName === 'INPUT' || ae.tagName === 'TEXTAREA' || ae.isContentEditable);
      if (!inEditable) {
        const heads = Array.from(document.querySelectorAll('#console-body .block-fold-head'));
        if (heads.length) {
          e.preventDefault();
          const cur = document.querySelector('#console-body .block-nav-anchor');
          let idx = cur ? heads.indexOf(cur) : heads.length;   // no anchor yet → step lands on the last head
          idx += (e.key === 'ArrowUp' ? -1 : 1);
          idx = Math.max(0, Math.min(heads.length - 1, idx));
          if (cur) cur.classList.remove('block-nav-anchor');
          heads[idx].classList.add('block-nav-anchor');
          heads[idx].scrollIntoView?.({ block: 'center' });
        }
      }
    }
  });

  // Esc: close whichever console overlay is open (A2-05) — one top-level
  // handler driven by the OVERLAYS registry, replacing the previous ad hoc
  // per-overlay Escape handling (grid-only listener here + doedit-text's own
  // Escape branch). Each close() already restores focus to #cmd-input.
  document.addEventListener('keydown', e => {
    if (e.key !== 'Escape') return;
    // B11: the find bar closes on Esc BEFORE any console overlay (design
    // law) — checked first, ahead of both the menu-dropdown check and the
    // OVERLAYS sweep below. closeFind() restores focus to wherever it was
    // before ⌘F opened the bar.
    if (isFindOpen()) { closeFind(); e.preventDefault(); return; }
    // N5.2: Esc disarms a pending click-swap BEFORE the menu-dropdown check
    // and the OVERLAYS sweep — same "most-local transient state first"
    // ordering as the find-bar check just above.
    if (_swapArmedId) { _disarmSwap(); e.preventDefault(); return; }
    // A12: Esc also closes an open File▾/Data▾ dropdown — menus use their
    // own .open toggle, not the OVERLAYS registry, so they were untouched
    // by the loop below.
    const openMenu = document.querySelector('.menu-dropdown.open');
    if (openMenu) {
      menuDropdowns.forEach(d => d.classList.remove('open'));
      e.preventDefault();
      return;
    }
    for (const o of OVERLAYS) {
      if (o.isOpen()) { o.close(); e.preventDefault(); return; }
    }
  });

  // Variable Editor mutations (dispatched by panels.js)
  document.addEventListener('piet:var-edit', async (e) => {
    // A1-02: don't race a command/do-run/other mutation already in flight.
    if (state.busy) return;
    const { action, ...params } = e.detail;
    setBusy(true);
    try {
      let result;
      if      (action === 'rename')         result = await client.rename(params.varname, params.newname);
      else if (action === 'label-variable') result = await client.labelVariable(params.varname, params.label);
      else if (action === 'label-define')   result = await client.labelDefine(params.lblname, params.mapping);
      else if (action === 'label-values')   result = await client.labelValues(params.varname, params.lblname);
      else if (action === 'sort')           result = await client.sort(params.varlist, params.ascending);
      else if (action === 'recode')         result = await client.recode(params.varname, params.rules, params.gen);
      else return;

      if (result.type === 'error') {
        appendBlock(result);
        updateVariables(state.varlist);
      } else if (result && result.varlist_after) {
        state.varlist = result.varlist_after;
        state.nCols   = result.varlist_after.length;
        updateVariables(result.varlist_after);
        if (result.n_rows_after != null) state.nRows = result.n_rows_after;
        updateStatus();
      } else {
        // A4-02: malformed DataMutationResult (non-error type, but missing
        // varlist_after — the native envelope bug A4-01 is being fixed
        // separately by WS-E1) — don't crash the listener, skip the refresh.
        updateVariables(state.varlist);
      }
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      updateVariables(state.varlist);
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      setBusy(false);
      document.getElementById('cmd-input')?.focus();
    }
  });

  // Probe kernel
  await checkKernel();

  // Load history (mock or live)
  try {
    const h = await client.history();
    loadHistory(h.history ?? []);
  } catch { /* ignore */ }

  // P7 carry-over: seed console.js's Results-console scrollback cap from the
  // user's real configured/recommended budget at boot too — not only once
  // the Settings panel happens to be opened later in the session (which also
  // calls setMemoryBudgetMb(), see populateSettings() further down this
  // file). Fire-and-forget: a failure here just leaves console.js on its
  // DEFAULT_MEMORY_BUDGET_MB fallback.
  try {
    // N2: hydrateUiState() (above) already fetched + cached this — reuse it
    // instead of a second round trip; falls back to a fresh fetch if that
    // earlier call failed, so this block's own behavior is unchanged.
    const s = _settingsCache ?? await client.getSettings();
    const cur = (s && s.memory_budget_mb) || (s && s.sys && s.sys.recommended_budget_mb);
    if (cur) { setMemoryBudgetMb(cur); setGridMemoryBudgetMb(cur); }  // SPEC II-3
  } catch { /* ignore */ }

  // In mock mode: show persistent banner + pre-populate varlist so the panel isn't empty
  if (useMock) {
    document.getElementById('mock-banner')?.removeAttribute('hidden');
    appendBlock({ type: 'echo', text: 'Open a dataset with File ▾ > Open Data…, or type a command below.' });
    try {
      const vl = await client.varlist();
      if (vl?.varlist?.length) {
        state.varlist = vl.varlist;
        state.nRows   = vl.n_rows ?? 0;
        state.nCols   = vl.n_cols ?? vl.varlist.length;
        state.currentFile = vl.source_file ?? 'fatalities.csv';
        updateVariables(vl.varlist);
        updateStatus();
      }
    } catch { /* ignore */ }
  }

  // 2.2.0 item C: restore a persisted split layout (if any) BEFORE the boot
  // tab reconcile, so the restored columns light their tabs correctly.
  restoreSplit();

  // UX-047: establish the boot tab-active state deterministically (Results —
  // no overlay is open yet). index.html hardcodes .active on the Results tab
  // for zero-FOUC, this just reconciles it via the same code path every
  // later transition uses.
  syncViewTabs();

  // WS-B (extcompat): first-run consent. LAST in init() on purpose — the app
  // behind the dialog is fully wired by the time it paints, so accepting drops
  // the user straight into a live session with nothing left to initialise.
  // Everything above already ran, so this can never delay boot either.
  maybeShowConsent(_settingsCache);
}

// ─── N2 (P8.1-W2): ui_state — native-backed durable store ──────────────────
// contract_B v2.4.0 adds an opaque `ui_state` string field to settings.get/
// settings.set (frontend-owned JSON blob). This closes the "reinstall/
// downgrade loses your layout" gap localStorage-only persistence always had
// on the native build, WITHOUT retiring localStorage: every migrated key
// keeps being written to localStorage exactly as before (every setter below
// is unchanged), and this subsystem mirrors those same values into ui_state
// so they survive independently of the browser-profile-style local store.
// Read preference is ui_state > localStorage, enforced the cheap way: at
// boot, hydrateUiState() overwrites localStorage FROM the (usually identical,
// after the first native session) ui_state blob BEFORE anything else reads
// its own key — see init()'s call site, positioned before initPanes()/
// initGutter()/restoreSplit(). Feature-detected throughout: no native bridge
// (mock/browser) -> both halves are inert no-ops, pure-localStorage exactly
// as pre-N2.
const UI_STATE_KEYS = [
  'piet-split', 'piet-recent-files', 'piet-col-vars', 'piet-col-history',
  'piet-theme', 'piet-mode', 'piet-theme-follow-system', 'piet-font-size',
  'piet-paste-autorun', 'piet-auto-summarize', 'piet-editor-gutter',
];

/** Boot-time hydrate + settings-cache populate (also used by N1's
 *  composeDefaultPath() and the P7 memory-budget seed above). Absent/corrupt
 *  ui_state, or no native bridge -> no-op on the hydrate half; localStorage
 *  (already the fallback) is left exactly as it was. Returns the fetched
 *  settings object (or null on failure) so callers can reuse it instead of
 *  re-fetching. */
async function hydrateUiState() {
  if (!client) return null;
  let s = null;
  try { s = await client.getSettings(); } catch { s = null; }
  _settingsCache = s;
  if (!client.native) return s;   // mock/browser: nothing to hydrate FROM — stays pure-localStorage
  if (!s || typeof s.ui_state !== 'string' || !s.ui_state) return s;
  let blob = null;
  try { blob = JSON.parse(s.ui_state); } catch { blob = null; }
  if (!blob || typeof blob !== 'object') return s;
  for (const key of UI_STATE_KEYS) {
    if (Object.prototype.hasOwnProperty.call(blob, key) && blob[key] != null) {
      try { localStorage.setItem(key, String(blob[key])); } catch { /* storage denied — non-fatal */ }
    }
  }
  // N2 gap fix (P8.1-W2 test-stint finding): index.html's pre-paint script read
  // piet-mode/piet-theme/piet-theme-follow-system/piet-font-size from
  // localStorage synchronously at parse time — BEFORE this async hydration
  // could land the native ui_state values. On a fresh profile that carries
  // prior ui_state (reinstall/cleared-localStorage), first paint used defaults
  // and stayed wrong all session. Re-apply those pre-paint keys live from the
  // hydrated values (syncThemeButtons() runs later in init(), after this).
  const html = document.documentElement;
  const mode = localStorage.getItem('piet-mode');
  if (mode === 'boring' || mode === 'destijl') html.dataset.mode = mode;
  if (localStorage.getItem('piet-theme-follow-system') === '1') {
    applySystemTheme();
  } else {
    const t = localStorage.getItem('piet-theme');
    if (t === 'dark') html.dataset.theme = 'dark';
    else if (t != null) delete html.dataset.theme;   // explicit light ('' persisted by toggleTheme)
  }
  const fz = localStorage.getItem('piet-font-size');
  if (fz) applyFontSize(fz);
  return s;
}

let _uiStatePersistTimer = null;
/** Schedule a debounced native mirror of every migrated localStorage key's
 *  CURRENT value (rebuilt fresh at flush time, not incrementally — so any
 *  key changed by ANY path, including panes.js's own col-vars/col-history
 *  writes picked up via the #app style MutationObserver in init(), rides the
 *  next flush regardless of which specific setter triggered it). No-op with
 *  no native bridge — mock/browser sessions never call setSettings for this. */
function markUiStateDirty() {
  if (!client || !client.native) return;
  clearTimeout(_uiStatePersistTimer);
  _uiStatePersistTimer = setTimeout(_flushUiState, 400);
}

function _flushUiState() {
  const blob = {};
  for (const key of UI_STATE_KEYS) {
    const v = localStorage.getItem(key);
    if (v != null) blob[key] = v;
  }
  client.setSettings({ ui_state: JSON.stringify(blob) }).catch(() => {
    // Best-effort mirror: localStorage (already written by the caller before
    // markUiStateDirty() was ever invoked) stays this session's source of
    // truth regardless; the next successful flush catches up.
  });
}

// ─── Kernel health ────────────────────────────────────────────────────────────

async function checkKernel() {
  const dotEl = document.getElementById('kernel-dot');
  const { ok } = await client.ping();
  state.kernelReady = ok;
  if (dotEl) {
    dotEl.className = ok ? 'kernel-dot dot-ok' : 'kernel-dot dot-err';
    dotEl.title = ok ? 'kernel ready' : 'kernel not reachable';
  }
}

// ─── Command execution ────────────────────────────────────────────────────────

/** Append a command-history entry, keeping the ring bounded (design 8 / A6-06;
 *  mirrors the native side's own 500-entry cap). */
function pushHistory(cmd, success) {
  state.commandHistory.push({ cmd, success });
  if (state.commandHistory.length > 500) state.commandHistory.shift();
}

/** U1: extract the target path from a `save filename[, replace]` command
 *  (Stata syntax) — used to keep state.currentFile honest after a
 *  console-typed save. Unlike `open`, CONTRACT B doesn't add a wire field for
 *  this, so the target is read straight from the command text. */
function parseSaveTarget(cmd) {
  const m = cmd.trim().match(/^save\s+(.+)$/i);
  if (!m) return null;
  let arg = m[1].split(',')[0].trim();     // drop `, replace` etc.
  arg = arg.replace(/^["']|["']$/g, '');   // strip a matching pair of quotes
  return arg || null;
}

async function runCommand(cmd) {
  if (!cmd.trim() || state.busy) return;

  // FR-cls: Stata's Results-window clear command. Exact match (case per
  // Stata is lowercase `cls`; the .trim() above tolerates surrounding
  // whitespace); any extra tokens ("cls foo") do NOT match here and fall
  // through to the engine, which honestly errors it as unsupported (BUG-040
  // principle — don't silently accept what Stata rejects). Client-side only:
  // clears the console via the same clear machinery the × button uses and
  // never reaches the bridge (cls touches no data).
  if (cmd.trim() === 'cls') {
    clearConsole();
    pushHistory('cls', true);
    state.historyIndex = -1;
    appendHistory('cls', true);
    document.getElementById('cmd-input')?.focus();
    return;
  }

  // B7 (P8-S6): bare `help` (no argument) — Stata opens a documentation
  // viewer PIET doesn't have; sending it to the engine bare would either
  // error or silently no-op. Intercept client-side and render ONE honest
  // hint line instead (W10: no modal, no wall of text — SPEC III-6 label
  // honesty over building a help system that doesn't exist yet). `help
  // <command>` (WITH an argument) is left to pass through to the engine
  // unchanged — it's RESERVED for the future Bridge Tutor; this does not
  // special-case it further.
  if (/^help$/i.test(cmd.trim())) {
    appendBlock({ type: 'echo', text: 'help' });
    appendBlock({ type: 'display', text: 'Type a command, or `help <command>` for a specific one — full reference in the handbook.' });
    pushHistory('help', true);
    state.historyIndex = -1;
    appendHistory('help', true);
    document.getElementById('cmd-input')?.focus();
    return;
  }

  // B9 (P8-S7): `note <text>` / `* <text>` — a plain-text annotation, not a
  // command. Rendered as an italic, prompt-less note block (console.js's
  // renderNote() — a .block-display variant with NO '. ' prefix, so it
  // visibly reads as "not a command", SPEC III-6 label honesty) and NEVER
  // reaches the engine. `* ...` is Stata's own full-line-comment syntax —
  // the dispatcher already silently no-ops a bare comment line (BUG-040
  // principle: pure comment lines produce zero blocks today), so this only
  // ADDS visible feedback for what already renders nothing; it doesn't
  // override any real engine behavior. `note` (bare Stata dataset-annotation
  // command) has no dispatcher handler at all, so nothing is shadowed there
  // either. Matched only at the START of the trimmed command, so e.g.
  // `regress y x, note("...")` is untouched — the option's own text is never
  // re-parsed as a command here.
  const noteMatch = cmd.trim().match(/^(?:note\s+(.*)|\*\s?(.*))$/is);
  if (noteMatch) {
    const noteText = (noteMatch[1] ?? noteMatch[2] ?? '').trim();
    const noteCmd = cmd.trim();
    appendBlock({ type: 'note', text: noteText });
    pushHistory(noteCmd, true);
    state.historyIndex = -1;
    appendHistory(noteCmd, true);
    document.getElementById('cmd-input')?.focus();
    return;
  }

  const browseCmds = /^(br|browse|ed|edit)\b/i.test(cmd.trim());
  // B1/B2: a console `do FILE` / `run FILE` blocks while the file runs — start
  // the liveness strip so it isn't a silent freeze; the echo carries resolved_path.
  const isDoRun = /^(do|run)\b/i.test(cmd.trim());

  setBusy(true);
  if (isDoRun) startProgressPolling();

  try {
    const result = await client.command(cmd);
    const blocks = result.blocks ?? [];

    // B2 (1.16.0): a console `do FILE` echoes {resolved_path, source} — load that
    // file's source into the do-file editor pane so the user can inspect/edit it.
    if (isDoRun) {
      const echoB = blocks.find(b => b.type === 'echo' && b.resolved_path);
      if (echoB && typeof echoB.source === 'string') {
        setDoEditorText(echoB.source);
        setDoEditorPath(echoB.resolved_path);
        trackRecentFile(echoB.resolved_path, 'do');   // B3: console `do FILE`/`run FILE`
      }
    }

    // Detect browse/edit command in result — open the overlay
    if (browseCmds || blocks.some(b => b.type === 'browse')) {
      const browseBlock = blocks.find(b => b.type === 'browse');
      if (browseBlock && grid) {
        // Echo first
        const echoB = blocks.find(b => b.type === 'echo');
        if (echoB) appendBlock(echoB);
        // Mission 1: the browse/edit overlay shares the console area with the
        // other three — close them first (registry-driven, same as every
        // other openX()).
        closeAllOverlays('browser-overlay');
        // editable iff the backend flagged it (the `edit` command); browse = read-only
        grid.setEditable(!!browseBlock.editable);
        await grid.load(browseBlock);
        grid.show();
      } else {
        appendBlocks(withMutationSummaries(blocks));
        if (blocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();
      }
    } else {
      appendBlocks(withMutationSummaries(blocks));
      if (blocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();
    }

    // Update varlist + dims if mutation changed it
    if (result.varlist_after) {
      state.varlist = result.varlist_after;
      state.nCols   = result.varlist_after.length;
      updateVariables(result.varlist_after);
      const mutBlock = blocks.find(b => b.type === 'data_mutation');
      if (mutBlock?.n_rows_after != null) state.nRows = mutBlock.n_rows_after;
      // U1: keep the tracked file honest for console-typed loads too (use/
      // sysuse) — File▾ Open already knows its own filepath (onOpenFile(),
      // below); this is the console/do-file leg, which doesn't, so it reads
      // the CONTRACT-B filepath the "open" mutation now carries. A mutation
      // that empties the varlist (clear, drop _all, ...) means data is gone.
      if (result.varlist_after.length === 0) state.currentFile = null;
      else if (mutBlock?.action === 'open' && mutBlock.filepath) state.currentFile = mutBlock.filepath;
      if (mutBlock?.action === 'open') {
        scheduleAutoSummarize();   // B14(b)
        if (mutBlock.filepath) trackRecentFile(mutBlock.filepath, 'data');   // B3: console `use`/`sysuse`
      }
      updateStatus();
    }

    // History
    const success = !blocks.some(b => b.type === 'error');
    // U1: `save filename` (Stata parity) updates the tracked file. No wire
    // field carries this (CONTRACT only adds filepath to the open action),
    // so the target is parsed straight from the command text.
    if (success && /^save\b/i.test(cmd.trim())) {
      const target = parseSaveTarget(cmd);
      if (target) { state.currentFile = target; updateStatus(); }
    }
    // U7 (P8.1): mirror a SUCCESSFUL engine-side `graph drop _all` into the
    // frontend Graphics gallery. Response-driven, NOT a cls-style intercept —
    // the native dispatcher already owns this command (dispatcher.cpp BS-056,
    // clears session.namedGraphs); intercepting client-side would leave that
    // engine store stale. Named drops (`graph drop g1`) stay engine-only: the
    // gallery is an ordered visual log, not a name-indexed store.
    if (success && /^graph\s+drop\b/i.test(cmd.trim()) && /\b_all\b/i.test(cmd)) {
      clearGraphGallery();
      refreshGraphicsIfOpen();
    }
    pushHistory(cmd, success);
    state.historyIndex = -1;
    appendHistory(cmd, success);

  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    pushHistory(cmd, false);
    appendHistory(cmd, false);
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    if (isDoRun) stopProgressPolling();
    setBusy(false);
    document.getElementById('cmd-input')?.focus();
  }
}

// ─── B14(b): auto-summarize on load ────────────────────────────────────────
// Called from every spot that detects a data_mutation block with
// action:"open" — onOpenFile()'s synthetic block, runCommand()'s mutBlock
// (console `use`/`sysuse`), and applyDoResult()'s lastMut (a do-file `use`/
// `sysuse`). Gated on the 'piet-auto-summarize' setting (setAutoSummarize(),
// OFF by default).

/** Schedule an auto-summarize pass if the setting is ON. Deferred via
 *  setTimeout(…, 0) because every call site is still inside its OWN
 *  setBusy(true) lock when this runs — runAutoSummarize() needs state.busy
 *  to have already been released (that call site's own `finally`), and
 *  re-checks it itself as a second guard against a fast next command. */
function scheduleAutoSummarize() {
  if (localStorage.getItem('piet-auto-summarize') !== '1') return;
  setTimeout(runAutoSummarize, 0);
}

/** Dispatch `summarize` through the SAME command path runCommand() uses
 *  (client.command()) — echoed as "summarize (auto)" so the Results log and
 *  command history stay honest about what actually ran (never silently
 *  injected). Skips quietly (no error) if busy, if data is gone, or if the
 *  setting flipped OFF between scheduling and now. */
async function runAutoSummarize() {
  if (state.busy || !state.varlist.length) return;
  if (localStorage.getItem('piet-auto-summarize') !== '1') return;
  setBusy(true);
  try {
    appendBlock({ type: 'echo', text: 'summarize (auto)' });
    const result = await client.command('summarize');
    const blocks = result.blocks ?? [];
    appendBlocks(withMutationSummaries(blocks));
    const success = !blocks.some(b => b.type === 'error');
    pushHistory('summarize (auto)', success);
    state.historyIndex = -1;
    appendHistory('summarize (auto)', success);
  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    setBusy(false);
  }
}

// ─── B3 (P8-S6): Recent files ──────────────────────────────────────────────
// localStorage-only, mirrors the piet-split/piet-paste-autorun persistence
// idiom above — no bridge route. trackRecentFile() is called from every site
// that already knows a just-opened path (onOpenFile/onOpenDoFile/onRunDo
// below, and runCommand()'s console `use`/`sysuse`/`do FILE` legs above);
// renderRecentFiles() re-populates the File▾ "Recent" group from storage.

function loadRecentFiles() {
  try {
    const list = JSON.parse(localStorage.getItem(RECENT_KEY) || '[]');
    return Array.isArray(list) ? list : [];
  } catch { return []; }
}

/** Record `path` as most-recently-opened (moves to front if already present,
 *  dedupe by path), cap at RECENT_MAX, persist, and refresh the File▾ menu. */
function trackRecentFile(path, kind) {
  if (!path || typeof path !== 'string') return;
  let list = loadRecentFiles().filter(f => f.path !== path);
  list.unshift({ path, kind, ts: Date.now() });
  if (list.length > RECENT_MAX) list.length = RECENT_MAX;
  try { localStorage.setItem(RECENT_KEY, JSON.stringify(list)); } catch { /* storage full/disabled — non-fatal */ }
  markUiStateDirty();   // N2: mirror piet-recent-files into ui_state
  renderRecentFiles();
}

/** Rebuild the File▾ "Recent" group's DOM from localStorage. Safe to call any
 *  time (boot, and after every trackRecentFile()). Discoverability fix
 *  (III-6-compatible): the group now stays VISIBLE even while empty, showing
 *  one non-clickable "(no recent files)" row instead of hiding the whole
 *  group — a hidden-until-populated group gave no hint the feature existed
 *  at all; the label now honestly explains its own empty state. */
function renderRecentFiles() {
  const group = document.getElementById('file-menu-recent-group');
  const list  = document.getElementById('file-menu-recent');
  if (!group || !list) return;
  const entries = loadRecentFiles();
  list.innerHTML = '';
  group.hidden = false;
  if (!entries.length) {
    const empty = document.createElement('div');
    empty.className = 'menu-recent-item menu-recent-empty';
    empty.textContent = '(no recent files)';
    list.appendChild(empty);
    return;
  }
  for (const entry of entries) {
    const a = document.createElement('a');
    a.href = '#';
    a.className = 'menu-recent-item';
    a.textContent = entry.path.split(/[/\\]/).pop() || entry.path;
    a.title = entry.path;
    a.addEventListener('click', e => {
      e.preventDefault();
      openRecentFile(entry);
    });
    list.appendChild(a);
  }
}

/** Reopen a Recent-files entry via the SAME open route its kind already uses
 *  — onOpenFile(path) for data files, onOpenDoFile(path) for do-files (load
 *  into the editor; deliberately NOT onRunDo — a one-click Recent reopen
 *  should never auto-run). No new bridge call. */
function openRecentFile(entry) {
  if (state.busy) return;
  if (entry.kind === 'do') onOpenDoFile(entry.path);
  else onOpenFile(entry.path);
}

// ─── File open ────────────────────────────────────────────────────────────────

/**
 * @param {string} [knownPath] B3 (P8-S6): when given (a Recent-files click),
 *   skip the picker and open this path directly — the SAME open route the
 *   picker leg uses below, so a recent-file reopen is byte-identical to
 *   picking it fresh. Omit to prompt via the native file picker (File ▾ >
 *   Open Data…, the original call site).
 */
async function onOpenFile(knownPath) {
  // A1-01: don't race a command/do-run/mutation already in flight.
  if (state.busy) return;
  // 2.0.1: open a native Finder file picker (NSOpenPanel) via the dialog.open
  // bridge route. The old code checked window.pywebview (the retired 1.x
  // launcher) then fell back to window.prompt — but the 2.0 native shell has
  // neither pywebview nor a WKWebView prompt panel, so this button was a dead
  // no-op. client.pickFile() resolves to the chosen absolute path (or null on
  // cancel); errors from open() below still surface in the try/catch.
  let filepath = knownPath;
  setBusy(true);
  try {
    if (!filepath) {
      filepath = await client.pickFile('dta,csv,hbin');
      if (!filepath) return;   // user cancelled — no error
    }

    const ext = filepath.split('.').pop().toLowerCase();
    const format = ext === 'dta' ? 'dta' : ext === 'csv' ? 'csv' : 'hbin';

    const result = await client.open(filepath, format);

    // CONTRACT B v1.1: failure returns HTTP 200 + {type:"error", ...}
    if (result.type === 'error') {
      appendBlock(result);
    } else {
      state.currentFile = filepath;
      state.varlist     = result.varlist_after ?? [];
      state.nRows       = result.n_rows_after ?? 0;
      state.nCols       = state.varlist.length;

      updateVariables(state.varlist);
      updateStatus();

      const fname = filepath.split(/[/\\]/).pop();
      appendBlock({ type: 'echo', text: `use ${fname}` });
      appendBlock({ type: 'data_mutation', action: 'open', varlist_after: state.varlist, n_rows_after: state.nRows });
      scheduleAutoSummarize();   // B14(b)
      trackRecentFile(filepath, 'data');   // B3
    }

  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: `Failed to open file: ${err.message ?? err}` });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    setBusy(false);
    document.getElementById('cmd-input')?.focus();
  }
}

// ─── Run .do ─────────────────────────────────────────────────────────────────

/**
 * Opening a do-file moves the working directory to its folder, so its relative
 * `use "data.dta"` finds the data beside it. A working directory that changes
 * under the user must not change silently, so the move is announced. Nothing is
 * printed when the file was already in the working directory. A later `cd` by the
 * user stands: the move happens when a file is opened, never when it is run.
 */
function noteWorkingDirectory(dir) {
  if (dir) appendBlock({ type: 'note', text: `Working directory is now ${dir} (the do-file's folder)` });
}

/**
 * BUG-045 (SPEC III-6 label honesty): a control's label is a contract — "Run
 * .do" must run. Picking a .do file loads its source into the Do-file Editor
 * AND runs it immediately via the SAME buffer-run path runDoEditor() uses
 * (runDoSourceCore(), below). If the run stops on an error, the editor
 * re-opens (source already loaded) so the fix-in-place intent is realized;
 * on success the editor stays closed and results are visible in the console
 * (unchanged). The pure load-without-run affordance — the 2.1.0 behavior
 * this replaces — moved to File ▾ > Open do-file… (BUG-046, onOpenDoFile()).
 * Console-typed `do FILE` (runCommand(), above) is untouched.
 */
async function onRunDo() {
  // A1-01: don't race a command/do-run/mutation already in flight.
  if (state.busy) return;
  // 2.0.1: native Finder picker (.do) via the dialog.open bridge route — was a
  // dead window.pywebview/window.prompt fallback in the WKWebView shell. Errors
  // from readDoFile() below still surface in the try/catch.
  let filepath;
  setBusy(true);
  try {
    filepath = await client.pickFile('do');
    if (!filepath) { document.getElementById('cmd-input')?.focus(); return; }   // user cancelled — no error
    const { source, resolved_path, cwd_changed } = await client.readDoFile(filepath, { followCwd: true });
    noteWorkingDirectory(cwd_changed);
    setDoEditorText(source);
    setDoEditorPath(resolved_path);
    trackRecentFile(resolved_path || filepath, 'do');   // B3 — reopening from
    // Recent always goes through onOpenDoFile (load-only), never back through
    // this run-immediately path, so a recent-file click can't silently re-run.
    // Guard interplay: runDoSourceCore() assumes the CALLER already holds
    // state.busy (see its doc comment below) — this call is still inside the
    // setBusy(true)/finally pair above, so that contract holds. Calling
    // runDoEditor() here instead would self-block: it re-checks state.busy
    // at its own top, which is already true from setBusy(true) above, so it
    // would return immediately without ever running the buffer.
    await runDoSourceCore(source, 'do (editor)');
  } catch (err) {
    // readDoFile() rejects with bridge error code "bad_request" for a
    // missing/non-.do/>2MB file — _nativeRequest() folds that into a normal
    // Error, so this reads the same as every other engine_error call site.
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    setBusy(false);
    // Don't steal focus from the editor if runDoSourceCore just re-opened it
    // (the stopped-on-error path) — openDoEditor() already focused #doedit-text.
    if (!_isOverlayOpen('doedit-overlay')) document.getElementById('cmd-input')?.focus();
  }
}

/**
 * BUG-173 (W:P8, User-found on the B.8.0 verification build): route a DROPPED
 * file by what it actually is.
 *
 * The drop delegate used to hand every path straight to onOpenFile(), the
 * DATA path — and onOpenFile() reads its format as "dta, or csv, or otherwise
 * assume hbin". So dropping the one file a Stata user is most likely to drop,
 * a `.do`, ran it through the hbin loader and answered
 * `bad magic (not a .hbin file): /…/p8_target_piet.do` — an error about a
 * format the user never mentioned, for an operation they never asked for.
 * Every other non-data extension (.log, .txt, a PDF) answered the same way.
 *
 * A dropped `.do` now LOADS INTO THE EDITOR without running (onOpenDoFile's
 * contract). Deliberately load-only: a drop is one gesture with no
 * confirmation step, and silently executing a script because it landed on the
 * window is not a thing this app should do — "Run .do" and the editor's own
 * Run button are where running is asked for. Data extensions are unchanged.
 * Anything else is named honestly instead of being guessed at.
 */
function routeDroppedFile(path) {
  const ext = String(path).split('.').pop().toLowerCase();
  if (ext === 'do') { onOpenDoFile(path); return; }
  if (ext === 'dta' || ext === 'csv' || ext === 'hbin') { onOpenFile(path); return; }
  appendBlock({
    type: 'error',
    code: 'engine_error',
    message: `Dropped file has extension .${ext}, which PIET does not open. ` +
             'Drop a dataset (.dta, .csv, .hbin) or a do-file (.do).',
  });
}

/**
 * BUG-046: File ▾ > Open do-file… — the pure "load a .do file into the
 * editor, don't run it" path. This is exactly onRunDo's pre-BUG-045 behavior,
 * relocated here now that Run .do actually runs (label honesty). Foregrounds
 * the editor; nothing executes.
 * @param {string} [knownPath] B3 (P8-S6): Recent-files reopen — see
 *   onOpenFile()'s matching param doc above.
 */
async function onOpenDoFile(knownPath) {
  // A1-01: don't race a command/do-run/mutation already in flight.
  if (state.busy) return;
  let filepath = knownPath;
  setBusy(true);
  try {
    if (!filepath) {
      filepath = await client.pickFile('do');
      if (!filepath) { document.getElementById('cmd-input')?.focus(); return; }   // user cancelled — no error
    }
    const { source, resolved_path, cwd_changed } = await client.readDoFile(filepath, { followCwd: true });
    noteWorkingDirectory(cwd_changed);
    setDoEditorText(source);
    setDoEditorPath(resolved_path);
    openDoEditor();   // foreground; focuses #doedit-text itself — don't refocus cmd-input below
    trackRecentFile(resolved_path || filepath, 'do');   // B3
  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    document.getElementById('cmd-input')?.focus();
  } finally {
    setBusy(false);
  }
}

/**
 * Shared do-run core (BUG-045): executes `source` via /api/do_string, echoes
 * a history-labeled marker, polls run progress, renders results
 * (applyDoResult), and records a history entry. If the run stopped on an
 * engine-reported error (an error block, or stopped_at_line/stopped_at_reason
 * in the response), re-opens the do-file editor — the source is already
 * loaded there — so the fix-in-place intent survives regardless of how the
 * run was launched: the toolbar's Run .do file-picker flow (onRunDo) and the
 * editor's own ▶ Run button (runDoEditor) both funnel through here. On
 * success the editor stays closed (current behavior).
 *
 * Contract: the CALLER owns state.busy (setBusy/finally) and any pre-run
 * editor-visibility flip (e.g. runDoEditor's closeDoEditor()) — this
 * function touches neither, so a caller that is already inside its own busy
 * lock can invoke it in-line without a second, self-blocking busy guard.
 */
async function runDoSourceCore(source, label, lineOffset = 0) {
  appendBlock({ type: 'echo', text: label });
  startProgressPolling();                          // B1: live liveness strip
  try {
    const result = await client.runDoString(source);
    applyDoResult(result);
    const stopped = (result.blocks ?? []).some(b => b.type === 'error') ||
                    result.stopped_at_line != null || !!result.stopped_at_reason;
    if (stopped) {
      openDoEditor();   // fix-in-place: bring the loaded source back to front
      // U6: point at the exact failing line — lineOffset accounts for a
      // partial-selection run (runDoEditorSelection), where stopped_at_line
      // is relative to the SUBMITTED source, not the whole buffer.
      if (result.stopped_at_line != null) selectEditorLine(result.stopped_at_line + lineOffset);
    }
    const ok = !stopped;
    pushHistory(label, ok);
    appendHistory(label, ok);
  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    stopProgressPolling();
  }
}

/**
 * B.3.0 (bounded transcripts, contract_B 2.5.0) TEMPORARY COMPAT GUARD —
 * remove at 2.6.0 once we're confident only the new shape is ever in play,
 * and inline `blocks` back into applyDoResult() below in its place.
 *
 * Pre-2.5.0: when `streamed > 0`, the do/do_string final response's `blocks`
 * was the FULL array for the whole run (already-streamed blocks included) —
 * the render step had to slice off the already-streamed prefix itself
 * (`doBlocks.slice(streamed)`).
 * 2.5.0+: when `streamed > 0`, `blocks` already contains ONLY the un-streamed
 * TAIL — slicing AGAIN would slice INTO that tail and drop part of it.
 * Verified against the native lane's actual F6c implementation (same
 * worktree, piet/native/src/main.cpp's runDoInBackground, ~line 405-438):
 * `tailBlocks()` there returns `ExecuteResult.blocks[streamedCount:]` as the
 * wire `blocks`, and its own comment states the contract in so many words —
 * "the frontend MUST drop that slice". The tail is whatever `pushBlock`
 * never got to stream: typically empty, or the one stored-but-unstreamed
 * block (doexec.cpp's cancelledBlock() — an `error`/`engine_error` block,
 * NOT a synthetic display block — for a cancelled run), or an F6a elision
 * marker once a run trips the stored-transcript ceiling. On the wire path
 * that actually reaches this frontend (bindAsyncRoute("do"/"do_string") ->
 * runDoInBackground), the legacy shape is not reachable today — this guard
 * is insurance for the 2.5.0 rollout window (e.g. a stale cached JS bundle
 * against a rebuilt native core during dev/debug), not a live dual-shape
 * production contract.
 *
 * Discriminator: an un-sliced legacy array is virtually always LONGER than
 * `streamed` (streamed counts a strict subset of it); the new tail is far
 * shorter (see above). `blocks.length > streamed` is therefore a safe (if
 * heuristic) way to tell the two shapes apart during the transition. Known
 * blind spot: a LEGACY run where EVERY block was already streamed (streamed
 * === blocks.length exactly) misreads as the new shape and appends the full
 * array as-is — a rare all-streamed edge case, and even then it only
 * double-renders rather than silently dropping anything. Acceptable for a
 * short-lived compat window; not a long-term discriminator.
 */
function _tailBlocksToRender(blocks, streamed) {
  if (streamed <= 0) return blocks;                              // nothing streamed — render everything, unchanged
  if (blocks.length > streamed) return blocks.slice(streamed);   // legacy full-array shape — slice off the streamed prefix
  return blocks;                                                 // 2.5.0+ tail-only shape — already just the remainder, append as-is
}

/** Render a /api/do or /api/do_string result: blocks + stop notice + varlist refresh. */
function applyDoResult(result) {
  const doBlocks = result.blocks ?? [];
  // N6 (P8.1-W2, contract_B v2.4.0): blocks[0..streamed) were already
  // rendered live via the {kind:"block"} push (initPietEventsBridge, above)
  // — render only the remainder so nothing double-appears. `streamed` is
  // additive/optional; absent or 0 (a transport that never pushes — fetch/
  // mock, or a native run with nothing to stream) renders every block here,
  // byte-identical to pre-N6 behavior. Everything ELSE in this function
  // (stopped_at handling, lastMut, refreshVarlistAuthoritative) still reads
  // the FULL doBlocks/result — only the live-console RENDER is deduped.
  // B.3.0 (contract_B 2.5.0): _tailBlocksToRender() replaces a bare
  // doBlocks.slice(streamed) — see its doc comment just above for the shape
  // change and the temporary compat guard.
  const streamed = Number(result.streamed) > 0 ? Number(result.streamed) : 0;
  appendBlocks(withMutationSummaries(_tailBlocksToRender(doBlocks, streamed)));
  if (doBlocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();   // U8
  // BUG-150: distinguish a HALT from a FAILURE. doexec.cpp sets
  // stopped_at_reason only in the else-branch of handleUncapturedError() —
  // i.e. never for a clean `exit`, which instead records only the line and an
  // exit code. "line but no reason" is therefore exactly, and only, a clean
  // exit; a real stop always names its reason. Saying "stopped at line 284"
  // about a run that finished the way its author wrote it reads as a failure,
  // which is the other half of the red-block defect (console.js suppresses the
  // sentinel itself).
  const cleanExit = result.stopped_at_line != null && !result.stopped_at_reason;
  if (cleanExit) {
    appendBlock({ type: 'echo', text: 'do-file ended (exit)' });
  } else if (result.stopped_at_line != null || result.stopped_at_reason) {
    const linePart   = result.stopped_at_line != null ? ` at line ${result.stopped_at_line}` : '';
    const reasonPart = result.stopped_at_reason ? ` — ${result.stopped_at_reason}` : '';
    appendBlock({ type: 'echo', text: `do-file stopped${linePart}${reasonPart}` });
  }
  // A13(3) (P8.1-T5): mirror the stop onto the statusbar too — flashStatus()
  // is the one-glance surface (B2), so noticing a stop shouldn't require
  // scrolling the log. The wire carries STRING error codes (syntax_error,
  // no_data_loaded, …), not Stata's numeric r() — mirrored honestly as the
  // code itself rather than fabricating an r()-shaped number. Independent of
  // the appendBlock() branch above (its gate is stopped_at_reason, not code).
  {
    // BUG-150: same clean-exit exemption as the console line above — a
    // finished run must not flash "stopped" at the one-glance surface either.
    const stopCode = doBlocks.find(b => b.type === 'error')?.code;
    if (!cleanExit && (result.stopped_at_line != null || stopCode)) {
      const lineBit = result.stopped_at_line != null ? `stopped at line ${result.stopped_at_line}` : 'stopped';
      flashStatus(stopCode ? `${lineBit} — ${stopCode}` : lineBit, 6000);
    }
  }
  // Cheap within-run update from the last mutation block, if any (top-level
  // varlist_after is not in the /api/do contract)...
  const lastMut = [...doBlocks].reverse().find(b => b.type === 'data_mutation');
  if (lastMut?.varlist_after) {
    state.varlist = lastMut.varlist_after;
    state.nCols   = lastMut.varlist_after.length;
    updateVariables(lastMut.varlist_after);
  }
  if (lastMut?.n_rows_after != null) state.nRows = lastMut.n_rows_after;
  if (lastMut) updateStatus();
  if (lastMut?.action === 'open') scheduleAutoSummarize();   // B14(b): do-file `use`/`sysuse`
  // ...then ALWAYS resync from the authoritative session state (BUG-044): a
  // run can change the dataset without emitting any data_mutation block —
  // `clear all` wipes it, a stop-at-error run may mutate then halt — and the
  // heuristic above then leaves the variables pane + statusbar showing a
  // ghost of the pre-run dataset while gen/describe correctly see none.
  refreshVarlistAuthoritative();
}

/** BUG-044: fetch GET /api/varlist (the session's real state) and sync the
 *  variables pane, dims, and statusbar. Fire-and-forget; failures leave the
 *  UI as-is (kernel-dot machinery handles a dead bridge separately). */
async function refreshVarlistAuthoritative() {
  try {
    const vl = await client.varlist();
    const list = vl?.varlist ?? [];
    state.varlist = list;
    state.nCols   = vl?.n_cols ?? list.length;
    state.nRows   = vl?.n_rows ?? 0;
    if (list.length === 0) state.currentFile = null;  // data gone → "(no data)"
    // U1: the do-file/editor run path's authoritative leg — a `use`/`sysuse`
    // inside a .do file has no local filepath the way File▾ Open does, so
    // pick it up from the wire's source_file (CONTRACT B) once there's data
    // again. `||` (not `??`) deliberately keeps the previous value on an
    // empty-string/falsy source_file rather than clobbering it.
    else state.currentFile = vl?.source_file || state.currentFile;
    updateVariables(list);
    updateStatus();
  } catch { /* bridge hiccup: keep current UI; ping/kernel-dot owns liveness */ }
}

// ─── U9: mutation-detail text summaries ────────────────────────────────────
// console.js's renderMutation() (private to that module — a different agent
// owns console.js this sprint) doesn't yet know about merge_counts/n_dropped/
// reshape direction+n_wide+n_long, three EXISTING wire fields on a
// data_mutation block that it currently just falls through to a generic
// "(<action> completed)" for. Rather than touch console.js, synthesize a
// plain TEXT-ONLY `display` block (console.js's existing generic text
// renderer — same one `display`/`list` output uses) right after the
// mutation, so the detail is honestly visible without guessing at a
// console.js-side rendering change.

/** Build zero or more supplementary `display` blocks for a data_mutation
 *  block's merge_counts / n_dropped / reshape fields (whichever are present). */
function summarizeMutationExtras(block) {
  if (!block || block.type !== 'data_mutation') return [];
  const extra = [];

  if (block.merge_counts) {
    const mc = block.merge_counts;
    // Real wire shape (native handlers_data_verbs.cpp:1911-1913): named keys
    // master_only (_merge==1) / using_only (_merge==2) / matched (_merge==3).
    const n1 = Number(mc.master_only) || 0;  // from master
    const n2 = Number(mc.using_only)  || 0;  // from using
    const n3 = Number(mc.matched)     || 0;  // matched
    const lines = [
      '    Result                           # of obs.',
      '    -----------------------------------------',
      `    not matched                    ${String(n1 + n2).padStart(7)}`,
      `        from master                    ${String(n1).padStart(3)}  (_merge==1)`,
      `        from using                     ${String(n2).padStart(3)}  (_merge==2)`,
      '',
      `    matched                        ${String(n3).padStart(7)}  (_merge==3)`,
      '    -----------------------------------------',
    ];
    extra.push({ type: 'display', text: lines.join('\n') });
  }

  if (block.n_dropped != null) {
    extra.push({ type: 'display', text: `(${block.n_dropped} observations deleted)` });
  }

  if (block.direction && (block.n_wide != null || block.n_long != null)) {
    extra.push({ type: 'display',
      text: `(data reshaped to ${block.direction}: ${block.n_wide ?? '?'} wide obs -> ${block.n_long ?? '?'} long obs)` });
  }

  return extra;
}

/** Expand a blocks array, inserting summarizeMutationExtras()'s output right
 *  after each data_mutation block that carries one of the extra fields.
 *  Shared by runCommand() and applyDoResult() — the only two places blocks
 *  from the wire get appended to the console. */
function withMutationSummaries(blocks) {
  const out = [];
  for (const b of blocks) {
    out.push(b);
    out.push(...summarizeMutationExtras(b));
  }
  return out;
}

// ─── Do-file editor (in-app) ──────────────────────────────────────────────────

// Dirty dot (P8.1-T5): Stata's own `*` convention — title only, no tab dot.
// _doEditorPath is the last path setDoEditorPath() was told about, kept so a
// dirty-flag-only change (an input keystroke) can re-render the title without
// its caller re-supplying the path.
let _doEditorPath = null;
let _doEditorDirty = false;

/** WS2: load do-file source into the editor textarea WITHOUT opening the overlay. */
function setDoEditorText(src) {
  const ta = document.getElementById('doedit-text');
  if (ta && typeof src === 'string') ta.value = src;
  markDoEditorClean();   // a fresh load has no unsaved changes yet
}

/** B2 (1.16.0): show the opened file's basename in the editor title bar. */
function setDoEditorPath(path) {
  _doEditorPath = (path && typeof path === 'string') ? path : null;
  renderDoEditorTitle();
}

/** Dirty dot (P8.1-T5): re-render #doedit-title from the current path +
 *  dirty flag — the one place that combines them, called whenever either
 *  input changes (setDoEditorPath, markDoEditorDirty/Clean). */
function renderDoEditorTitle() {
  const el = document.getElementById('doedit-title');
  if (!el) return;
  const mark = _doEditorDirty ? ' *' : '';
  if (_doEditorPath) {
    const base = _doEditorPath.split('/').pop().split('\\').pop();
    el.textContent = `Do-file Editor — ${base}${mark}`;
    el.title = _doEditorPath;
  } else {
    el.textContent = `Do-file Editor${mark}`;
    el.removeAttribute('title');
  }
}

/** Dirty dot (P8.1-T5): mark modified-since-load — wired to #doedit-text's
 *  'input' event (init(), below). */
function markDoEditorDirty() {
  if (_doEditorDirty) return;   // already marked — skip the redundant re-render
  _doEditorDirty = true;
  renderDoEditorTitle();
}

/** Dirty dot (P8.1-T5): mark no-unsaved-changes — called by setDoEditorText
 *  (a fresh load) and the Clear button (init(), below). */
function markDoEditorClean() {
  if (!_doEditorDirty) return;
  _doEditorDirty = false;
  renderDoEditorTitle();
}

/** U6: scroll the do-file editor to `lineNum` (1-based) and select that
 *  line's text — so a stop-on-error run visibly points at the failing line
 *  instead of leaving the editor scrolled wherever it happened to be. */
function selectEditorLine(lineNum) {
  const ta = document.getElementById('doedit-text');
  if (!ta || lineNum == null) return;
  const lines = ta.value.split('\n');
  const idx = Math.max(0, Math.min(lineNum - 1, lines.length - 1));
  let start = 0;
  for (let i = 0; i < idx; i++) start += lines[i].length + 1;   // +1 for the '\n'
  const end = start + lines[idx].length;
  ta.focus();
  ta.setSelectionRange(start, end);
  // textarea has no native "scroll to selection" API — approximate from line
  // height, with a little context above the target line.
  const lineHeight = parseFloat(getComputedStyle(ta).lineHeight) || 16;
  ta.scrollTop = Math.max(0, (idx - 3) * lineHeight);
}

function openDoEditor() {
  const ov = document.getElementById('doedit-overlay');
  if (!ov) return;
  // 2.2.0 item C: in split-view the editor is a docked column beside Results
  // (coexists with the other surfaces) — don't close anything, just dock it.
  if (splitOn) { dockSurface('doedit-overlay', true); document.getElementById('doedit-text')?.focus(); return; }
  // Mission 1: close whichever other overlay shares the console area first
  // (registry-driven — replaces the old ad hoc per-function close subset).
  closeAllOverlays('doedit-overlay');
  ov.classList.add('open');
  document.getElementById('doedit-text')?.focus();
  syncViewTabs();   // UX-047: Do-file tab active
}

function closeDoEditor() {
  // 2.2.0 item C: in split-view "close" = undock the editor column.
  if (splitOn) { dockSurface('doedit-overlay', false); return; }
  const ov = document.getElementById('doedit-overlay');
  if (ov) ov.classList.remove('open');
  document.getElementById('cmd-input')?.focus();
  syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
}

/** ▶ Run — execute the whole editor buffer (unchanged behavior). */
async function runDoEditor() {
  const ta = document.getElementById('doedit-text');
  if (!ta) return;
  await _runEditorBuffer(ta.value, 'do (editor)');
}

/**
 * 2.2.0 item B — Execute selection (do). If a range is selected in the
 * do-file editor, run ONLY that range; with no selection, run the whole
 * buffer (Stata's Do-button parity). Same execution path as Run .do (no new
 * engine surface); BUG-045's stop-on-error editor re-open applies here too
 * (via _runEditorBuffer → runDoSourceCore).
 */
async function runDoEditorSelection() {
  const ta = document.getElementById('doedit-text');
  if (!ta) return;
  const start = ta.selectionStart ?? 0;
  const end   = ta.selectionEnd ?? 0;
  const hasSel = end > start;
  const source = hasSel ? ta.value.slice(start, end) : ta.value;
  // U6: a selection-run's stopped_at_line is relative to the SUBMITTED
  // source, not the whole buffer — count the newlines before the selection
  // start so runDoSourceCore can point the editor at the right absolute line.
  const lineOffset = hasSel ? (ta.value.slice(0, start).match(/\n/g) ?? []).length : 0;
  await _runEditorBuffer(source, hasSel ? 'do (selection)' : 'do (editor)', lineOffset);
}

/** Shared editor-run core for ▶ Run and Execute-selection: busy-guard, hide
 *  the editor (single-view only — in split it stays a column beside Results),
 *  run via the whole-file do_string path, restore. */
async function _runEditorBuffer(source, label, lineOffset = 0) {
  if (!source.trim() || state.busy) return;
  setBusy(true);
  const runBtn = document.getElementById('doedit-run');
  if (runBtn) runBtn.disabled = true;
  // A6 (P8.1-T2): the new Run-selection button shares the same busy-guard as
  // ▶ Run — both funnel through this one shared core.
  const runSelBtn = document.getElementById('doedit-run-selection');
  if (runSelBtn) runSelBtn.disabled = true;
  try {
    // Single-view: hide the editor so results are visible in the console
    // (runDoSourceCore re-opens it iff the run stops on an error). Split-view:
    // Results is already a column beside the editor — leave the editor docked.
    if (!splitOn) closeDoEditor();
    await runDoSourceCore(source, label, lineOffset);
  } finally {
    setBusy(false);
    if (runBtn) runBtn.disabled = false;
    if (runSelBtn) runSelBtn.disabled = false;
    // Don't steal focus from the editor if runDoSourceCore just re-opened it
    // (single-view stop-on-error) or it's a docked column (split).
    if (!_isOverlayOpen('doedit-overlay')) document.getElementById('cmd-input')?.focus();
  }
}

// ─── Graphics gallery (WS4) ────────────────────────────────────────────────────

/** U8: rebuild the Graphics gallery in place if it's currently showing
 *  (docked column in split-view, or the single-view overlay) — a newly
 *  registered graph (console.js's registerGraph(), invoked from its own
 *  appendBlock() for every `type:'graph'` block) should appear immediately,
 *  with no close/reopen needed. openGraphics() is idempotent — it rebuilds
 *  from getGraphEntries() and re-applies the SAME open/docked state — so it's safe
 *  to call whenever a freshly appended batch of blocks included a graph. */
function refreshGraphicsIfOpen() {
  const ov = document.getElementById('graphics-overlay');
  if (ov && ov.classList.contains('open')) openGraphics();
}

function openGraphics() {
  const ov = document.getElementById('graphics-overlay');
  const gal = document.getElementById('graphics-gallery');
  if (!ov || !gal) return;
  // Mission 1 / 2.2.0 item C: single-view closes the other overlays first;
  // split-view docks Graphics as a column beside Results (coexists) — the
  // gallery is rebuilt either way.
  if (!splitOn) closeAllOverlays('graphics-overlay');

  // Rebuild the gallery from the session's collected graph entries — every
  // one, in order: resident blocks render now, and a plot console.js has
  // stored on disk (gallery spill) is a card loaded when it is opened or
  // downloaded (BUG-1471; getGraphs() was resident-only).
  gal.replaceChildren();
  const entries = getGraphEntries();
  if (!entries.length) {
    _graphicsFullIndex = -1;   // B10: nothing to view full
    const empty = document.createElement('div');
    empty.className = 'graphics-empty';
    empty.textContent = 'No graphs yet — run a plotting command (e.g. histogram, scatter, marginsplot).';
    gal.appendChild(empty);
  } else {
    // B10: out-of-range guard — the gallery may have shrunk (a fresh session,
    // `graph drop _all`-equivalent, the BUG-066 byte/count cap, …) since a
    // prior full view was open.
    if (_graphicsFullIndex >= entries.length) _graphicsFullIndex = -1;
    // BUG-066: the gallery is byte- and count-bounded (console.js
    // enforceGalleryCap). Say so when it has actually dropped something —
    // the same honesty the console's elision marker carries, rather than
    // letting older plots vanish silently — and how many it keeps on disk.
    const galleryNote = text => {
      const note = document.createElement('div');
      note.className = 'graphics-elision';
      note.textContent = text;
      gal.appendChild(note);
    };
    const dropped = getGraphDroppedCount();
    if (dropped > 0) galleryNote(`…${dropped} older plot${dropped === 1 ? '' : 's'} trimmed (memory budget)`);
    const stored = getGraphSpilledCount();
    if (stored > 0) galleryNote(`${stored} plot${stored === 1 ? '' : 's'} stored on disk (memory budget) — loaded when opened`);
    entries.forEach((ent, i) => {
      const card = document.createElement('div');
      card.className = 'graphics-card';
      card.tabIndex = 0;
      card.setAttribute('role', 'button');
      card.setAttribute('aria-label', `View ${ent.kind || 'graph'} ${i + 1} of ${entries.length} full size`);
      // Re-render the SAME block as a thumbnail (no new rendering code); a
      // stored plot shows a placeholder until it is loaded.
      const block = getResidentGraph(ent.id);
      let thumb;
      if (block) {
        thumb = renderGraph(block);
      } else {
        thumb = document.createElement('div');
        thumb.className = 'graphics-empty';
        thumb.textContent = `${ent.title || ent.kind || 'graph'} — stored on disk; open to view`;
      }
      card.appendChild(thumb);
      const dl = document.createElement('button');
      dl.className = 'graphics-dl-btn';
      dl.textContent = '⬇ PNG';
      dl.addEventListener('click', async e => {
        e.stopPropagation();   // B10: a download click must not ALSO open the full view
        if (!thumb.querySelector('svg')) {
          const loaded = await getGraphById(ent.id);
          if (!loaded) { thumb.textContent = 'This plot could not be loaded from disk.'; return; }
          const t = renderGraph(loaded);
          card.replaceChild(t, thumb);
          thumb = t;
        }
        const svg = thumb.querySelector('svg');
        if (svg) svgToPng(svg, `${ent.kind || 'graph'}-${i + 1}.png`);
      });
      card.appendChild(dl);
      // B10: click (or Enter/Space) the card, outside the download button
      // (which stops its own propagation above) — open the full-size view.
      card.addEventListener('click', () => openGraphicsFull(i));
      card.addEventListener('keydown', e => {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openGraphicsFull(i); }
      });
      gal.appendChild(card);
    });
  }

  renderGraphicsFullView();   // B10: (re)apply whichever mode (gallery/full) is active

  // 2.2.0 item C: split-view docks Graphics as a column; single-view shows it
  // as the exclusive overlay.
  if (splitOn) {
    dockSurface('graphics-overlay', true);
  } else {
    ov.classList.add('open');
    syncViewTabs();   // UX-047: Graphics tab active
  }
}

function closeGraphics() {
  _graphicsFullIndex = -1;   // B10: gallery is always the default on reopen
  // 2.2.0 item C: in split-view "close" = undock the Graphics column.
  if (splitOn) { dockSurface('graphics-overlay', false); return; }
  const ov = document.getElementById('graphics-overlay');
  if (ov) ov.classList.remove('open');
  document.getElementById('cmd-input')?.focus();
  syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
}

// ─── B10 (P8-S7): Graphics gallery full-size view + prev/next ─────────────
// Gallery stays the DEFAULT (openGraphics() above always rebuilds it first).
// Clicking a card switches to a single-graph full view driven by
// _graphicsFullIndex; renderGraphicsFullView() toggles which of
// #graphics-gallery / #graphics-full is visible and (re)builds the full-size
// SVG via renderGraph(block) — the SAME pure renderer the thumbnail already
// used, over the SAME getGraphEntries() list; a plot stored on disk is loaded
// from the gallery's own store (getGraphById). No bridge/server call either
// way ("don't re-fetch").

function renderGraphicsFullView() {
  const gal     = document.getElementById('graphics-gallery');
  const full    = document.getElementById('graphics-full');
  const actions = document.getElementById('graphics-full-actions');
  const posEl   = document.getElementById('graphics-full-pos');
  if (!gal || !full) return;
  const entries = getGraphEntries();
  const showFull = _graphicsFullIndex >= 0 && _graphicsFullIndex < entries.length;

  gal.hidden  = showFull;
  full.hidden = !showFull;
  if (actions) actions.hidden = !showFull;
  if (!showFull) return;

  const ent = entries[_graphicsFullIndex];
  const show = block => {
    const big = renderGraph(block);
    big.classList.add('graphics-full-svg');
    full.replaceChildren(big);
  };
  if (posEl) posEl.textContent = `${_graphicsFullIndex + 1} / ${entries.length}`;
  const block = getResidentGraph(ent.id);
  if (block) { show(block); return; }
  // BUG-1471: a plot stored on disk is loaded when it is opened.
  const wait = document.createElement('div');
  wait.className = 'graphics-empty';
  wait.textContent = 'Loading the plot from disk…';
  full.replaceChildren(wait);
  const idx = _graphicsFullIndex;
  getGraphById(ent.id).then(loaded => {
    if (_graphicsFullIndex !== idx || full.hidden) return;   // moved on meanwhile
    if (loaded) show(loaded);
    else wait.textContent = 'This plot could not be loaded from disk.';
  });
}

function openGraphicsFull(idx) {
  _graphicsFullIndex = idx;
  renderGraphicsFullView();
}

/** Prev/next (wraps around) — both toolbar buttons and the ArrowLeft/Right
 *  keydown wiring in init() call this. No-op with no graphs / not in full view. */
function graphicsFullStep(delta) {
  const n = getGraphEntries().length;
  if (!n || _graphicsFullIndex < 0) return;
  _graphicsFullIndex = (_graphicsFullIndex + delta + n) % n;
  renderGraphicsFullView();
}

/** "‹ Back to gallery" — returns to the grid without closing the overlay/column. */
function closeGraphicsFull() {
  _graphicsFullIndex = -1;
  renderGraphicsFullView();
}

// ─── About / Legal panel (WI-5 acknowledgements + WI-6 disclaimer) ─────────────

async function openAbout() {
  const ov = document.getElementById('about-overlay');
  if (!ov) return;
  // Mission 1: close whichever other overlay shares the console area first.
  closeAllOverlays('about-overlay');
  ov.classList.add('open');
  // UX-047: About isn't a view tab itself, but it's still an overlay — no
  // tab is active while it's open ("Results = no overlay open").
  syncViewTabs();

  // Version line (best-effort; absent in mock/browser sessions).
  const verEl = document.getElementById('about-version');
  if (verEl) {
    try {
      const v = await client.version();
      verEl.textContent = (v && v.piet_version)
        ? `PIET ${v.piet_version} · engine CoBrA ${v.cobra_version}`
        : 'PIET (no kernel connected)';
    } catch {
      verEl.textContent = 'PIET (no kernel connected)';
    }
  }

  // Third-party notices — fetched once per session, then kept on the element.
  const pre = document.getElementById('about-notices');
  if (pre && !pre.dataset.loaded) {
    const fallback = 'Third-party notices ship with the packaged app as '
      + 'Resources/THIRD_PARTY_NOTICES.txt (not available in this browser/mock session).';
    pre.textContent = 'Loading third-party notices…';
    try {
      const n = await client.legalNotices();
      if (n && n.text) {
        pre.textContent = n.text;
        pre.dataset.loaded = '1';
      } else {
        pre.textContent = fallback;
      }
    } catch {
      pre.textContent = fallback;
    }
  }
}

function closeAbout() {
  const ov = document.getElementById('about-overlay');
  if (ov) ov.classList.remove('open');
  document.getElementById('cmd-input')?.focus();
  syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
}

// ─── Split-view layout (2.2.0 item C) ──────────────────────────────────────────
// The smallest mechanism satisfying De Stijl/Boring (SPEC III-1, orthogonal
// grid; NO floating windows): #pane-console flips to a flex ROW of surface
// columns. Results is the always-present base column (#results-surface); the
// Do-file Editor / Graphics overlays DOCK beside it as equal columns (adding
// .docked, which the split CSS turns into relative flex columns). In split
// mode the view tabs become per-column toggles; "Reset layout" (one click)
// restores the default single-view tabs and clears the persisted layout.

/** Dock (or undock) a surface as a side-by-side column. Docked carries BOTH
 *  .open (so syncViewTabs lights its tab) and .docked (column geometry). */
function dockSurface(overlayId, dock) {
  const ov = document.getElementById(overlayId);
  if (!ov) return;
  ov.classList.toggle('docked', dock);
  ov.classList.toggle('open', dock);
  applySplitClass();
  persistSplit();
  syncViewTabs();
}

/** #pane-console.split reflects split mode (Results is a column even with
 *  nothing else docked, so the base geometry stays stable). */
function applySplitClass() {
  document.getElementById('pane-console')?.classList.toggle('split', splitOn);
}

/** B4 (P8-S7): apply + remember the Results/second-column ratio (percentage
 *  of #pane-console given to the Results column — the rest goes to whichever
 *  docked surface, or F10's placeholder, follows it). Mirrors panes.js's
 *  --col-vars/--col-history idiom: a CSS custom property on the flex
 *  container (piet.css §12c's #pane-console.split #results-surface rule
 *  reads it), clamped so neither side can be dragged to nothing. */
function applySplitRatio(ratio) {
  splitRatio = Math.max(SPLIT_RATIO_MIN, Math.min(SPLIT_RATIO_MAX, Number(ratio) || 50));
  document.getElementById('pane-console')?.style.setProperty('--split-ratio', `${splitRatio}%`);
}

/** BUG-113: apply + remember the doedit↔graphics weighted-grow ratio (only
 *  meaningful once BOTH are docked columns — see the CSS's #doedit-overlay.
 *  docked/#graphics-overlay.docked rules). Same idiom as applySplitRatio
 *  above, two custom properties instead of one since flex-grow weights (not
 *  a single percentage split) is the natural fit for "N columns wide". */
function applySplitWeights(w1, w2) {
  splitW1 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, Number(w1) || 1));
  splitW2 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, Number(w2) || 1));
  const pc = document.getElementById('pane-console');
  if (pc) {
    pc.style.setProperty('--split-w1', splitW1);
    pc.style.setProperty('--split-w2', splitW2);
  }
}

/** F10 (P8-S7): keep the split-view "second column" state honest and the
 *  ratio resizer wired to whichever second column is actually showing (a
 *  real docked surface, or the wait-for-a-second-view placeholder). Folded
 *  into syncViewTabs() (below) — the ONE call already made after every
 *  split-state transition (dock/undock, setSplit, closeAllOverlays, the
 *  Results-tab additive-close) — rather than hunting each call site again.
 *  BUG-113: #split-resizer-2 joins the same sweep, visible only when BOTH
 *  dockable surfaces are docked columns (the only configuration it separates). */
function syncSplitLayout() {
  const doeditDocked   = document.getElementById('doedit-overlay')?.classList.contains('docked');
  const graphicsDocked = document.getElementById('graphics-overlay')?.classList.contains('docked');
  const anyDocked = doeditDocked || graphicsDocked;
  const bothDocked = doeditDocked && graphicsDocked;
  const placeholder = document.getElementById('split-placeholder');
  const resizer = document.getElementById('split-resizer');
  const resizer2 = document.getElementById('split-resizer-2');
  if (placeholder) placeholder.hidden = !(splitOn && !anyDocked);
  if (resizer) resizer.hidden = !splitOn;
  // N5: the columns↔stack weight divider only means anything in the
  // 'columns' variant (two SIDE-BY-SIDE docked columns) — stacked panes have
  // no horizontal boundary to drag.
  if (resizer2) resizer2.hidden = !(splitOn && bothDocked && _splitVariant === 'columns');
  // N5.1: the layout-variant toggle.
  // B21-3 (User dogfood 07-16): it used to stay [hidden] until BOTH dockable
  // surfaces were already docked — the User couldn't find it because it only
  // ever appears once you've stumbled into the exact state that makes it
  // meaningful. Now it's visible for the whole splitOn lifetime; short of
  // both-docked it renders DISABLED instead of absent, with a title that
  // tells the User the one thing to do to unlock it — discoverable rather
  // than a control that only exists once you already know the trick.
  const layoutToggle = document.getElementById('layout-variant-toggle');
  if (layoutToggle) {
    layoutToggle.hidden = !splitOn;
    layoutToggle.disabled = !bothDocked;
    layoutToggle.title = bothDocked
      ? 'Stack the two docked panes vertically ↔ side-by-side columns'
      : 'Dock both Do-file Editor and Graphics (their view tabs) to stack them';
    // Label names the DESTINATION action (what clicking does), same
    // glyph+word shape as #split-toggle's own "⊟ Split" — ⬓/⬒ read as
    // "square split one way" / "the other way", matching the stack/columns
    // pairing without leaning on the ⊞/⊟ glyphs #split-toggle already owns.
    layoutToggle.textContent = _splitVariant === 'stack' ? '⬒ Columns' : '⬓ Stack';
  }
  // N5.2: swap-arm is scoped to "both docked" too — disarm if that stops
  // being true (a pane got undocked mid-arm) so a stale armed cue can't
  // linger on a bar that no longer has a swap partner.
  if (!(splitOn && bothDocked) && _swapArmedId) _disarmSwap();
}

/** N5.1: apply + remember the split-view layout variant. 'columns' (default)
 *  is the pre-N5 side-by-side geometry — #docked-stack stays display:contents
 *  (piet.css §12c-stack), byte-identical to before this wave. 'stack' turns
 *  #docked-stack into a flex column so both docked surfaces stack vertically
 *  in ONE right-hand compound column, Results staying a full-height left
 *  column (splitRatio, unchanged, still owns that boundary). */
function setSplitVariant(variant) {
  _splitVariant = (variant === 'stack') ? 'stack' : 'columns';
  document.getElementById('pane-console')?.classList.toggle('split-stack', _splitVariant === 'stack');
  const btn = document.getElementById('layout-variant-toggle');
  if (btn) btn.setAttribute('aria-pressed', _splitVariant === 'stack' ? 'true' : 'false');
  syncSplitLayout();   // resizer-2/layout-toggle visibility depend on variant too
}

function _bothDocked() {
  return DOCKABLE.every(id => document.getElementById(id)?.classList.contains('docked'));
}

/** N5.2: swap doedit-overlay ↔ graphics-overlay's DOM order inside their
 *  shared #docked-stack wrapper — flex order follows SOURCE order (no CSS
 *  `order` used) in both variants, so this one function drives the visible
 *  left/right (columns) or top/bottom (stack) swap. #split-resizer-2 rides
 *  along, kept sandwiched between the two (its own doc comment's invariant —
 *  "directly between the two elements it separates") even though it's
 *  hidden/inert in stack mode. */
function swapDockedOrder() {
  const wrap     = document.getElementById('docked-stack');
  const doedit   = document.getElementById('doedit-overlay');
  const graphics = document.getElementById('graphics-overlay');
  const resizer2 = document.getElementById('split-resizer-2');
  if (!wrap || !doedit || !graphics) return;
  // Instance access (doedit.DOCUMENT_POSITION_FOLLOWING), not the bare global
  // `Node` — Web IDL mirrors interface constants onto every instance too, and
  // this way works with no dependency on `Node` being exposed as a bare
  // global (it isn't in every headless/jsdom harness, only as window.Node —
  // real browsers/WKWebView expose both, so this is strictly more portable).
  const doeditFirst = !!(doedit.compareDocumentPosition(graphics) & doedit.DOCUMENT_POSITION_FOLLOWING);
  if (doeditFirst) {
    wrap.insertBefore(graphics, doedit);
    if (resizer2) wrap.insertBefore(resizer2, doedit);
  } else {
    wrap.insertBefore(doedit, graphics);
    if (resizer2) wrap.insertBefore(resizer2, graphics);
  }
}

/** N5.3: force the DOCKABLE ids into a specific order (restoreSplit()'s
 *  saved.order / resetLayout()'s default) — a no-op if already in that
 *  order, else exactly one swapDockedOrder() call (there are only two
 *  possible orders for a 2-element set). */
function applyDockedOrder(order) {
  if (!Array.isArray(order) || order.length !== 2) return;
  const [firstId, secondId] = order;
  const first  = document.getElementById(firstId);
  const second = document.getElementById(secondId);
  if (!first || !second) return;
  const currentlyFirst = !!(first.compareDocumentPosition(second) & first.DOCUMENT_POSITION_FOLLOWING);   // instance access — see swapDockedOrder()'s note
  if (!currentlyFirst) swapDockedOrder();
}

/** N5.2: arm swap mode on `overlayId`'s .doedit-bar (visual cue), or —
 *  clicked again, or the OTHER docked pane clicked — resolve it (disarm, or
 *  disarm-and-swap respectively). Only meaningful while split is on with
 *  BOTH surfaces docked (mirrors the layout-toggle's own visibility gate);
 *  a stray click while that's not true is a silent no-op. */
function _onDockedTitleClick(overlayId, e) {
  e.stopPropagation();   // this IS the gesture — the document click-elsewhere
                          // listener (init()) must not ALSO see it and re-disarm.
  if (!splitOn || !_bothDocked()) return;
  if (_swapArmedId === null) {
    _armSwap(overlayId);
  } else if (_swapArmedId === overlayId) {
    _disarmSwap();
  } else {
    swapDockedOrder();
    persistSplit();
    _disarmSwap();
  }
}

function _armSwap(overlayId) {
  _swapArmedId = overlayId;
  document.getElementById(overlayId)?.querySelector('.doedit-bar')?.classList.add('swap-armed');
}

function _disarmSwap() {
  if (_swapArmedId) {
    document.getElementById(_swapArmedId)?.querySelector('.doedit-bar')?.classList.remove('swap-armed');
  }
  _swapArmedId = null;
}

/** B4 (P8-S7): wire the draggable divider between Results and the second
 *  column. Same pointerdown → pointermove (RAF-coalesced) → pointerup shape
 *  as panes.js's _bindDrag(), adapted to a clamped percentage instead of a
 *  clamped pixel width. */
function initSplitResizer() {
  const handle = document.getElementById('split-resizer');
  const paneConsole = document.getElementById('pane-console');
  if (!handle || !paneConsole) return;
  handle.addEventListener('pointerdown', e => {
    e.preventDefault();
    handle.setPointerCapture(e.pointerId);
    handle.classList.add('dragging');
    const startX = e.clientX;
    const startRatio = splitRatio;
    const width = paneConsole.clientWidth || 1;
    let raf = null;
    let pending = null;

    function onMove(ev) {
      const deltaPct = ((ev.clientX - startX) / width) * 100;
      pending = Math.max(SPLIT_RATIO_MIN, Math.min(SPLIT_RATIO_MAX, startRatio + deltaPct));
      if (raf) return;
      raf = requestAnimationFrame(() => { raf = null; applySplitRatio(pending); });
    }

    function onUp() {
      if (raf) { cancelAnimationFrame(raf); raf = null; }
      if (pending != null) applySplitRatio(pending);
      persistSplit();
      handle.classList.remove('dragging');
      handle.removeEventListener('pointermove', onMove);
      handle.removeEventListener('pointerup', onUp);
    }

    handle.addEventListener('pointermove', onMove);
    handle.addEventListener('pointerup', onUp);
  });
}

/** BUG-113: wire the draggable divider between the two docked columns
 *  (doedit ↔ graphics), when both are showing. Same pointerdown →
 *  pointermove (RAF-coalesced) → pointerup shape as initSplitResizer above,
 *  adapted to a pair of clamped flex-grow weights instead of one clamped
 *  percentage — the px-delta is converted to a weight delta at whatever rate
 *  the two columns are CURRENTLY rendering at (captured at drag-start), so
 *  the divider tracks the cursor 1:1 regardless of the columns' starting size. */
function initSplitResizer2() {
  const handle = document.getElementById('split-resizer-2');
  const col1 = document.getElementById('doedit-overlay');
  const col2 = document.getElementById('graphics-overlay');
  if (!handle || !col1 || !col2) return;
  handle.addEventListener('pointerdown', e => {
    e.preventDefault();
    handle.setPointerCapture(e.pointerId);
    handle.classList.add('dragging');
    const startX = e.clientX;
    const startW1 = splitW1;
    const startW2 = splitW2;
    const pxTotal = (col1.getBoundingClientRect().width || 1) + (col2.getBoundingClientRect().width || 1);
    const weightTotal = startW1 + startW2 || 1;
    const pxPerWeight = pxTotal / weightTotal || 1;
    let raf = null;
    let pending = null;

    function onMove(ev) {
      const deltaWeight = (ev.clientX - startX) / pxPerWeight;
      const w1 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, startW1 + deltaWeight));
      const w2 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, startW2 - deltaWeight));
      pending = [w1, w2];
      if (raf) return;
      raf = requestAnimationFrame(() => { raf = null; applySplitWeights(pending[0], pending[1]); });
    }

    function onUp() {
      if (raf) { cancelAnimationFrame(raf); raf = null; }
      if (pending != null) applySplitWeights(pending[0], pending[1]);
      persistSplit();
      handle.classList.remove('dragging');
      handle.removeEventListener('pointermove', onMove);
      handle.removeEventListener('pointerup', onUp);
    }

    handle.addEventListener('pointermove', onMove);
    handle.addEventListener('pointerup', onUp);
  });
}

/** Enter/leave split mode. Entering docks any dockable that was already open
 *  (so an open editor becomes a column); leaving undocks every dockable. */
function setSplit(on) {
  splitOn = on;
  document.getElementById('split-toggle')?.setAttribute('aria-pressed', on ? 'true' : 'false');
  const resetBtn = document.getElementById('reset-layout');
  if (resetBtn) resetBtn.hidden = !on;
  if (on) {
    for (const id of DOCKABLE) {
      if (_isOverlayOpen(id)) document.getElementById(id)?.classList.add('docked');
    }
  } else {
    for (const id of DOCKABLE) {
      document.getElementById(id)?.classList.remove('docked', 'open');
    }
  }
  applySplitClass();
  syncViewTabs();
}

/** Persist {on, docked:[…], ratio, ratio2, layout, order} in localStorage;
 *  off clears the key so a restart boots single-view (Reset-clears-
 *  persistence, User: recovery must be easy). B4 (P8-S7): `ratio` joins the
 *  SAME blob rather than a new key. BUG-113: `ratio2` (the doedit↔graphics
 *  weight pair) does too. N5.3: `layout` ('columns'|'stack') and `order`
 *  (DOCKABLE ids in their current DOM order — click-swap, N5.2) ride the
 *  same blob. markUiStateDirty() mirrors the whole SPLIT_KEY value into
 *  ui_state (N2) on every call, including the off/removeItem branch. */
function persistSplit() {
  try {
    if (!splitOn) { localStorage.removeItem(SPLIT_KEY); markUiStateDirty(); return; }
    const docked = DOCKABLE.filter(id => document.getElementById(id)?.classList.contains('docked'));
    // N5.3: current DOM order of the two dockable ids — compareDocumentPosition
    // is the source of truth (swapDockedOrder() actually MOVES the nodes;
    // this just reads back whatever order resulted), so a page reload
    // restores whichever pane the user last swapped to the front.
    const order = [...DOCKABLE].sort((a, b) => {
      const ea = document.getElementById(a), eb = document.getElementById(b);
      if (!ea || !eb) return 0;
      return (ea.compareDocumentPosition(eb) & ea.DOCUMENT_POSITION_FOLLOWING) ? -1 : 1;   // instance access — see swapDockedOrder()'s note
    });
    localStorage.setItem(SPLIT_KEY, JSON.stringify({
      on: true, docked, ratio: splitRatio, ratio2: { w1: splitW1, w2: splitW2 },
      layout: _splitVariant, order,
    }));
    markUiStateDirty();
  } catch { /* storage denied/quota — non-fatal, layout just won't persist */ }
}

/** Restore a persisted split layout at boot (called from init()). */
function restoreSplit() {
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem(SPLIT_KEY) || 'null'); } catch { saved = null; }
  if (!saved || !saved.on) return;
  // B4: apply the saved ratio BEFORE the layout becomes visible (setSplit
  // below), so there's no flash of the 50/50 default.
  if (Number.isFinite(saved.ratio)) applySplitRatio(saved.ratio);
  // BUG-113: same "apply before visible" treatment for the second ratio.
  if (saved.ratio2 && Number.isFinite(saved.ratio2.w1) && Number.isFinite(saved.ratio2.w2)) {
    applySplitWeights(saved.ratio2.w1, saved.ratio2.w2);
  }
  // N5.3: variant BEFORE setSplit()/dockSurface() below make anything
  // visible, same "no flash of the default" treatment as the ratios above.
  setSplitVariant(saved.layout === 'stack' ? 'stack' : 'columns');
  setSplit(true);
  for (const id of (saved.docked || [])) {
    if (DOCKABLE.includes(id)) {
      if (id === 'graphics-overlay') openGraphics();   // rebuilds the gallery + docks
      else dockSurface(id, true);
    }
  }
  // N5.3: only meaningful once both are actually docked (a lone docked
  // surface has no "order" to speak of) — applyDockedOrder() itself is a
  // no-op if either id is missing/undocked.
  if (Array.isArray(saved.order) && saved.order.length === 2 &&
      DOCKABLE.every(id => saved.docked?.includes(id))) {
    applyDockedOrder(saved.order);
  }
}

/** Reset layout: leave split, restore single-view tabs, clear persistence
 *  (B4: including the drag ratio, back to 50/50; N5.4: including the layout
 *  variant and click-swap order, back to columns/default-order). */
function resetLayout() {
  setSplit(false);
  // N5.4: back to the default variant + source order — before persistSplit()
  // below so the cleared blob (splitOn=false -> removeItem) never even
  // briefly recorded the pre-reset variant/order.
  setSplitVariant('columns');
  applyDockedOrder(DOCKABLE);
  persistSplit();   // splitOn=false -> removes the key
  // U16: also drop the persisted pane-width overrides (panes.js reads these
  // at boot) — otherwise "Reset layout" only undid the split, leaving a
  // dragged Variables/History column exactly as wide as before. Clear the
  // live inline override too so the reset is immediate, not just next-load.
  localStorage.removeItem('piet-col-vars');
  localStorage.removeItem('piet-col-history');
  markUiStateDirty();   // N2: mirror the clear into ui_state too
  const app = document.getElementById('app');
  if (app) {
    app.style.removeProperty('--col-vars');
    app.style.removeProperty('--col-history');
  }
  // B4: back to 50/50 — clear the live inline override too, same immediate-
  // effect precedent as --col-vars/--col-history above.
  splitRatio = 50;
  document.getElementById('pane-console')?.style.removeProperty('--split-ratio');
  // BUG-113: back to 1:1 for the doedit↔graphics weight pair too.
  splitW1 = 1;
  splitW2 = 1;
  const pc = document.getElementById('pane-console');
  if (pc) {
    pc.style.removeProperty('--split-w1');
    pc.style.removeProperty('--split-w2');
  }
  document.getElementById('cmd-input')?.focus();
}

/** A view-tab click. Single-view: open the surface (exclusive overlay).
 *  Split-view: toggle that surface's docked column on/off. */
function onViewTabClick(overlayId, openFn, closeFn) {
  if (splitOn && DOCKABLE.includes(overlayId)) {
    // BUG-095 (P8.1-T1): a view-tab gesture always dismisses the full-surface
    // panels (Settings/About) even in split mode — this branch used to bypass
    // closeAllOverlays entirely, leaving Settings stranded on top (the
    // Results tab's own handler already does this sweep; mirror it here).
    for (const o of OVERLAYS) { if (!DOCKABLE.includes(o.id)) o.close(); }
    const docked = document.getElementById(overlayId)?.classList.contains('docked');
    if (docked) closeFn(); else openFn();
    return;
  }
  openFn();
}

// ─── First-run consent dialog (WS-B, extcompat 2026-08-06) ──────────────────
// macOS ships as a drag-install, so there is no installer step to carry the
// non-affiliation + GPL notice; this dialog is it, and it also collects the
// one real choice behind it (the external-library compatibility names).
//
// THREE conditions must ALL hold before it can paint, and each one is a
// separate way an automated run could otherwise be stalled by a modal:
//   · a native bridge exists — a mock/browser session (the DOM runner) has no
//     durable store to record consent in, so it is never asked;
//   · settings.get answered at all — a failed probe fails OPEN (no dialog),
//     because a bridge hiccup must never wedge the app behind an unanswerable
//     question;
//   · native said `consent_required` — computed in settings.cpp, and always
//     false under $PIET_SETTINGS_FILE isolation. THAT is the test-mode
//     auto-accept: --ui-smoke and every headless harness set that variable
//     already, so the decision is made natively, before any JS runs, rather
//     than by a flag the product build could accidentally honour.
function maybeShowConsent(s) {
  if (!client || !client.native) return;
  if (!s || s.consent_required !== true) return;
  const ov = document.getElementById('consent-overlay');
  if (!ov) return;
  const box = document.getElementById('consent-extcompat');
  // Default ON — the dialog opens with existing do-files still working, and
  // the user opts OUT rather than having to opt in to what already shipped.
  if (box) box.checked = s.extcompat !== false;
  ov.classList.add('open');
  document.getElementById('consent-continue')?.focus();
}

/** Continue — persist {extcompat, extcompat_consent_at} and dismiss. The
 *  timestamp is what suppresses the dialog on every later launch, so it is
 *  written even when the box is left at its default. A failed write leaves the
 *  dialog up with an honest message rather than silently dropping the choice:
 *  dismissing it would record consent that never reached disk, and the dialog
 *  would reappear next launch anyway. */
async function acceptConsent() {
  const status = document.getElementById('consent-status');
  const btn = document.getElementById('consent-continue');
  const on = !!document.getElementById('consent-extcompat')?.checked;
  if (status) status.textContent = '';
  if (btn) btn.disabled = true;
  try {
    const partial = { extcompat: on, extcompat_consent_at: new Date().toISOString() };
    await client.setSettings(partial);
    _settingsCache = { ..._settingsCache, ...partial, consent_required: false };
    document.getElementById('consent-overlay')?.classList.remove('open');
    document.getElementById('cmd-input')?.focus();
  } catch (err) {
    if (status) status.textContent = '⚠ Could not save your choice: '
      + String(err && err.message ? err.message : err);
  } finally {
    if (btn) btn.disabled = false;
  }
}

// ─── Settings panel (2.2.0 item D) ─────────────────────────────────────────────
// Two groups: engine/scratch (persisted natively via settings.get/settings.set;
// restart-to-apply where noted) and display (font size, paste-autorun mirror —
// localStorage, applied live). Apply writes the native group; the display group
// applies on change.

async function openSettings() {
  const ov = document.getElementById('settings-overlay');
  if (!ov) return;
  closeAllOverlays('settings-overlay');
  ov.classList.add('open');
  syncViewTabs();   // Settings is not a view tab (like About) — no tab lights up.
  await populateSettings();
  document.getElementById('set-scratch-path')?.focus();
}

function closeSettings() {
  const ov = document.getElementById('settings-overlay');
  if (ov) ov.classList.remove('open');
  document.getElementById('cmd-input')?.focus();
  syncViewTabs();
}

/** Load persisted settings + detected system info into the panel. Best-effort:
 *  a mock/browser session answers from navigator.* (api.js) so it still fills. */
async function populateSettings() {
  const status = document.getElementById('settings-status');
  if (status) status.textContent = '';
  let s = null;
  try { s = await client.getSettings(); } catch { s = null; }
  _settingsCache = s;   // N1: keep composeDefaultPath() current with whatever's actually persisted
  const sys = (s && s.sys) || { total_ram_mb: 0, cpu_count: 1, recommended_budget_mb: 2048 };

  const sysinfo = document.getElementById('settings-sysinfo');
  if (sysinfo) {
    sysinfo.textContent = `Detected: ${sys.cpu_count} cores · ${sys.total_ram_mb} MB RAM · `
      + `recommended budget ${sys.recommended_budget_mb} MB`
      + (s && s.settings_file ? `  ·  ${s.settings_file}` : '');
  }

  const scratch = document.getElementById('set-scratch-path');
  if (scratch) scratch.value = (s && s.scratch_path) || '';
  const workdir = document.getElementById('set-workdir');
  if (workdir) workdir.value = (s && s.default_working_dir) || '';
  // N1: "Export folder" — default_export_dir (contract_B v2.4.0, additive).
  // Empty = ask every time (native NSSavePanel); see composeDefaultPath().
  const exportDir = document.getElementById('set-export-dir');
  if (exportDir) exportDir.value = (s && s.default_export_dir) || '';

  const mem = document.getElementById('set-mem-budget');
  const memOut = document.getElementById('set-mem-value');
  const memRec = document.getElementById('set-mem-recommended');
  if (mem) {
    if (sys.total_ram_mb && sys.total_ram_mb > Number(mem.max)) mem.max = String(sys.total_ram_mb);
    const cur = (s && s.memory_budget_mb) ? s.memory_budget_mb : sys.recommended_budget_mb;
    mem.value = String(cur);
    if (memOut) memOut.textContent = `${cur} MB`;
    // P7 carry-over: keep console.js's Results-console scrollback cap
    // following the user's real configured (or recommended, pre-first-save)
    // budget instead of console.js's hardcoded 2048 default.
    setMemoryBudgetMb(cur);
    setGridMemoryBudgetMb(cur);  // SPEC II-3: grid cache/prefetch depth too
  }
  if (memRec) {
    // U12: name the real consumers instead of a vague label.
    memRec.textContent = `Handed to the engine as its memory budget (bounds parallel scratch), caps the Results `
      + `console's scrollback and graph gallery, and bounds the automatic parallel worker count (SPEC II-2). `
      + `Recommended: ${sys.recommended_budget_mb} MB.`;
  }

  const cores = document.getElementById('set-cores');
  if (cores) {
    const want = (s && s.compute_cores) || 0;
    cores.replaceChildren();
    const auto = document.createElement('option');
    auto.value = '0'; auto.textContent = 'Auto (all available)';
    cores.appendChild(auto);
    const n = Math.max(1, sys.cpu_count || 1);
    for (let i = 1; i <= n; i++) {
      const o = document.createElement('option');
      o.value = String(i); o.textContent = String(i);
      cores.appendChild(o);
    }
    cores.value = String(want);
  }

  const prov = document.getElementById('set-provenance');
  if (prov) prov.checked = !!(s && s.provenance_hash);

  // WS-B (extcompat): default TRUE, so an absent field (a mock session, or a
  // store written before this build) shows the names as enabled — which is
  // what the engine actually does in that state.
  const ext = document.getElementById('set-extcompat');
  if (ext) ext.checked = !s || s.extcompat !== false;

  const fsSel = document.getElementById('set-fontsize');
  if (fsSel) fsSel.value = localStorage.getItem('piet-font-size') || 'medium';
  const pa = document.getElementById('set-paste-autorun');
  if (pa) pa.checked = localStorage.getItem('piet-paste-autorun') === '1';
  const as = document.getElementById('set-auto-summarize');
  if (as) as.checked = localStorage.getItem('piet-auto-summarize') === '1';
  const fst = document.getElementById('set-follow-system-theme');
  if (fst) fst.checked = localStorage.getItem('piet-theme-follow-system') === '1';
}

/** Apply — persist the engine/scratch group natively (settings.set). */
async function applySettings() {
  const status = document.getElementById('settings-status');
  const partial = {
    scratch_path: document.getElementById('set-scratch-path')?.value ?? '',
    default_working_dir: document.getElementById('set-workdir')?.value ?? '',
    // N1: additive field, passes straight through settings.set (api.js) —
    // empty string means "ask every time" (composeDefaultPath() below).
    default_export_dir: document.getElementById('set-export-dir')?.value ?? '',
    memory_budget_mb: Number(document.getElementById('set-mem-budget')?.value ?? 0) || 0,
    compute_cores: Number(document.getElementById('set-cores')?.value ?? 0) || 0,
    provenance_hash: !!document.getElementById('set-provenance')?.checked,
    // WS-B (extcompat): the durable half of the choice. settings.set republishes
    // it to the dispatch gate's cache, so it takes effect on the next command
    // rather than at the next launch.
    extcompat: !!document.getElementById('set-extcompat')?.checked,
  };
  try {
    await client.setSettings(partial);
    // N1: keep the save-time cache current so a save right after Apply (no
    // reopen of Settings in between) sees the new export folder immediately.
    _settingsCache = { ..._settingsCache, ...partial };
    if (status) status.textContent = 'Saved. Compute cores apply to the next command; '
      + 'scratch directory & working directory apply on restart.';
  } catch (err) {
    if (status) status.textContent = '⚠ Could not save settings: '
      + String(err && err.message ? err.message : err);
  }
}

/** N1: default save-folder path for a given filename, or undefined (→ the
 *  caller falls back to a native NSSavePanel). Reads the settings cache
 *  SYNCHRONOUSLY (populated at boot by hydrateUiState() and refreshed by
 *  populateSettings()/applySettings()) rather than a fresh bridge round trip
 *  on every save click. */
function composeDefaultPath(name) {
  const dir = _settingsCache && _settingsCache.default_export_dir;
  return dir ? `${dir.replace(/[/\\]+$/, '')}/${name}` : undefined;
}

/** Font size (display setting) — drives the --fs-* tokens via <html
 *  data-fontsize>; 'medium' = no attribute (base tokens, default DOM unchanged). */
function applyFontSize(size) {
  const root = document.documentElement;
  if (size === 'small' || size === 'large') {
    root.dataset.fontsize = size;
    localStorage.setItem('piet-font-size', size);
  } else {
    delete root.dataset.fontsize;
    localStorage.setItem('piet-font-size', 'medium');
  }
  markUiStateDirty();   // N2
}

/** Paste-autorun (display setting) — persisted + mirrored across the two
 *  checkboxes (do-editor header ↔ Settings panel). */
function setPasteAutorun(on) {
  localStorage.setItem('piet-paste-autorun', on ? '1' : '0');
  markUiStateDirty();   // N2
  const a = document.getElementById('paste-autorun-toggle');
  const b = document.getElementById('set-paste-autorun');
  if (a) a.checked = on;
  if (b) b.checked = on;
}

/** B14(b) — "Auto-summarize on load" (display setting). Persisted in
 *  localStorage 'piet-auto-summarize' ('1'/'0'; absent = OFF), the exact same
 *  idiom as 'piet-paste-autorun' above — a single Settings-panel checkbox, no
 *  native settings.set bridge call. See scheduleAutoSummarize()/
 *  runAutoSummarize() (near runCommand()) for the behavior this gates. */
function setAutoSummarize(on) {
  localStorage.setItem('piet-auto-summarize', on ? '1' : '0');
  markUiStateDirty();   // N2
  const el = document.getElementById('set-auto-summarize');
  if (el) el.checked = on;
}

// ─── Follow system appearance (D5) ───────────────────────────────────────────
// Light/Dark can track window.matchMedia('(prefers-color-scheme: dark)')
// instead of the manually-persisted 'piet-theme' choice — no native code
// (matchMedia is a standard webview API). Persisted in localStorage
// 'piet-theme-follow-system' ('1'/'0'; absent = OFF), same idiom as
// 'piet-auto-summarize' above. index.html's pre-paint <head> script applies
// it before first paint (zero FOUC, mirrors piet-mode/piet-theme); this
// module only owns the LIVE follow (the matchMedia 'change' listener,
// registered once in init()) plus the Settings checkbox + manual-override path.

/** Sync <html data-theme> + #theme-toggle's label to the OS's current
 *  prefers-color-scheme. Shared by the matchMedia change listener and
 *  setFollowSystemTheme(true)'s immediate-apply. Does NOT touch the
 *  'piet-theme' localStorage key — follow-system stays authoritative on the
 *  next boot regardless of what it was last manually set to. */
function applySystemTheme() {
  beginThemeFade();   // A7: arm the transition before the recolor below
  const html = document.documentElement;
  const prefersDark = !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
  if (prefersDark) html.dataset.theme = 'dark';
  else delete html.dataset.theme;
  syncThemeButtons();   // BUG-114: both labels, not just #theme-toggle
}

function setFollowSystemTheme(on) {
  localStorage.setItem('piet-theme-follow-system', on ? '1' : '0');
  markUiStateDirty();   // N2
  const el = document.getElementById('set-follow-system-theme');
  if (el) el.checked = on;
  if (on) applySystemTheme();
}

// ─── Overlay registry (design decision 5 / A2-05) ──────────────────────────────
// The four console overlays (browser/doedit/graphics/about) share the console
// area (piet.css §12b's .console-overlay). This is the ONE place that knows
// about all four, replacing the ad hoc per-function close subsets that used
// to live inline in each openX() (app.js:479-481, 527-530, 572-575 pre-W2).
//
// #browser-overlay is the one exception: grid.js (WS-F2, frozen to WS-F1)
// owns that container's visibility via show()/hide() (toggling the `hidden`
// DOM property, not a class) — see index.html's comment on #browser-overlay.
// isOpen()/close() below dispatch to grid's own API for that one entry so the
// registry still treats it uniformly for closeAllOverlays() and the unified
// Escape handler.
const OVERLAYS = [
  {
    id: 'browser-overlay',
    isOpen: () => !!grid && !grid._container.hidden,
    close:  () => { if (grid && !grid._container.hidden) grid.hide(); },
  },
  { id: 'doedit-overlay',   isOpen: () => _isOverlayOpen('doedit-overlay'),   close: closeDoEditor },
  { id: 'graphics-overlay', isOpen: () => _isOverlayOpen('graphics-overlay'), close: closeGraphics },
  { id: 'about-overlay',    isOpen: () => _isOverlayOpen('about-overlay'),    close: closeAbout },
  // 2.2.0 item D: Settings joins the registry so Escape + closeAllOverlays +
  // the tab-sync sweep treat it uniformly (like About, it lights no view tab).
  { id: 'settings-overlay', isOpen: () => _isOverlayOpen('settings-overlay'), close: closeSettings },
];

function _isOverlayOpen(id) {
  return !!document.getElementById(id)?.classList.contains('open');
}

/** Close every console overlay except `exceptId` (if given). Used by every openX(). */
function closeAllOverlays(exceptId) {
  for (const o of OVERLAYS) {
    if (o.id !== exceptId) o.close();
  }
  syncViewTabs();   // UX-047: reconcile the view tabs after the sweep
}

// ─── View tabs (UX-047) ─────────────────────────────────────────────────────
// The toolbar's RIGHT group (Results / Do-file Editor / Graphics — segmented,
// .toolbar-views/.view-tab in piet.css) mirrors which surface is currently
// showing. Results = no overlay open at all (the Data Browser/Editor opens
// via Data ▾, not a view tab, so it deliberately clears the tab row rather
// than lighting one up — same "no overlay open" rule the About panel uses).
// Single source of truth: the OVERLAYS registry above, so this stays correct
// for browser-overlay too without duplicating grid.js's own hidden-attribute
// check. Called from every openX()/closeX() above, from closeAllOverlays()
// (covers the Results tab's own click handler), from init()'s boot call, and
// from the MutationObserver "grid show/hide adapter" registered in init()
// (grid.js toggles #browser-overlay's `hidden` attribute directly via its own
// Back button + internal Escape handler, bypassing the registry's close()).
function syncViewTabs() {
  const anyOpen     = OVERLAYS.some(o => o.isOpen());
  const doeditOpen   = _isOverlayOpen('doedit-overlay');
  const graphicsOpen = _isOverlayOpen('graphics-overlay');
  const resultsTab   = document.querySelector('.view-tab[data-view="results"]');
  const doeditTab    = document.querySelector('.view-tab[data-view="doedit"]');
  const graphicsTab  = document.querySelector('.view-tab[data-view="graphics"]');
  // BUG-096 (P8.1-T1): in split mode the Results column is ALWAYS present —
  // docked editor/graphics columns must not douse its underline ("underline
  // marks every currently-visible surface"). Only a FULL-surface overlay
  // (Data Browser / About / Settings — non-dockable) actually covers it.
  if (resultsTab) {
    const covered = OVERLAYS.some(o => o.isOpen() && !DOCKABLE.includes(o.id));
    resultsTab.classList.toggle('active', splitOn ? !covered : !anyOpen);
  }
  if (doeditTab)   doeditTab.classList.toggle('active', doeditOpen);
  if (graphicsTab) graphicsTab.classList.toggle('active', graphicsOpen);
  // F10/B4 (P8-S7): every split-state transition already ends up here (dock/
  // undock, setSplit, closeAllOverlays, the Results-tab additive-close) — one
  // hook keeps the "second column" placeholder/resizer honest without
  // duplicating this call at each site.
  syncSplitLayout();
}

/** N1: Blob -> base64 (no data: prefix) for client.saveFile()'s content_b64
 *  field — the FileReader idiom, since a webview has no Buffer/btoa-on-
 *  arbitrary-bytes shortcut for binary blobs (btoa needs a binary STRING,
 *  which is exactly what readAsDataURL's base64 half already is). */
function _blobToBase64(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      const result = reader.result || '';
      const idx = result.indexOf(',');
      resolve(idx >= 0 ? result.slice(idx + 1) : result);
    };
    reader.onerror = () => reject(reader.error || new Error('FileReader failed'));
    reader.readAsDataURL(blob);
  });
}

/** Serialize an inline <svg> → PNG and save it. N1 (P8.1-W2): native bridge
 *  present -> real save via client.saveFile() (Settings' export folder, or a
 *  native NSSavePanel when unset). No bridge (browser/mock) -> the original
 *  client-side Blob→URL.createObjectURL→<a download> path, UNCHANGED. */
function svgToPng(svgEl, filename) {
  try {
    // viewBox "0 0 W H" gives the natural pixel size; fall back to 700x440 (graph.js).
    const vb = (svgEl.getAttribute('viewBox') || '0 0 700 440').split(/\s+/).map(Number);
    const W = vb[2] || 700, H = vb[3] || 440;
    // The graph <svg> is themed entirely through piet.css classes (.g-frame,
    // .g-point, .g-ticklabel, …) and CSS variables — a serialized <svg> carries
    // NO external stylesheet, so once it is rasterized standalone (data: URL →
    // <img> → canvas) every class-based fill/stroke collapses to the SVG default
    // (black): the .g-frame panel fills solid black and the points disappear
    // into it (reported on Windows, but the raster loses the CSS on every
    // platform). Flatten the resolved paint props onto a clone — getComputedStyle
    // must read the LIVE node, so read from svgEl and write to the clone — and
    // bake the themed background (dark mode paints .g-svg via CSS `background`,
    // also dropped) as a real rect, so the PNG matches what is on screen.
    const clone = svgEl.cloneNode(true);
    const src = [svgEl, ...svgEl.querySelectorAll('*')];
    const dst = [clone, ...clone.querySelectorAll('*')];
    const PAINT = ['fill', 'stroke', 'stroke-width', 'stroke-dasharray', 'stroke-linejoin',
                   'stroke-linecap', 'opacity', 'fill-opacity', 'stroke-opacity',
                   'font-size', 'font-family', 'font-weight', 'text-anchor'];
    for (let i = 0; i < src.length && i < dst.length; i++) {
      const cs = getComputedStyle(src[i]);
      for (const p of PAINT) {
        const v = cs.getPropertyValue(p);
        if (v) dst[i].style.setProperty(p, v);
      }
    }
    const bg = getComputedStyle(svgEl).backgroundColor;
    if (bg && bg !== 'transparent' && bg !== 'rgba(0, 0, 0, 0)') {
      const r = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
      r.setAttribute('x', '0'); r.setAttribute('y', '0');
      r.setAttribute('width', String(W)); r.setAttribute('height', String(H));
      r.setAttribute('fill', bg);
      clone.insertBefore(r, clone.firstChild);
    }
    const xml = new XMLSerializer().serializeToString(clone);
    const svg64 = btoa(unescape(encodeURIComponent(xml)));
    const img = new Image();
    img.onload = () => {
      const scale = 2;  // crisp export
      const canvas = document.createElement('canvas');
      canvas.width = W * scale; canvas.height = H * scale;
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
      canvas.toBlob(async blob => {
        if (!blob) return;
        // N1: real save when a native bridge exists — feature-detected so a
        // plain browser tab / the DOM-fixture mock keeps the anchor-download
        // path below byte-unchanged.
        if (client && client.native) {
          try {
            const content_b64 = await _blobToBase64(blob);
            const res = await client.saveFile({
              suggested_name: filename, kind: 'png', content_b64,
              path: composeDefaultPath(filename),
            });
            if (res && res.ok) flashStatus(`Saved ${res.path}`);
            // {ok:false, cancelled:true} (user dismissed NSSavePanel) — quiet no-op.
          } catch (err) {
            appendBlock({ type: 'error', code: 'engine_error', message: `PNG export failed: ${err.message ?? err}` });
          }
          return;
        }
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url; a.download = filename;
        a.click();
        URL.revokeObjectURL(url);
      }, 'image/png');
    };
    img.src = 'data:image/svg+xml;base64,' + svg64;
  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: `PNG export failed: ${err.message ?? err}` });
  }
}

// ─── Clear ───────────────────────────────────────────────────────────────────

function onClear() {
  clearConsole();
}

// ─── B2 (P8-S6): Save Results ──────────────────────────────────────────────

/**
 * Serialize the results log (#console-body's rendered blocks) to a
 * plain-text .log and save it. N1 (P8.1-W2): native bridge present -> real
 * save via client.saveFile() (Settings' export folder, or a native
 * NSSavePanel when unset); the OLD unconditional "Saved" flash dies on that
 * path — it now fires ONLY on a confirmed {ok:true}, never on a cancelled
 * dialog. No bridge (browser/mock) -> the original Blob → URL.
 * createObjectURL → temp <a download> → revoke path, UNCHANGED — that flash
 * stays unconditional there because it always WAS a real save (the browser's
 * own download), never a fabricated claim.
 */
async function onSaveResults() {
  const body = document.getElementById('console-body');
  // Walk the block children (not body.textContent directly) so each block
  // gets a blank-line separator — plain textContent concatenation across
  // sibling <div>s runs everything together with no line breaks.
  const blocks = body ? Array.from(body.children).filter(el => el.classList.contains('block')) : [];
  // A10 (P8.1): strip UI action bars (Copy table / Export CSV — marked
  // .console-action, incl. the pre-existing esttab bar) from the saved text;
  // they are chrome, not results. Clone-and-remove so the live DOM is untouched.
  const blockText = (b) => {
    if (!b.querySelector('.console-action')) return b.textContent;
    const clone = b.cloneNode(true);
    clone.querySelectorAll('.console-action').forEach(el => el.remove());
    return clone.textContent;
  };
  const text = blocks.map(blockText).join('\n\n').trim();
  if (!text) {
    flashStatus('Nothing to save — the results log is empty');
    return;
  }
  const d = new Date();
  const pad = n => String(n).padStart(2, '0');
  const stamp = `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
  const filename = `piet-results-${stamp}.log`;

  if (client && client.native) {
    try {
      const res = await client.saveFile({
        suggested_name: filename, kind: 'log', text: text + '\n',
        path: composeDefaultPath(filename),
      });
      if (res && res.ok) flashStatus(`Saved ${res.path}`);
      // {ok:false, cancelled:true} (user dismissed NSSavePanel) — quiet no-op.
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: `Save Results failed: ${err.message ?? err}` });
    }
    return;
  }

  const blob = new Blob([text + '\n'], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);

  flashStatus(`Saved ${filename}`);
}

// ─── Status bar ───────────────────────────────────────────────────────────────

// N2(b) (P8.1-W2): last file we retitled the native window for — updateStatus()
// runs far more often than the file identity actually changes (every grid
// cell commit, every mutation refresh), so this guards setWindowTitle()
// against redundant fire-and-forget calls. `undefined` (distinct from the
// no-file '' currentFile) guarantees exactly one call at boot too.
let _lastTitledFile = undefined;

function updateStatus() {
  const fileEl   = document.getElementById('status-file');
  const dimsEl   = document.getElementById('status-dims');
  const tbFileEl = document.getElementById('toolbar-filename');
  // U1: the statusbar reads "<cwd> · <file> · N vars, M obs · ●" — cwd and
  // file are both derived from the one tracked state.currentFile path (no
  // separate "current directory" field exists on the wire — CONTRACT B only
  // adds source_file/filepath); the toolbar strip (tbFileEl) stays
  // filename-only, unchanged.
  let dir = '', fname = '';
  if (state.currentFile) {
    const parts = state.currentFile.split(/[/\\]/);
    fname = parts.pop() || '';
    dir   = parts.join('/');
  }
  if (fileEl)   fileEl.textContent   = state.currentFile ? [dir, fname].filter(Boolean).join(' · ') : '(no data)';
  if (tbFileEl) tbFileEl.textContent = fname;
  if (dimsEl) {
    // A1-05: key the blank fallback off state.currentFile (like fname above),
    // not state.nRows — a genuinely-loaded 0-observation dataset used to show
    // blank dims instead of "N vars, 0 obs".
    dimsEl.textContent = state.currentFile
      ? `${state.nCols} vars, ${state.nRows.toLocaleString('en-US')} obs`
      : '';
  }
  // N2(b): native window title — 'PIET — <basename>', or bare 'PIET' with no
  // file loaded. Feature-detected (no bridge -> no window chrome to retitle,
  // mock/browser stays exactly as before) and fire-and-forget (a failed
  // retitle is cosmetic only, never surfaced as an error block).
  if (client && client.native && state.currentFile !== _lastTitledFile) {
    _lastTitledFile = state.currentFile;
    const title = fname ? `PIET — ${fname}` : 'PIET';
    client.setWindowTitle(title).catch(() => { /* cosmetic only */ });
  }
}

// B2 (P8-S6): transient status-bar confirmation (W10 — no modal). One owner
// (#status-flash), self-clearing; a new call replaces whatever is showing
// rather than queuing. Currently used by onSaveResults() only, but general
// enough for future lightweight confirmations to reuse.
let _statusFlashTimer = null;
function flashStatus(msg, ms = 2500) {
  const el = document.getElementById('status-flash');
  if (!el) return;
  clearTimeout(_statusFlashTimer);
  el.textContent = msg;
  el.hidden = false;
  _statusFlashTimer = setTimeout(() => { el.hidden = true; el.textContent = ''; }, ms);
}

// A7 (P8.1-T5): brief color/background transition (~160ms, CSS-driven; see
// piet.css's `html.theme-fade` rule) when mode or theme flips, so the
// recolor doesn't read as an abrupt hard cut. Adds `theme-fade` to <html>
// BEFORE the dataset change that triggers the recolor (so the transition is
// armed ahead of the new colors applying) and removes it ~250ms later — long
// enough to cover the 160ms CSS transition with margin. The CSS rule itself
// is the reduced-motion gate (wrapped in @media prefers-reduced-motion:
// no-preference), so this class is a harmless no-op toggle there.
let _themeFadeTimer = null;
function beginThemeFade() {
  const html = document.documentElement;
  html.classList.add('theme-fade');
  clearTimeout(_themeFadeTimer);
  _themeFadeTimer = setTimeout(() => html.classList.remove('theme-fade'), 250);
}

// ─── Busy state ───────────────────────────────────────────────────────────────

function setBusy(busy) {
  state.busy = busy;
  // BUG-1473: a run starts on its own line; a `_continue` line joins only
  // within the run that opened it (console.js endDisplayLine).
  if (busy) endDisplayLine();
  const inputEl = document.getElementById('cmd-input');
  if (inputEl) inputEl.disabled = busy;
  // #cmd-bar.busy (piet.css) replaces the old per-input opacity mutation —
  // same 0.5 visual, plus pointer-events:none for the whole bar.
  const cmdBar = document.getElementById('cmd-bar');
  if (cmdBar) cmdBar.classList.toggle('busy', busy);
  const runBtn = document.getElementById('btn-run');
  if (runBtn) runBtn.disabled = busy;

  // A1-01: File▾Open Data / Run .do / File▾Open do-file… are plain <a>
  // toolbar links (no native `disabled`) — onOpenFile()/onRunDo()/
  // onOpenDoFile() already guard on state.busy so this is belt-and-suspenders,
  // plus an a11y signal. `#toolbar .toolbar-menu a.busy` (piet.css) dims +
  // pointer-events:none's any matching descendant, including dropdown-content
  // items (BUG-046) — no new CSS needed.
  const openLink   = document.querySelector('[data-action="open"]');
  const doLink     = document.querySelector('[data-action="do"]');
  const doOpenLink = document.querySelector('[data-action="do-open"]');
  for (const el of [openLink, doLink, doOpenLink]) {
    if (!el) continue;
    el.classList.toggle('busy', busy);
    el.setAttribute('aria-disabled', busy ? 'true' : 'false');
  }

  // A1-02: visual/pointer-events lock signal on the Variable Editor grid
  // while a command/mutation is in flight (the piet:var-edit listener and
  // the grid's cell-commit callback both also guard on state.busy directly —
  // see above). Same `.busy`-needs-a-piet.css-rule caveat as the toolbar
  // links.
  const varWrap = document.getElementById('var-list-wrap');
  if (varWrap) varWrap.classList.toggle('busy', busy);
}

// ─── B1 (1.16.0) / M4b: run-progress liveness strip ────────────────────────────
// Two delivery paths, both landing on the SAME _renderProgress(snap):
//   1. Poll GET /api/run_progress (~1s) — the original 1.16.0 mechanism,
//      still the ONLY path when running against the fetch/HTTP transport
//      (dev server / a browser tab). A do-run BLOCKS the /api/do call, so
//      polling MUST run concurrently — it does, because the kernel serves
//      the sync /api/do handler on a threadpool worker while the poll
//      returns on another.
//   2. window.pietEvents push (M4b, native transport only) — the C++ core's
//      background run thread calls Shell::postEvent() with the identical
//      RunProgress JSON schema roughly every 100ms WHILE a run is in
//      flight (see main.cpp's runDoInBackground) — near-instant updates
//      instead of waiting for the next ~1s poll tick. Polling stays
//      ENABLED alongside this (harmless — routes::runProgress answers from
//      the same mutex-guarded snapshot either way) so nothing regresses if
//      a push is ever missed.
// initPietEventsBridge() wires path 2; it is a no-op (never throws, listener
// simply never fires) when window.pietEvents doesn't exist (fetch transport).

function initPietEventsBridge() {
  if (typeof window === 'undefined' || !window.pietEvents) return;
  window.pietEvents.addEventListener('piet-event', (ev) => {
    const snap = ev.detail;
    // D9 (BETA Phase F): request_dialog is a NEW discriminated push — the
    // native do-run thread is blocked mid-run waiting on an answer. Checked
    // FIRST via the "kind" discriminator (existing RunProgress ticks carry no
    // such field, so this can never misfire against them); everything below
    // is the pre-existing shape-based RunProgress handling, untouched.
    if (snap && typeof snap === 'object' && snap.kind === 'request_dialog') {
      openRequestDialog(snap);
      return;
    }
    // N6 (P8.1-W2, contract_B v2.4.0): {kind:"block", seq, block} — a do/
    // do_string run's blocks streamed live as they complete, same
    // discriminator idiom as request_dialog just above (checked first, for
    // the same reason: existing RunProgress ticks carry no "kind" field at
    // all, so this can never misfire against them). appendBlock() renders it
    // immediately; applyDoResult()'s FINAL render (below) dedups against the
    // response's additive `streamed` count, so nothing double-renders.
    if (snap && typeof snap === 'object' && snap.kind === 'block') {
      // N6 integration fix: route through withMutationSummaries so a streamed
      // data_mutation block gets its merge-counts/n_dropped/reshape decoration
      // exactly like the batch path renders it — the synthesized display
      // blocks are client-side extras and do NOT advance the wire seq count
      // (dedup counts WIRE blocks only, matching the contract's `streamed`).
      appendBlocks(withMutationSummaries([snap.block]));
      _streamedSeq++;
      return;
    }
    // S6 (OVERHEAD-ZERO, drag-and-drop file open): the native window/webview
    // delegate (shell.cpp's installFileDropHandler()) is the ONLY thing that
    // ever sees a dropped file's real filesystem path — WKWebView never
    // exposes one to page script. It pushes the real path here, same "kind"
    // discriminator idiom as request_dialog/block above. Route it through
    // onOpenFile(path) — the SAME knownPath leg openRecentFile() already
    // uses (see above) — so a dropped file is byte-identical to picking it
    // via the toolbar: same /api/open -> handlers::runUse() path the `use`
    // command runs, same card, same errors. Guard the path itself so a
    // malformed push can never pop the native file picker as a side effect
    // (onOpenFile() falls back to pickFile() when its argument is falsy).
    if (snap && typeof snap === 'object' && snap.kind === 'file_dropped') {
      if (typeof snap.path === 'string' && snap.path) routeDroppedFile(snap.path);
      return;
    }
    // RunProgress ticks are self-identifying by shape (running + current_line
    // + total_lines) — other future pietEvents payloads (none yet) would be
    // ignored here rather than mis-rendered.
    if (snap && typeof snap === 'object' && 'running' in snap && 'current_line' in snap) {
      _progressLastSnap = snap;
      _paintProgress();
    }
  });
}

let _progressTimer = null;
// C3 (P8-S10, latency bands — mockup §5/PLAN.md P5): originally <1s nothing,
// ≥1s a plain text cue, ≥10s the full paced progress text + Cancel — two
// independent one-shot timers set the tier flags below, _paintProgress()
// (the single repaint path for BOTH the ~1s poll and the ~100ms native push)
// reading them alongside the latest snapshot.
// B21-1 (User dogfood 07-16): tier1's ≥1s delay is GONE — startProgressPolling()
// now sets _progressTier1Reached synchronously at run-start (see below). The
// original "a run finishing <1s never flashes the strip" rationale predates
// N6 (P8.1-W2): do/do_string runs now stream their blocks live as they
// complete, so on a fast run the RESULT blocks were appearing on screen
// before the strip that's supposed to announce "running" — backwards. Tier2
// (≥10s, Cancel) is UNCHANGED — long do-files still get a plain "running…"
// cue before the full paced text + Cancel button show up.
let _progressTier1Timer = null;   // B21-1: never scheduled now (see startProgressPolling()) —
                                   // stays declared so stopProgressPolling()'s clearTimeout(null)
                                   // no-op keeps that function's body untouched, per the brief.
let _progressTier2Timer = null;   // ≥10s
let _progressTier1Reached = false;
let _progressTier2Reached = false;
let _progressLastSnap = null;
// N6: count of blocks rendered live via the {kind:"block"} push THIS run —
// reset on every run start (startProgressPolling(), the same "one owner,
// call it at run-start" convenience the tier timers above already use).
// applyDoResult() prefers the response's own `streamed` count for the actual
// dedup logic (the authoritative wire value; see _tailBlocksToRender() —
// B.3.0 made this a slice-or-append decision, no longer a bare slice); this
// counter is bookkeeping for that same quantity, kept in step for
// consistency/future debugging.
let _streamedSeq = 0;

/** Format the strip text: "▶ running 132/1744 · reghdfe … · 3m 12s". */
function _fmtProgress(snap) {
  const line = snap.total_lines
    ? `${snap.current_line}/${snap.total_lines}`
    : `${snap.current_line}`;
  const cmd = (snap.current_cmd || '').trim();
  const secs = Math.floor((snap.elapsed_ms || 0) / 1000);
  const m = Math.floor(secs / 60), s = secs % 60;
  const elapsed = m ? `${m}m ${String(s).padStart(2, '0')}s` : `${s}s`;
  const parts = [`running ${line}`];
  if (cmd) parts.push(cmd);
  parts.push(elapsed);
  return parts.join(' · ');
}

/** C3: repaint the strip from the latest snapshot + the current latency
 *  tier. Not running (or tier1 not yet reached) → hidden, no flash. Tier1
 *  (B21-1: immediate, was ≥1s) → bare "running…" cue, Cancel stays hidden.
 *  Tier2 (≥10s) → the full line/cmd/elapsed text, Cancel revealed (only
 *  while snap.running). */
function _paintProgress() {
  const bar = document.getElementById('run-progress');
  const txt = document.getElementById('rp-text');
  const cancelBtn = document.getElementById('rp-cancel');
  if (!bar || !txt) return;
  const snap = _progressLastSnap;
  const running = !!(snap && snap.running);
  if (!running || !_progressTier1Reached) {
    bar.hidden = true;
    if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
    return;
  }
  bar.hidden = false;
  if (_progressTier2Reached) {
    txt.textContent = _fmtProgress(snap);
    if (cancelBtn) { cancelBtn.hidden = false; cancelBtn.disabled = false; }
  } else {
    txt.textContent = 'running…';
    if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
  }
}

/** Begin polling for progress. Safe to call once per do-run — every current
 *  caller (runCommand's isDoRun leg, runDoSourceCore, runPastedBlock) IS a
 *  do/do_string run; single non-do commands never call this at all, so
 *  there's no "wrong context" to guard against here.
 *  B21-1 (User dogfood 07-16): tier1 fires IMMEDIATELY (see below) — only
 *  tier2 (≥10s → Cancel) still waits on a timer; see _paintProgress(). */
function startProgressPolling() {
  stopProgressPolling();
  _streamedSeq = 0;   // N6: fresh run — reset the live-push bookkeeping counter
  // B21-1: was `setTimeout(..., 1000)` — N6 streams blocks live during a
  // do-run, so gating the strip's own appearance behind a 1s delay meant
  // fast runs rendered their (streamed) result blocks BEFORE the strip that
  // announces the run is in progress — the loading cue trailing its own
  // content. Setting the flag synchronously means the very first snapshot
  // (the immediate poll() below, or an early native piet-event push) paints
  // the strip the moment there's something real to show, with no artificial
  // floor. Nothing shows before that first snapshot regardless — see
  // _paintProgress()'s `!running` check, and stopProgressPolling() just
  // above cleared _progressLastSnap back to null.
  _progressTier1Reached = true;
  _paintProgress();
  _progressTier2Timer = setTimeout(() => { _progressTier2Reached = true; _paintProgress(); }, 10000);
  const poll = async () => {
    try {
      const snap = await client.runProgress();
      _progressLastSnap = snap;
      _paintProgress();
    } catch { /* transient — keep polling */ }
  };
  poll();                                   // immediate first paint (tier1 already open; tier2 still gated)
  _progressTimer = setInterval(poll, 1000); // ~1s cadence
}

/** Stop polling and hide the strip (call when the do-run promise settles). */
function stopProgressPolling() {
  if (_progressTimer)      { clearInterval(_progressTimer);      _progressTimer = null; }
  if (_progressTier1Timer) { clearTimeout(_progressTier1Timer);  _progressTier1Timer = null; }
  if (_progressTier2Timer) { clearTimeout(_progressTier2Timer);  _progressTier2Timer = null; }
  _progressTier1Reached = false;
  _progressTier2Reached = false;
  _progressLastSnap = null;
  const bar = document.getElementById('run-progress');
  if (bar) bar.hidden = true;
  const cancelBtn = document.getElementById('rp-cancel');
  if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
}

/** Cancel button → POST /api/run_cancel; the run stops between commands. */
async function onCancelRun() {
  const cancelBtn = document.getElementById('rp-cancel');
  if (cancelBtn) cancelBtn.disabled = true;   // debounce; re-enabled on next run
  const txt = document.getElementById('rp-text');
  if (txt) txt.textContent = 'cancelling…';
  try {
    await client.runCancel();
  } catch {
    // A1-06: a failed cancel REQUEST (network/transport error, not a failed
    // cancellation) shouldn't leave the button stuck disabled until the next
    // run starts — the run is presumably still alive, so let the user retry
    // immediately.
    if (cancelBtn) cancelBtn.disabled = false;
  }
}

// ─── Request dialog (D9, BETA Phase F — native _request()) ─────────────────
// Native mid-run blocking question. Wire shape (LOCKED, contract_B additive):
//   push   (native → frontend, the existing 'piet-event' CustomEvent —
//           initPietEventsBridge() above):
//     {kind:"request_dialog", requestId, prompt, macroName}
//   answer (frontend → native, api.js's answerRequest() ->
//           POST /api/request/answer -> routing key "request.answer"):
//     {requestId, value} -> {ok:true}
//
// Edge case — abandoned modal: the native do-run thread is genuinely BLOCKED
// on this answer, so a dismiss path that silently closes the modal without
// resolving it would hang that thread forever. Decision: disable
// dismiss-without-answering outright. #request-overlay is deliberately NOT
// registered in the OVERLAYS array (app.js, below) — so Escape /
// closeAllOverlays() / view-tab clicks can never touch it — AND its own
// keydown listener (init(), above) stops Escape from propagating to the
// document-level handler at all, belt-and-suspenders. There is no close/[x]
// control in the markup. The only two ways out are (1) submit a non-empty
// answer (<input required> plus the _pendingRequestId guard below) or
// (2) "Cancel run", which reuses the EXISTING /api/run_cancel route
// (client.runCancel() — the same one #rp-cancel already drives) and then
// dismisses the modal locally. This is the simpler of the two options this
// task's brief offered, and it ties the abandon path to a control the user
// already understands rather than inventing new UI.
//
// Cross-boundary note (for the native side, flagged rather than assumed):
// whether run_cancel() actually unblocks a thread parked inside _request()'s
// wait is a native-core decision this frontend change cannot see or verify —
// D9's wire spec doesn't say either way. If it does NOT, "Cancel run" still
// gives the user their console back immediately (this modal closes; the
// existing busy/progress-strip cleanup runs on its own schedule whenever
// runDoString()'s pending promise eventually settles) even if the native
// worker thread itself stays parked — that would be a native-core cleanup
// gap, not a frontend one, and is worth confirming with that agent.
let _pendingRequestId = null;

/** Show the blocking request-dialog overlay for a `request_dialog` push. */
function openRequestDialog(detail) {
  const ov = document.getElementById('request-overlay');
  if (!ov) return;
  // Defensive: the LOCKED wire spec always sends requestId, but don't
  // silently show an unanswerable dialog if it's ever missing.
  if (!detail || !detail.requestId) {
    console.error('request_dialog event missing requestId — ignoring', detail);
    return;
  }
  _pendingRequestId = detail.requestId;
  const macroEl  = document.getElementById('request-macro');
  const promptEl = document.getElementById('request-prompt');
  const inputEl  = document.getElementById('request-input');
  const statusEl = document.getElementById('request-status');
  if (macroEl) {
    macroEl.textContent = detail.macroName
      ? `Do-run is paused — waiting on “${detail.macroName}”`
      : 'Do-run is paused, waiting for your answer.';
  }
  if (promptEl) promptEl.textContent = detail.prompt || '';
  if (inputEl)  inputEl.value = '';
  if (statusEl) statusEl.textContent = '';
  setRequestDialogBusy(false);
  // SPEC II-4 (2026-09-02, contract_B additive): a push may carry `choices`
  // ([{value,label}]) — a fixed-choice question (the memory pre-flight's
  // proceed / raise budget / cancel) instead of free text. Render buttons and
  // hide the text form; the answer wire is unchanged ({requestId, value}).
  renderRequestChoices(detail);
  ov.classList.add('open');
  if (!(Array.isArray(detail.choices) && detail.choices.length)) inputEl?.focus();
}

/** Render (or clear) the fixed-choice buttons for a request_dialog push. */
function renderRequestChoices(detail) {
  const form = document.getElementById('request-form');
  let box = document.getElementById('request-choices');
  const choices = detail && Array.isArray(detail.choices) ? detail.choices : [];
  if (!choices.length) {
    if (box) box.replaceChildren();
    if (box) box.hidden = true;
    if (form) form.hidden = false;
    return;
  }
  if (!box) {
    box = document.createElement('div');
    box.id = 'request-choices';
    box.className = 'request-actions request-choices';
    const statusEl = document.getElementById('request-status');
    (statusEl?.parentNode || form?.parentNode || document.body).insertBefore(box, statusEl || null);
  }
  box.replaceChildren();
  box.hidden = false;
  if (form) form.hidden = true;
  for (const c of choices) {
    if (!c || typeof c.value !== 'string') continue;
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'doedit-btn' + (c.value === 'cancel' ? ' doedit-btn-danger' : (c.value === 'proceed' ? ' doedit-run' : ''));
    b.dataset.value = c.value;
    b.textContent = c.label || c.value;
    b.addEventListener('click', () => onRequestChoice(c.value, detail));
    box.appendChild(b);
  }
}

/** A fixed-choice answer. `raise` first persists the suggested budget through
 *  the existing settings.set route (and pushes it to the console/grid caps), then
 *  answers "raise" so the native gate proceeds under the new budget. */
async function onRequestChoice(value, detail) {
  if (!_pendingRequestId) return;
  const statusEl = document.getElementById('request-status');
  const box = document.getElementById('request-choices');
  if (box) for (const b of box.querySelectorAll('button')) b.disabled = true;
  try {
    if (value === 'raise' && detail && Number(detail.suggest_budget_mb) > 0) {
      const mb = Math.round(Number(detail.suggest_budget_mb));
      await client.setSettings({ memory_budget_mb: mb });
      setMemoryBudgetMb(mb);
      setGridMemoryBudgetMb(mb);
      _settingsCache = null;   // the panel re-reads on next open
    }
    await client.answerRequest(_pendingRequestId, value);
    closeRequestDialog();
  } catch (err) {
    if (statusEl) statusEl.textContent = '⚠ Could not send answer: ' + String(err && err.message ? err.message : err);
    if (box) for (const b of box.querySelectorAll('button')) b.disabled = false;
  }
}

/** Dismiss the overlay (submit success or Cancel run) and clear pending state. */
function closeRequestDialog() {
  const ov = document.getElementById('request-overlay');
  if (ov) ov.classList.remove('open');
  _pendingRequestId = null;
  renderRequestChoices(null);   // back to the text form for the next push
  document.getElementById('cmd-input')?.focus();
}

function setRequestDialogBusy(busy) {
  const submitBtn = document.getElementById('request-submit');
  const cancelBtn = document.getElementById('request-cancel-run');
  const inputEl   = document.getElementById('request-input');
  if (submitBtn) submitBtn.disabled = busy;
  if (cancelBtn) cancelBtn.disabled = busy;
  if (inputEl)   inputEl.disabled   = busy;
}

/** Submit handler — POST /api/request/answer, then dismiss on success. */
async function onRequestSubmit(e) {
  e.preventDefault();
  if (!_pendingRequestId) return;
  const inputEl  = document.getElementById('request-input');
  const statusEl = document.getElementById('request-status');
  const value = inputEl ? inputEl.value : '';
  setRequestDialogBusy(true);
  if (statusEl) statusEl.textContent = '';
  try {
    await client.answerRequest(_pendingRequestId, value);
    closeRequestDialog();
  } catch (err) {
    // Leave the modal open on failure — the native thread is presumably
    // still waiting, so a failed POST (transient transport error) must stay
    // retryable rather than silently stranding the run.
    if (statusEl) {
      statusEl.textContent = '⚠ Could not send answer: '
        + String(err && err.message ? err.message : err);
    }
    setRequestDialogBusy(false);
    inputEl?.focus();
  }
}

/** "Cancel run" — reuses the existing /api/run_cancel route (same one
 *  #rp-cancel drives), then dismisses the modal locally (see the edge-case
 *  note above). */
async function onRequestCancelRun() {
  setRequestDialogBusy(true);
  try {
    await client.runCancel();
  } catch {
    // A1-06-style: a failed cancel REQUEST is a transport hiccup, not proof
    // the run is unrecoverable — dismiss anyway (see the edge-case note
    // above); staying stuck open with no way out would be worse than closing.
  }
  closeRequestDialog();
}

// ─── Mode toggle (De Stijl ↔ Boring) ─────────────────────────────────────────

function toggleMode() {
  beginThemeFade();   // A7: arm the transition before the recolor below
  const html = document.documentElement;
  const next = html.dataset.mode === 'destijl' ? 'boring' : 'destijl';
  html.dataset.mode = next;
  localStorage.setItem('piet-mode', next);
  markUiStateDirty();   // N2
  syncThemeButtons();   // BUG-114: mode also flips theme-toggle's label (Pip Boy/De Stijl vs Dark/Light)
}

// ─── Theme toggle (light ↔ dark) ─────────────────────────────────────────────

function toggleTheme() {
  beginThemeFade();   // A7: arm the transition before the recolor below
  // D5/T2: a manual click always wins — if "Follow system appearance" was on,
  // switch it off (+ a status flash so the change isn't silent) BEFORE
  // flipping, so the click's result isn't immediately clobbered by the next
  // matchMedia 'change' event.
  if (localStorage.getItem('piet-theme-follow-system') === '1') {
    setFollowSystemTheme(false);
    flashStatus('Follow system appearance off — manual');
  }
  const html = document.documentElement;
  const isDark = html.dataset.theme === 'dark';
  if (isDark) {
    delete html.dataset.theme;
    localStorage.setItem('piet-theme', '');
  } else {
    html.dataset.theme = 'dark';
    localStorage.setItem('piet-theme', 'dark');
  }
  markUiStateDirty();   // N2
  syncThemeButtons();   // BUG-114: both labels, not just #theme-toggle
}

/** BUG-114 (User-specified): single owner for BOTH toggle labels — replaces
 *  4 previously uncoordinated update sites (toggleMode above, toggleTheme
 *  above, applySystemTheme, and index.html's old boot-script assignment —
 *  see its relocation comment) that only ever touched their OWN button,
 *  leaving the other stale after a mode-only or theme-only flip. Reads
 *  current state fresh off <html data-mode>/<html data-theme> rather than a
 *  captured pre/post-flip flag, so any caller can just call this instead of
 *  re-deriving the text itself.
 *  B21-5 (User re-ruling, 07-16 evening): the two buttons no longer
 *  share one naming convention. #theme-toggle now names the CURRENT theme
 *  ("De Stijl"/"Pip Boy" in style mode, "Light"/"Dark" in boring mode) — the
 *  User's own words, "지금이랑 반대로... 더 직관적" (the old destination-naming
 *  read as "backwards" from what's on screen — you had to mentally invert
 *  the label to know what you were looking at). #mode-toggle is UNCHANGED:
 *  it still names the ACTION ("add style?"/"go boring"), not a state, so
 *  destination-naming still applies there — there's no "current mode" noun
 *  to display instead ("boring" isn't a mode name a User thinks in the way
 *  "De Stijl"/"Pip Boy" are). Boring mode's theme label flips too (was
 *  destination Dark/Light, now current Light/Dark) purely for internal
 *  consistency — one button, one convention, regardless of which mode
 *  you're in. */
function syncThemeButtons() {
  const html = document.documentElement;
  const destijl = html.dataset.mode === 'destijl';
  const dark = html.dataset.theme === 'dark';
  const modeBtn = document.getElementById('mode-toggle');
  if (modeBtn) modeBtn.textContent = destijl ? 'go boring' : 'add style?';
  const themeBtn = document.getElementById('theme-toggle');
  if (themeBtn) themeBtn.textContent = destijl ? (dark ? 'Pip Boy' : 'De Stijl') : (dark ? 'Dark' : 'Light');
}

// ─── Command input keyboard handler ───────────────────────────────────────────

/** Submit the current command-input value (shared by Enter and the Run button). */
function submitCommand() {
  const inputEl = document.getElementById('cmd-input');
  if (!inputEl) return;
  // Defensive: if the value carries a newline (e.g. a future textarea), treat it
  // as a pasted do-block rather than mangling it into one command (BUG-010).
  // Design decision 4: same routing as onInputPaste (pref-gated).
  if (inputEl.value.includes('\n')) {
    const block = inputEl.value;
    inputEl.value = '';
    state.historyIndex = -1;
    handleMultilineBlock(block);
    return;
  }
  const cmd = inputEl.value.trim();
  inputEl.value = '';
  state.historyIndex = -1;
  if (cmd) runCommand(cmd);
}

/**
 * A11 (P8.1-T5): normalize curly/smart quotes (an autocorrect artifact from
 * Notes/Word/iOS-style keyboards) to straight ASCII quotes — Stata's parser
 * only recognizes straight quotes, so a smart-quoted `use "my file"` would
 * silently fail as unparseable syntax with no clue why the file "wasn't
 * found". Returns the input unchanged (same value, cheap === check for
 * callers) if there was nothing to convert.
 */
function normalizeSmartQuotes(text) {
  return text.replace(/[“”]/g, '"').replace(/[‘’]/g, "'");
}

/**
 * Paste handler for #cmd-input (BUG-010 origin): a single-line <input> collapses
 * pasted newlines, silently dropping every command after the first. Design
 * decision 4: when the clipboard holds a genuine multi-line block (≥2 non-empty
 * lines), route it to handleMultilineBlock() — by default that loads the
 * do-file editor (NO auto-run); with the opt-in 'piet-paste-autorun' preference
 * it runs immediately as a do-string block. A single effective line (including
 * a paste that merely ends with a trailing newline) falls through to the
 * browser's normal single-line insert — except A11 smart-quote normalization,
 * which applies there too (manual re-insert via setRangeText, since letting
 * the native paste through would carry the un-normalized text).
 */
function onInputPaste(e) {
  const raw = e.clipboardData?.getData('text');
  if (!raw) return;
  const text = raw.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const nonEmpty = text.split('\n').filter(l => l.trim()).length;
  if (nonEmpty < 2) {
    const normalized = normalizeSmartQuotes(text);
    if (normalized === text) return;   // nothing to convert: native insert, untouched
    e.preventDefault();
    const el = e.target;
    const start = el.selectionStart ?? el.value.length;
    const end   = el.selectionEnd ?? el.value.length;
    el.setRangeText(normalized, start, end, 'end');
    flashStatus('Smart quotes converted to straight quotes');
    return;
  }
  e.preventDefault();
  e.target.value = '';
  const normalized = normalizeSmartQuotes(text);
  if (normalized !== text) flashStatus('Smart quotes converted to straight quotes');
  handleMultilineBlock(normalized);
}

/**
 * Route a multi-line command block by the 'piet-paste-autorun' preference:
 * OFF (default, key absent or '0') → load into the do-file editor, foreground,
 * nothing executes (design decision 4). ON ('1', the do-editor header
 * checkbox) → run immediately as a do-string block (the pre-W2 semantics,
 * resurrected as an explicit opt-in).
 */
function handleMultilineBlock(text) {
  if (localStorage.getItem('piet-paste-autorun') === '1') {
    runPastedBlock(text);
  } else {
    loadBlockIntoDoEditor(text);
  }
}

/**
 * Load a multi-line block into the do-file editor and bring it to the
 * foreground (design decision 4). Appends with a separating newline if the
 * editor already has content, else replaces. NO auto-run.
 */
function loadBlockIntoDoEditor(text) {
  const ta = document.getElementById('doedit-text');
  if (ta) ta.value = ta.value ? `${ta.value}\n${text}` : text;
  openDoEditor();
}

/**
 * Run a multi-line pasted block as a do-string (mirrors runDoEditor). Reached
 * ONLY behind the opt-in 'piet-paste-autorun' preference — see
 * handleMultilineBlock(). Echo + history entry say 'do (pasted)' so the
 * console record makes clear what ran (SPEC III-6 label honesty).
 */
async function runPastedBlock(source) {
  if (!source.trim() || state.busy) return;
  setBusy(true);
  try {
    appendBlock({ type: 'echo', text: 'do (pasted)' });
    startProgressPolling();                          // B1: live liveness strip
    const result = await client.runDoString(source);
    applyDoResult(result);
    const ok = !(result.blocks ?? []).some(b => b.type === 'error');
    pushHistory('do (pasted)', ok);
    appendHistory('do (pasted)', ok);
  } catch (err) {
    appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
    checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
  } finally {
    stopProgressPolling();
    setBusy(false);
    document.getElementById('cmd-input')?.focus();
  }
}

function onInputKeydown(e) {
  const inputEl = e.target;

  if (e.key === 'Enter') {
    submitCommand();
    return;
  }

  // History recall: ArrowUp / PageUp
  if (e.key === 'ArrowUp' || e.key === 'PageUp') {
    e.preventDefault();
    const hist = state.commandHistory;
    if (!hist.length) return;
    if (state.historyIndex === -1) state.historyIndex = hist.length;
    if (state.historyIndex > 0) {
      state.historyIndex--;
      inputEl.value = hist[state.historyIndex].cmd;
      inputEl.select();
    }
    return;
  }

  // History forward: ArrowDown / PageDown
  if (e.key === 'ArrowDown' || e.key === 'PageDown') {
    e.preventDefault();
    const hist = state.commandHistory;
    if (state.historyIndex === -1 || state.historyIndex >= hist.length - 1) {
      state.historyIndex = -1;
      inputEl.value = '';
    } else {
      state.historyIndex++;
      inputEl.value = hist[state.historyIndex].cmd;
      inputEl.select();
    }
    return;
  }

  // Tab: varname completion with multi-match cycling
  if (e.key === 'Tab') {
    e.preventDefault();
    const varlist = getVarlist();
    if (!varlist.length) return;

    const val        = inputEl.value;
    const pos        = inputEl.selectionStart;
    const before     = val.slice(0, pos);
    const word       = before.match(/\S+$/)?.[0] ?? '';
    const stemBefore = before.slice(0, before.length - word.length);
    if (!word) return;

    const inCycle = _tabCycle.stemBefore === stemBefore &&
                    _tabCycle.idx >= 0 &&
                    _tabCycle.matches[_tabCycle.idx]?.name === word;

    if (!inCycle) {
      _tabCycle.prefix     = word;
      _tabCycle.stemBefore = stemBefore;
      _tabCycle.matches    = varlist.filter(v => v.name.startsWith(word));
      _tabCycle.idx        = -1;
    }
    if (!_tabCycle.matches.length) return;

    _tabCycle.idx = (_tabCycle.idx + 1) % _tabCycle.matches.length;
    const match   = _tabCycle.matches[_tabCycle.idx];
    const after   = val.slice(pos);
    inputEl.value = stemBefore + match.name + after;
    const newPos  = stemBefore.length + match.name.length;
    inputEl.setSelectionRange(newPos, newPos);
    return;
  }
}
