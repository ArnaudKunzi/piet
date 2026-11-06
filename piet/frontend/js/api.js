// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// api.js — kernel client for the PIET /api/* surface.
//
// M4b (PIET 2.0 native core, ROADMAP §2.0.0 "M4b" / Master Plan Phase C):
// transport swap from `fetch(baseUrl+path)` (the Python-backend HTTP era)
// to `window.pietNative.request(path, body)` (the C++ native-core webview
// bridge, piet/native/src/shell.cpp's installBridge()). Endpoint IDENTITY
// is preserved — every `/api/foo` REST path becomes routing key "foo" (and
// `/api/label/variable` becomes "label.variable" — see pathToRoutingKey()
// below), so every OTHER file in piet/frontend/ is untouched: they all call
// HTTPClient's public methods (command/runDo/browse/rename/...), never
// fetch() or a raw path, and those method signatures are unchanged.
//
// Detection: `window.pietNative` is installed by Shell::installBridge()
// BEFORE the page's own <script> runs (via webview's init(), which injects
// ahead of any page load — see shell.cpp's installBridge() comment), so by
// the time HTTPClient's constructor runs, `typeof window.pietNative` is a
// reliable signal for "running inside the native shell" vs "opened in a
// plain browser tab" (dev/demo) vs explicit mock mode. Precedence: explicit
// `mock` param/`window.PIET_MOCK` > native bridge — i.e. mock mode, if
// requested, always wins, exactly
// as before this swap.
//
// Error-shape mapping (ROADMAP "M0 shell-review carry-forward #2" — binds
// at this exact seam): a REJECTED pietNative.request() promise carries the
// bridge's ad-hoc `{code, message}` shape (unknown_route/bad_json/
// handler_exception/cobra_launch_failed — see shell.cpp's installBridge()),
// NOT the CONTRACT.md `CommandErrorResult{type,code,message,reason?}`
// shape every OTHER error path in this app already expects (app.js's
// command-run catch block does `String(err.message ?? err)` — see
// _nativeRequest()'s catch below, which normalises the rejection into a
// real Error with a populated `.message` so that call site (and every
// other `catch`/`.message` read across the frontend) keeps working
// unmodified). This mapping happens HERE, at the shim boundary, per the
// carry-forward note's first option (rather than growing a second
// transport-error family in contract_B) — see internal note piet/CONTRACT.md's
// 2.0.0 entry for the ratified rationale.
export class ApiError extends Error {
  constructor(message, status) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

// ─── Mock fixture data ("Fatalities" — the bundled sample dataset) ────────────
// Demo/mock mode (a plain browser tab with no kernel + no native bridge)
// mirrors the app's bundled sample: the US traffic fatalities panel, 48
// states × 1982–1988 (Ruhm 1996; the Stock & Watson textbook dataset),
// compiled from US government statistics (NHTSA/FARS, Census, BEA) —
// public-domain facts. See Resources/sample/fatalities.csv and the
// "Bundled sample dataset" section of THIRD_PARTY_NOTICES.txt for
// provenance. Values below are REAL traces from that dataset: the first
// five rows, the actual summary stats, and an actual
// `feols fatal ~ beertax unemp` HC1 fit (cobra 0.19.1) — mock-fidelity law,
// structure AND values pinned to observed output.

const MOCK_VARLIST = {
  source: 'mock',
  source_file: 'fatalities.csv',
  varlist: [
    { name: 'fatal',   dtype: 'f64',    format: '%8.0gc', label: 'Number of vehicle fatalities',  n_missing: 0 },
    { name: 'beertax', dtype: 'f64',    format: '%8.0g',  label: 'Tax on case of beer',           n_missing: 0 },
    { name: 'unemp',   dtype: 'f64',    format: '%8.0g',  label: 'Unemployment rate',             n_missing: 0 },
    { name: 'income',  dtype: 'f64',    format: '%8.0gc', label: 'Per capita personal income',    n_missing: 0 },
    // B.3.0 (bounded transcripts, contract_B 2.5.0): the bulk varlist wire
    // shape dropped inline `levels` — n_levels-only now (a 108K-row factor's
    // levels array was 2.3MB per mutation). The real level strings ('no'/
    // 'yes') live in MOCK_FACTOR_LEVELS below, mockVarDetail()'s source for
    // the lazy per-variable fetch panels.js's card now does on PIN.
    { name: 'breath',  dtype: 'factor', format: '%8.0g',  label: 'Preliminary breath test law',   n_levels: 2, n_missing: 0 },
    { name: 'jail',    dtype: 'factor', format: '%8.0g',  label: 'Mandatory jail sentence',       n_levels: 2, n_missing: 1 },
  ],
  n_rows: 336,
  n_cols: 6,
};

const MOCK_COMMAND_REGRESS = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  regress fatal beertax unemp, robust' },
    {
      type: 'estimation',
      estimator: 'feols',
      depvar: 'fatal',
      n_obs: 336,
      vcov_type: 'hc1',
      r2: 0.009812560224601863,
      sigma2: 869079.746032829,
      dof_resid: 333,
      coeftable: [
        { name: 'beertax',     b:  88.53936112335201,  se:  86.74234812014322,  t: 1.02, p: 0.3081, ci_lo: -82.0926781044347,   ci_hi: 259.17140035113874 },
        { name: 'unemp',       b:  31.546229333217035, se:  14.918215364889912, t: 2.11, p: 0.0352, ci_lo:   2.2004073152937433, ci_hi:  60.892051351140324 },
        { name: '(intercept)', b: 651.458823867026,    se: 135.06717071676377,  t: 4.82, p: 0.0000, ci_lo: 385.76637638787946,   ci_hi: 917.1512713461726 },
      ],
    },
  ],
  varlist_after: null,
};

const MOCK_COMMAND_UNSUPPORTED = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  foreach v in fatal beertax {' },
    {
      type: 'error',
      code: 'unsupported_loop',
      token: 'foreach',
      message: 'foreach is a next-milestone feature (loops and macros arrive with the Tier-A interpreter).',
      reason: 'piet_gap',
    },
  ],
};

const MOCK_SUMMARIZE = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  summarize fatal beertax' },
    {
      type: 'summarize',
      detail: false,
      stats: {
        fatal:   { N: 336, mean: 928.6637, sd: 934.0515, min: 79,     max: 5504   },
        beertax: { N: 336, mean: 0.5133,   sd: 0.4778,   min: 0.0433, max: 2.7208 },
      },
    },
  ],
  varlist_after: null,
};

const MOCK_BROWSE = {
  source: 'mock',
  type: 'browse',
  offset: 0,
  limit: 5,
  n_rows_total: 336,
  columns: [
    { name: 'fatal',   dtype: 'f64',    index: 17 },
    { name: 'beertax', dtype: 'f64',    index: 7 },
    { name: 'unemp',   dtype: 'f64',    index: 4 },
    { name: 'breath',  dtype: 'factor', index: 14, levels: ['no','yes'] },
  ],
  // The real first five rows (Alabama 1982–1986).
  rows: [
    [ 839, 1.5394, 14.4, 0],
    [ 930, 1.7890, 13.7, 0],
    [ 932, 1.7143, 11.1, 0],
    [ 882, 1.6525,  8.9, 0],
    [1081, 1.6099,  9.8, 0],
  ],
};

const MOCK_HISTORY = {
  source: 'mock',
  history: [
    { cmd: 'use fatalities.csv',                    ts: Date.now() - 5000, success: true },
    { cmd: 'regress fatal beertax unemp, robust',   ts: Date.now() - 2000, success: true },
    { cmd: 'foreach v in fatal {',                  ts: Date.now() - 1000, success: false },
  ],
};

// ─── Version cache (module-level; populated on first connect) ─────────────────
// Holds the string from GET /api/version → { piet_version, ... } (Contract B v1.8.0).
// Exported so console.js can read the cached value for error-block rendering.
let _cachedPietVersion = null;

/** Return the cached PIET version string, or null if not yet fetched. */
export function getCachedPietVersion() {
  return _cachedPietVersion;
}

// ─── Client class ─────────────────────────────────────────────────────────────

export class HTTPClient {
  /**
   * @param {boolean} [mock]  if true, bypass the bridge and return fixture data
   */
  constructor(mock) {
    this.mock   = mock ?? (typeof window !== 'undefined' && window.PIET_MOCK) ?? false;
    // M4b: window.pietNative is installed by the native shell (piet::Shell::
    // installBridge()) before this module ever runs. Its presence is the
    // transport switch — see this file's top-of-file comment for the full
    // precedence rule (explicit mock > native). The Python-kernel fetch leg
    // (127.0.0.1:7331 / ?kernel= / window.PIET_KERNEL_URL) was removed with
    // the 1.x backend deletion (I-8 cutover sweep, 2026-07-09).
    this.native = (typeof window !== 'undefined' && window.pietNative) || null;
  }

  // ── POST /api/command ───────────────────────────────────────────────────────
  async command(cmd) {
    if (this.mock) return mockCommand(cmd);
    return this._post('/api/command', { cmd });
  }

  // ── POST /api/do ────────────────────────────────────────────────────────────
  async runDo(filepath) {
    if (this.mock) return { source: 'mock', blocks: [{ type: 'echo', text: `.  do ${filepath}` }, { type: 'error', code: 'no_data_loaded', message: 'mock: .do execution not available without a running kernel.' }] };
    return this._post('/api/do', { filepath });
  }

  // ── POST /api/do_string ───────────────────────────────────────────────────────
  // Run an in-memory do-file SOURCE string (the in-app do-file editor).
  async runDoString(source) {
    if (this.mock) return { source: 'mock', blocks: [{ type: 'echo', text: '.  do (editor)' }, { type: 'error', code: 'no_data_loaded', message: 'mock: .do execution not available without a running kernel.' }] };
    return this._post('/api/do_string', { source });
  }

  // ── GET /api/run_progress (B1, 1.16.0) ───────────────────────────────────────
  // Liveness snapshot for the in-flight do-run. Idle → {running:false, ...}.
  async runProgress() {
    if (this.mock) return { running: false, current_line: 0, total_lines: 0, current_cmd: '', elapsed_ms: 0, blocks_ready: 0, do_path: null, cancel_requested: false, stopped_reason: null };
    return this._get('/api/run_progress');
  }

  // ── POST /api/run_cancel (B1, 1.16.0) ────────────────────────────────────────
  // Request cancellation of the in-flight do-run. → {cancelled: bool}.
  async runCancel() {
    if (this.mock) return { cancelled: false };
    return this._post('/api/run_cancel', {});
  }

  // ── POST /api/request/answer (D9, BETA Phase F — native _request()) ─────────
  // Answers a blocking `request_dialog` push (see app.js's
  // initPietEventsBridge()/openRequestDialog() — the 'piet-event' CustomEvent
  // branch keyed on detail.kind === "request_dialog") that the native do-run
  // thread is blocked on. New contract_B additive route, mirroring pickFile()'s
  // shape (a plain {requestId, value} -> {ok:true} round trip, no data-shape
  // parsing needed so a bare _post suffices, unlike pickFile's res.path
  // unwrap). Mock mode never actually reaches here (window.pietEvents doesn't
  // exist there, so 'request_dialog' never fires) — the mock branch below is
  // just for parity with every other HTTPClient method.
  async answerRequest(requestId, value) {
    if (this.mock) return { ok: true };
    return this._post('/api/request/answer', { requestId, value });
  }

  // ── POST /api/file/save (N1, P8.1-W2 contract_B v2.4.0) ─────────────────────
  // Real save-to-disk for PNG/log/csv/svg exports — the honest replacement
  // for the anchor-download-only path (app.js's svgToPng()/onSaveResults()).
  // `payload` is exactly the contract's request shape ({suggested_name, kind,
  // text?|content_b64?, path?}); no `path` -> native NSSavePanel, an explicit
  // `path` (Settings' default-export-folder) -> direct write, no dialog.
  // Mock/browser: there is no bridge to actually write a file through — an
  // honest rejection (mirrors pickFile()'s "requires the native shell" idiom
  // below) rather than fabricating a {ok:false,cancelled:true} the user never
  // saw. Call sites feature-detect `client.native` BEFORE ever calling this
  // (house rule for this wave), so this mock branch is defensive, not load-
  // bearing, in mock/DOM-fixture runs.
  async saveFile(payload) {
    if (this.mock) throw new ApiError('file save requires the native shell', 0);
    return this._post('/api/file/save', payload);
  }

  // ── GET /api/var_detail (N7, P8.1-W2 contract_B v2.4.0) ──────────────────────
  // Single-column stats for panels.js's B13 detail-card upgrade: on PIN, if
  // the passive summarize cache (varstats.js) has nothing for this var yet,
  // fetch it directly instead of showing only the "run summarize" hint.
  // Mock: a benign default sourced from MOCK_VARLIST (+ MOCK_SUMMARIZE's real
  // traces for the two vars it covers) rather than a rejection — this is a
  // plain informational GET, unlike saveFile/pickFile's dialog-shaped calls,
  // so a fixture-friendly answer is more honest than an error here. Unknown
  // name -> ApiError (mirrors the contract's "unknown name -> bridge error").
  async varDetail(name) {
    if (this.mock) return mockVarDetail(name);
    return this._get('/api/var_detail', { name });
  }

  // ── POST /api/window/title (N2, P8.1-W2 contract_B v2.4.0) ───────────────────
  // Sets the native window title verbatim; app.js composes "PIET — <file>"
  // (or "PIET" with no file loaded) and calls this fire-and-forget from
  // updateStatus(). Mock: {ok:true} no-op — same "parity, even though this
  // transport never really needs it" idiom as answerRequest() above (a mock/
  // browser tab has no native window chrome to retitle).
  async setWindowTitle(text) {
    if (this.mock) return { ok: true };
    return this._post('/api/window/title', { text });
  }

  // ── POST /api/open ──────────────────────────────────────────────────────────
  async open(filepath, format) {
    // U1 mock-fidelity: the real action:"open" mutation now carries the
    // resolved `filepath` (CONTRACT B) — echo it back here too so the mock
    // shape matches, even though onOpenFile() already knows this value
    // locally (from the picker) and doesn't strictly need it from the wire.
    if (this.mock) return { source: 'mock', action: 'open', filepath, varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows };
    return this._post('/api/open', { filepath, format });
  }

  // ── dialog.open (2.0.1) ───────────────────────────────────────────────────
  // Native Finder file picker. Resolves to the chosen file's absolute path, or
  // null if the user cancelled. In the native shell this shows an NSOpenPanel
  // via the `dialog.open` bridge route (WKWebView has no window.showOpenFilePicker;
  // see the native Shell::openFileDialog / webview.h show_open_dialog).
  // Without the bridge this throws honestly — there is no browser fallback.
  // `exts` is a comma-separated extension filter (e.g. "dta,csv,hbin").
  async pickFile(exts) {
    if (this.native) {
      const res = await this._nativeRequest('/api/dialog/open', { exts: exts || '' });
      return (res && res.path) ? res.path : null;   // {cancelled:true} -> null
    }
    // No window.prompt fallback: WKWebView never implemented it (the 2.0.1
    // dead-buttons lesson) and the I-8 sweep bans the idiom outright.
    throw new ApiError('file dialog requires the native shell', 0);
  }

  // ── file.read (WS-F1, editor-first Run .do — design decision 3) ─────────────
  // Reads a .do file's source WITHOUT executing it, feeding the do-file
  // editor's "load, don't run" flow (onRunDo() in app.js). Native side binds
  // this as routing key "file.read" (WS-N1, in parallel); request {path} ->
  // {source, resolved_path}. Rejects with bridge error code "bad_request" for
  // a missing/non-.do/>2MB file — the caller surfaces that via the normal
  // ApiError.message path (see _nativeRequest()'s catch below).
  // {followCwd:true} is the app's "the user opened this do-file" gesture: the
  // kernel moves the working directory to the file's folder, and the reply names
  // it in cwd_changed if that moved anything.
  async readDoFile(path, { followCwd = false } = {}) {
    if (this.mock) return { source: '* mock: file reading not available without a running kernel.\n', resolved_path: path };
    return this._post('/api/file/read', followCwd ? { path, follow_cwd: true } : { path });
  }

  // ── GET /api/varlist ────────────────────────────────────────────────────────
  async varlist() {
    if (this.mock) return MOCK_VARLIST;
    return this._get('/api/varlist');
  }

  // ── GET /api/browse ─────────────────────────────────────────────────────────
  async browse(offset = 0, limit = 200, vars) {
    if (this.mock) {
      const page = { ...MOCK_BROWSE, offset, limit };
      return page;
    }
    const params = { offset, limit };
    if (vars) params.vars = Array.isArray(vars) ? vars.join(',') : vars;
    return this._get('/api/browse', params);
  }

  // ── GET /api/history ────────────────────────────────────────────────────────
  async history() {
    if (this.mock) return MOCK_HISTORY;
    return this._get('/api/history');
  }

  // ── POST /api/rename ────────────────────────────────────────────────────────
  async rename(varname, newname) {
    if (this.mock) {
      const vl = MOCK_VARLIST.varlist.map(v => v.name === varname ? { ...v, name: newname } : v);
      return { source: 'mock', type: 'data_mutation', action: 'rename', varlist_after: vl, n_rows_after: MOCK_VARLIST.n_rows, renamed: { from: varname, to: newname } };
    }
    return this._post('/api/rename', { varname, newname });
  }

  // ── POST /api/label/variable ─────────────────────────────────────────────────
  async labelVariable(varname, label) {
    if (this.mock) {
      const vl = MOCK_VARLIST.varlist.map(v => v.name === varname ? { ...v, label } : v);
      return { source: 'mock', type: 'data_mutation', action: 'label', varlist_after: vl, n_rows_after: MOCK_VARLIST.n_rows };
    }
    return this._post('/api/label/variable', { varname, label });
  }

  // ── POST /api/label/define ────────────────────────────────────────────────────
  async labelDefine(lblname, mapping) {
    if (this.mock) {
      return { source: 'mock', type: 'data_mutation', action: 'label_define', varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows };
    }
    return this._post('/api/label/define', { lblname, mapping });
  }

  // ── POST /api/label/values ────────────────────────────────────────────────────
  async labelValues(varname, lblname) {
    if (this.mock) {
      return { source: 'mock', type: 'data_mutation', action: 'label_values', varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows };
    }
    return this._post('/api/label/values', { varname, lblname: lblname ?? null });
  }

  // ── POST /api/sort ────────────────────────────────────────────────────────────
  async sort(varlist, ascending) {
    if (this.mock) {
      return { source: 'mock', type: 'data_mutation', action: 'sort', varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows };
    }
    const body = { varlist };
    if (ascending != null) body.ascending = ascending;
    return this._post('/api/sort', body);
  }

  // ── POST /api/recode ──────────────────────────────────────────────────────────
  async recode(varname, rules, gen) {
    if (this.mock) {
      const newVarlist = gen
        ? [...MOCK_VARLIST.varlist, { name: gen, dtype: 'f64', format: '%9.0g', n_missing: 0 }]
        : MOCK_VARLIST.varlist;
      return { source: 'mock', type: 'data_mutation', action: 'recode', varlist_after: newVarlist, n_rows_after: MOCK_VARLIST.n_rows, new_vars: gen ? [gen] : undefined };
    }
    const body = { varname, rules };
    if (gen != null) body.gen = gen;
    return this._post('/api/recode', body);
  }

  // ── POST /api/cell ────────────────────────────────────────────────────────────
  // Data editor: write `value` at (row, varname) in the in-memory dataset.
  async cell(varname, row, value) {
    if (this.mock) {
      return { source: 'mock', type: 'data_mutation', action: 'cell_write',
               varname, row, value, varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows };
    }
    return this._post('/api/cell', { varname, row, value });
  }

  // ── GET /api/version (Contract B v1.8.0) ────────────────────────────────────
  // Returns { piet_version, cobra_version, contract_a, contract_b }.
  // On success, caches piet_version in the module-level _cachedPietVersion.
  async version() {
    if (this.mock) return null;
    try {
      const data = await this._get('/api/version');
      if (data && data.piet_version) {
        _cachedPietVersion = data.piet_version;
      }
      return data;
    } catch {
      return null;
    }
  }

  // ── legal.notices (WI-5, 2.0.3) ──────────────────────────────────────────────
  // Third-party notices + disclaimer text for the About / Legal panel.
  // Served by the app-shell `legal.notices` bridge route: packaged runs read
  // the bundled Resources/THIRD_PARTY_NOTICES.txt (generated + preflight-
  // asserted by package_native_app.sh); dev runs assemble the same content
  // from the checked-in piet/packaging/licenses/ manifest. Mock/browser
  // sessions have no notices source — the panel falls back to static text.
  async legalNotices() {
    if (this.mock) return { text: '', source: 'mock' };
    return this._get('/api/legal/notices');
  }

  // ── settings.get / settings.set (2.2.0 item D) ──────────────────────────────
  // App-shell config routes (same classification family as dialog.open /
  // file.read / legal.notices — NOT a Boundary-B kernel/data endpoint, no
  // ResultBlock, no CoefficientRow: contract_B is UNCHANGED). settings.get
  // returns the persisted native settings plus detected system info (RAM /
  // cores / recommended memory budget, SPEC II-1/II-2); settings.set writes a
  // partial and re-persists. A mock/browser session has no native store — it
  // answers from navigator.* so the panel is still usable offline.
  async getSettings() {
    if (this.mock) {
      const cores = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
      const ramMb = (typeof navigator !== 'undefined' && navigator.deviceMemory)
        ? Math.round(navigator.deviceMemory * 1024) : 8192;
      return {
        scratch_path: '', default_working_dir: '',
        // N1 (P8.1-W2, contract_B v2.4.0): default_export_dir is additive —
        // '' (ask every time) matches scratch_path/default_working_dir's own
        // empty-string default above. `ui_state` is deliberately OMITTED
        // here (not even ''): a mock/browser session has no native durable
        // store to have persisted one into, and app.js's hydrateUiState()
        // already treats an absent/non-string field as "nothing to
        // hydrate" — so this stays honest rather than implying a blob exists.
        default_export_dir: '',
        memory_budget_mb: 0, compute_cores: 0, provenance_hash: false,
        sys: { total_ram_mb: ramMb, cpu_count: cores, recommended_budget_mb: Math.round(ramMb / 2) },
        settings_file: '(mock — not persisted)',
      };
    }
    return this._get('/api/settings/get');
  }

  async setSettings(partial) {
    if (this.mock) return { ...(partial || {}), source: 'mock' };
    return this._post('/api/settings/set', partial || {});
  }

  // ── Kernel health check ─────────────────────────────────────────────────────
  async ping() {
    if (this.mock) return { ok: false, mock: true };
    if (this.native) {
      try {
        await this._nativeRequest('varlist', {});
        this.version().catch(() => { /* best-effort; ignore failures */ });
        return { ok: true };
      } catch {
        return { ok: false };
      }
    }
    // No native bridge and not mock: nothing to reach (the HTTP kernel era
    // is gone) — report not-ready honestly.
    return { ok: false };
  }

  // ── Transport dispatch ──────────────────────────────────────────────────────
  // Every public method above calls _get(path[, params]) / _post(path, body)
  // with the SAME `/api/...` REST path string it always used — these two
  // just pick a transport now. (M4b: was fetch(baseUrl+path)-only.)
  async _get(path, params) {
    if (this.native) return this._nativeRequest(path, params ?? {});
    // Mock mode never reaches here (every public method checks this.mock
    // first) — a non-mock call with no bridge is an honest failure, not a
    // silent fetch to a kernel that no longer exists.
    throw new ApiError(`GET ${path}: no native bridge (PIET runs inside its native shell)`, 0);
  }

  async _post(path, body) {
    if (this.native) return this._nativeRequest(path, body);
    throw new ApiError(`POST ${path}: no native bridge (PIET runs inside its native shell)`, 0);
  }

  // ── Native bridge transport (M4b) ───────────────────────────────────────────
  // path is the SAME `/api/...` string every public method above already
  // used with fetch — pathToRoutingKey() strips the leading "/api/" and
  // rewrites "/" to "." (POST /api/label/variable -> "label.variable"),
  // matching the routing-key names piet/native/src/main.cpp's
  // bindAllRoutes() registers. `params` is the request body for POST-shaped
  // endpoints, or the would-be query params for GET-shaped ones (the native
  // bridge has no separate query-string channel — window.pietNative.
  // request(path, body) takes exactly one body argument — so GET routes
  // just receive their params as the body instead; routes.cpp's handlers
  // for GET-shaped endpoints already read offset/limit/vars etc. from the
  // parsed JSON body for exactly this reason).
  async _nativeRequest(path, params) {
    const key = pathToRoutingKey(path);
    try {
      return await this.native.request(key, params);
    } catch (bridgeErr) {
      // M0 carry-forward #2 (bridge error-shape unification): bridgeErr is
      // the bridge's ad-hoc {code, message} object (unknown_route/bad_json/
      // handler_exception/cobra_launch_failed), NOT a CommandErrorResult and
      // NOT a JS Error — normalise it into an ApiError with a populated
      // `.message` so every existing `catch (err) { ...err.message... }`
      // call site across the frontend (app.js's command runner, etc.) keeps
      // working exactly as it did against a rejected fetch().
      const code = (bridgeErr && bridgeErr.code) || 'transport_error';
      const message = (bridgeErr && bridgeErr.message) || String(bridgeErr);
      throw new ApiError(`${key}: [${code}] ${message}`, null);
    }
  }
}

// Maps a `/api/...` REST path (exactly the strings every HTTPClient method
// above already passes to _get/_post) to the routing key piet::Shell's
// bindRoute/bindAsyncRoute calls register (main.cpp's bindAllRoutes()).
// Mechanical: strip the leading "/api/", replace "/" with ".". Covers every
// endpoint in CONTRACT.md (command, do, do_string, run_progress, run_cancel,
// open, varlist, browse, history, version, rename, label/variable,
// label/define, label/values, sort, recode, cell, tabulate, describe,
// summarize) plus file/read (WS-F1/WS-N1, editor-first Run .do),
// request/answer (D9, BETA Phase F — /api/request/answer -> "request.answer"),
// and N1/N7/N2's file/save -> "file.save", var_detail -> "var_detail" (no
// slash to rewrite — passes through unchanged), window/title -> "window.title"
// (P8.1-W2, contract_B v2.4.0) — with ONE rule, no per-endpoint mapping table
// to keep in sync.
function pathToRoutingKey(path) {
  return path.replace(/^\/api\//, '').replace(/\//g, '.');
}

// ─── Mock command dispatcher ──────────────────────────────────────────────────

function mockCommand(cmd) {
  const lc = cmd.trim().toLowerCase();

  if (/^(regress|reg|feols|xtreg|logit|probit)\b/.test(lc)) {
    return { ...MOCK_COMMAND_REGRESS, blocks: [{ type: 'echo', text: `.  ${cmd}` }, ...MOCK_COMMAND_REGRESS.blocks.slice(1)] };
  }
  if (/^(sum|summ|summarize)\b/.test(lc)) {
    return { ...MOCK_SUMMARIZE, blocks: [{ type: 'echo', text: `.  ${cmd}` }, ...MOCK_SUMMARIZE.blocks.slice(1)] };
  }
  if (/^(br|browse)\b/.test(lc)) {
    return { source: 'mock', blocks: [{ type: 'echo', text: `.  ${cmd}` }, { ...MOCK_BROWSE }] };
  }
  if (/^(tab|tabulate)\b/.test(lc)) {
    const words = cmd.trim().split(/\s+/);
    const tvar = words[1] ?? 'breath';
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        {
          type: 'tabulate',
          vars: [tvar],
          table: {
            // Real one-way counts of `breath` in the bundled Fatalities panel.
            values: { no: 181, yes: 155 },
            margins: { total: 336 },
          },
        },
      ],
    };
  }
  if (/^(desc|describe)\b/.test(lc)) {
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        {
          type: 'describe',
          variables: MOCK_VARLIST.varlist.map(v => ({
            name: v.name, dtype: v.dtype, format: v.format || '%9.0g', label: v.label || '', n_missing: v.n_missing || 0,
          })),
        },
      ],
    };
  }
  // U3: the empty-state hint teaches `sysuse fatalities` and File ▾ > Open
  // sample dataset also runs it as a console command — mock it like the real
  // `open` action (CONTRACT: action:"open" mutation carries filepath) so the
  // taught command isn't a dead end in mock/demo mode. `use` is included for
  // the same reason (both load the one canned sample in mock mode).
  if (/^(use|sysuse)\b/.test(lc)) {
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        { type: 'data_mutation', action: 'open', filepath: MOCK_VARLIST.source_file, varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows },
      ],
      varlist_after: MOCK_VARLIST.varlist,
    };
  }
  if (/^(gen|generate|egen)\b/.test(lc)) {
    const newVarName = cmd.split(/\s+/)[1]?.split('=')[0]?.trim() || 'newvar';
    const newVarlist = [
      ...MOCK_VARLIST.varlist,
      { name: newVarName, dtype: 'f64', format: '%9.0g', n_missing: 0 },
    ];
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        { type: 'data_mutation', action: 'gen', new_vars: [newVarName], varlist_after: newVarlist, n_rows_after: MOCK_VARLIST.n_rows },
      ],
      varlist_after: newVarlist,
    };
  }
  if (/^(clear)\b/.test(lc)) {
    // U1: top-level varlist_after (not just the nested block's) so
    // runCommand()'s existing "did this mutate the varlist" check fires and
    // state.currentFile correctly clears to "(no data)" in mock mode too.
    return { source: 'mock', blocks: [{ type: 'echo', text: `.  ${cmd}` }, { type: 'data_mutation', action: 'drop', varlist_after: [], n_rows_after: 0 }], varlist_after: [] };
  }
  if (/^(foreach|forvalues|while|local|global|program|#delimit)\b/.test(lc)) {
    return MOCK_COMMAND_UNSUPPORTED;
  }

  // Unknown command
  return {
    source: 'mock',
    blocks: [
      { type: 'echo', text: `.  ${cmd}` },
      {
        type: 'error',
        code: 'unsupported_command',
        token: cmd.trim().split(/\s+/)[0],
        message: `'${cmd.trim().split(/\s+/)[0]}' is not a supported command in PIET.`,
        reason: 'piet_gap',
      },
    ],
  };
}

// ─── Factor level strings (B.3.0 bounded transcripts) ──────────────────────
// MOCK_VARLIST.varlist above no longer carries inline `levels` — only
// `n_levels` — mirroring the real native varlist/data_mutation emission
// change (a 108K-row factor's levels array was 2.3MB per mutation; see
// panels.js's _renderCard()/_maybeFetchVarDetail lazy-fetch-on-PIN pattern).
// The real level strings now live ONLY behind GET /api/var_detail's
// per-variable fetch — this map is mockVarDetail()'s source for them, the
// SAME two real values ('no'/'yes') MOCK_VARLIST.varlist used to carry
// inline, just relocated rather than invented (mock-fidelity law).
//
// NOTE for whoever ratifies contract_B 2.5.0: as of this edit, the REAL
// native varDetail() handler (piet/native/src/routes.cpp) does not yet
// serialize `levels` for factor columns (only n_missing, in its factor/
// string branch) and CONTRACT.md's documented var_detail shape doesn't list
// it either. The native lane's OWN F5 comment (datastore.cpp's
// VarInfo::toJson(), same worktree) confirms the intent matches this mock —
// "the C++ `levels` vector is untouched and still read directly by
// var_detail" — so routes.cpp's varDetail() growing a `levels` field is
// expected, just not landed as of this edit. This mock runs slightly ahead
// of that so the frontend's lazy-load-on-PIN path is exercisable/testable
// now; if routes.cpp ships without it, the "click to load" affordance
// degrades quietly (placeholder just never resolves, no crash) rather than
// breaking anything.
const MOCK_FACTOR_LEVELS = {
  breath: ['no', 'yes'],
  jail:   ['no', 'yes'],
};

// ─── GET /api/var_detail mock (N7) ─────────────────────────────────────────
// Sourced from MOCK_VARLIST + (for the two vars it covers) MOCK_COMMAND_
// REGRESS-adjacent MOCK_SUMMARIZE's real cobra-fit traces — mock-fidelity
// law, no invented numbers. Unrecognized name -> ApiError, mirroring the
// real bridge's "unknown name -> bridge error" contract behavior.
function mockVarDetail(name) {
  const v = MOCK_VARLIST.varlist.find(x => x.name === name);
  if (!v) throw new ApiError(`var_detail: unknown variable '${name}'`, null);
  const detail = {
    name: v.name, dtype: v.dtype, format: v.format || '%9.0g',
    label: v.label || '', N: MOCK_VARLIST.n_rows, n_missing: v.n_missing || 0,
  };
  // B.3.0: factor level strings, now fetched here instead of shipping
  // inline on the bulk varlist (see MOCK_FACTOR_LEVELS above).
  if (v.dtype === 'factor' && MOCK_FACTOR_LEVELS[name]) {
    detail.levels = MOCK_FACTOR_LEVELS[name];
  }
  const stats = MOCK_SUMMARIZE.blocks[1]?.stats?.[name];   // only fatal/beertax carry real moments
  if (stats) {
    detail.mean = stats.mean; detail.sd = stats.sd; detail.min = stats.min; detail.max = stats.max;
  }
  return detail;
}
