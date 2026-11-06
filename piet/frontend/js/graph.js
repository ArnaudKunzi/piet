// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph.js — render a GraphResult block (CONTRACT.md B v1.3.0) as inline SVG.
// The kernel ships plot DATA (point arrays / histogram bins / line series); we
// draw the axes, ticks, gridless Stata-style frame, labels, and title here.
// No external charting lib — hand-rolled SVG, themed via the same CSS variables.

const NS = 'http://www.w3.org/2000/svg';

// Plot geometry (viewBox units; CSS scales it responsively).
const W = 520, H = 360;
const M = { top: 34, right: 18, bottom: 48, left: 62 };
const PLOT_W = W - M.left - M.right;
const PLOT_H = H - M.top - M.bottom;

function svgEl(tag, attrs = {}) {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
  return e;
}

/** "Nice" axis ticks (~target count) à la Stata/d3: rounded 1/2/5 × 10^k steps. */
function niceTicks(lo, hi, target = 5) {
  if (!isFinite(lo) || !isFinite(hi)) return [0];
  if (lo === hi) return [lo];
  const span = hi - lo;
  const raw = span / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  let step;
  if (norm < 1.5) step = 1;
  else if (norm < 3) step = 2;
  else if (norm < 7) step = 5;
  else step = 10;
  step *= mag;
  const start = Math.ceil(lo / step) * step;
  const ticks = [];
  for (let t = start; t <= hi + step * 1e-9; t += step) {
    // kill -0 and float dust
    ticks.push(Math.abs(t) < step * 1e-9 ? 0 : t);
  }
  return ticks.length ? ticks : [lo, hi];
}

/** Compact tick label: integers plain, else up to 4 sig-ish digits, with k/M suffix. */
function fmtTick(v) {
  if (v === 0) return '0';
  const a = Math.abs(v);
  if (a >= 1e6) return (v / 1e6).toFixed(a >= 1e7 ? 0 : 1) + 'M';
  if (a >= 1e4) return (v / 1e3).toFixed(0) + 'k';
  if (Number.isInteger(v)) return String(v);
  if (a >= 100) return v.toFixed(0);
  if (a >= 1)   return v.toFixed(2).replace(/\.?0+$/, '');
  return v.toPrecision(2).replace(/\.?0+$/, '');
}

/** Build a [lo,hi] range padded ~4% on each side; histograms get no bottom pad. */
function paddedRange(lo, hi, padLo = 0.04, padHi = 0.04) {
  if (!isFinite(lo) || !isFinite(hi)) return [0, 1];
  if (lo === hi) return [lo - 0.5, hi + 0.5];
  const span = hi - lo;
  return [lo - span * padLo, hi + span * padHi];
}

function makeScales(xr, yr) {
  const [x0, x1] = xr, [y0, y1] = yr;
  const sx = (x) => M.left + ((x - x0) / (x1 - x0)) * PLOT_W;
  const sy = (y) => M.top + PLOT_H - ((y - y0) / (y1 - y0)) * PLOT_H;
  return { sx, sy };
}

/**
 * Draw the plot frame, tick marks, axis labels, and axis titles.
 * @param {SVGElement} svg
 * @param {[number,number]} xr  — [x_lo, x_hi]
 * @param {[number,number]} yr  — [y_lo, y_hi]
 * @param {string} [xLabel]
 * @param {string} [yLabel]
 * @param {{ suppressXTickLabels?: boolean }} [opts]
 *   suppressXTickLabels: draw x tick marks but omit numeric text labels
 *   (used by marginsplot which places its own categorical labels instead).
 */
function drawFrameAndAxes(svg, xr, yr, xLabel, yLabel, opts = {}) {
  const { sx, sy } = makeScales(xr, yr);

  // Plot frame (Stata draws L-shaped axes; we use a light full box for clarity).
  // suppressFrame: omit the border (xscale(noline)/yscale(noline)).
  if (!opts.suppressFrame) {
    svg.appendChild(svgEl('rect', {
      x: M.left, y: M.top, width: PLOT_W, height: PLOT_H,
      class: 'g-frame',
    }));
  }

  // X ticks + labels
  for (const t of niceTicks(xr[0], xr[1])) {
    const x = sx(t);
    if (x < M.left - 0.5 || x > M.left + PLOT_W + 0.5) continue;
    svg.appendChild(svgEl('line', {
      x1: x, y1: M.top + PLOT_H, x2: x, y2: M.top + PLOT_H + 5, class: 'g-tick',
    }));
    if (!opts.suppressXTickLabels) {
      const lab = svgEl('text', { x, y: M.top + PLOT_H + 18, class: 'g-ticklabel', 'text-anchor': 'middle' });
      lab.textContent = fmtTick(t);
      svg.appendChild(lab);
    }
  }
  // Y ticks + labels
  for (const t of niceTicks(yr[0], yr[1])) {
    const y = sy(t);
    if (y < M.top - 0.5 || y > M.top + PLOT_H + 0.5) continue;
    svg.appendChild(svgEl('line', {
      x1: M.left - 5, y1: y, x2: M.left, y2: y, class: 'g-tick',
    }));
    if (!opts.suppressYTickLabels) {
      const lab = svgEl('text', { x: M.left - 9, y: y + 3.5, class: 'g-ticklabel', 'text-anchor': 'end' });
      lab.textContent = fmtTick(t);
      svg.appendChild(lab);
    }
  }

  // Axis titles
  const xt = svgEl('text', { x: M.left + PLOT_W / 2, y: H - 10, class: 'g-axistitle', 'text-anchor': 'middle' });
  xt.textContent = xLabel ?? '';
  svg.appendChild(xt);

  const yt = svgEl('text', {
    x: 14, y: M.top + PLOT_H / 2, class: 'g-axistitle', 'text-anchor': 'middle',
    transform: `rotate(-90 14 ${M.top + PLOT_H / 2})`,
  });
  yt.textContent = yLabel ?? '';
  svg.appendChild(yt);

  return { sx, sy };
}

function addTitle(svg, title) {
  const t = svgEl('text', { x: W / 2, y: 20, class: 'g-title', 'text-anchor': 'middle' });
  t.textContent = title ?? '';
  svg.appendChild(t);
}

/**
 * BS-009 (U2): render a graph's note() text. Stata draws note() as a small
 * caption below the plot; the fixed 520×360 SVG canvas has no spare room in
 * its margins for an arbitrarily long note (analysis.do's own note() text
 * runs 60+ characters) without crowding or clipping the axis title, so this
 * appends it as its own line in the DOM caption area (a sibling of the SVG,
 * inside the same .block-graph wrapper) instead of drawing it into the SVG
 * itself. Wired once, in renderGraph()'s shared final assembly, so every
 * graph kind (including graph_combine) picks it up for free.
 */
function addNote(container, note) {
  if (!note) return;
  const el = document.createElement('div');
  el.className = 'g-note';
  el.textContent = note;
  container.appendChild(el);
}

function newSvg() {
  const svg = svgEl('svg', {
    viewBox: `0 0 ${W} ${H}`, class: 'g-svg',
    xmlns: NS, role: 'img',
  });
  return svg;
}

// ─── Scatter ──────────────────────────────────────────────────────────────────
function renderScatter(block) {
  const svg = newSvg();
  addTitle(svg, block.title);
  const xr = paddedRange(block.x_min, block.x_max);
  const yr = paddedRange(block.y_min, block.y_max);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label, block.y_label);

  // Clip group to the plot rect so stray points never escape the frame.
  const g = svgEl('g', { 'clip-path': 'url(#g-clip)' });
  const clip = svgEl('clipPath', { id: 'g-clip' });
  clip.appendChild(svgEl('rect', { x: M.left, y: M.top, width: PLOT_W, height: PLOT_H }));
  svg.appendChild(clip);

  // BS-050 (BETA E): multi-y `scatter y1 y2 ... x` now populates a `series`
  // array (2+ entries, one per y-var) alongside the legacy flat `points`
  // (native's own backward-compat union of every series, for any
  // as-yet-unupdated consumer). Reading only `points` here would plot every
  // series' data as one undifferentiated cloud. `series` is ONLY present for
  // genuine multi-y blocks (native's runScatter emits the flat single-series
  // shape — no `series` key at all — whenever there's just one y-var), so
  // this branch never engages for the single-y case; the pre-existing path
  // below is untouched and renders byte-identical to before.
  const series = block.series;
  if (Array.isArray(series) && series.length > 1) {
    const seriesColors = readSeriesColors();
    // Same point-count → radius scaling as the single-series path, but keyed
    // off the combined count (block.points, native's flat union) so density
    // reads consistently regardless of how the total splits across series.
    const r = (block.points || []).length > 2000 ? 1.3 : (block.points || []).length > 500 ? 1.8 : 2.4;
    for (let si = 0; si < series.length; si++) {
      const s = series[si];
      const pts = s.points || [];
      const color = (s.style && s.style.color) || seriesColors[si % seriesColors.length];
      for (const [x, y] of pts) {
        g.appendChild(svgEl('circle', { cx: sx(x), cy: sy(y), r, fill: color, 'fill-opacity': '0.7' }));
      }
    }
    svg.appendChild(g);

    // Legend: one entry per y-series (right margin, same idiom as renderXtline).
    const LEGEND_X = M.left + PLOT_W + 4;
    for (let si = 0; si < series.length; si++) {
      const s = series[si];
      const color = (s.style && s.style.color) || seriesColors[si % seriesColors.length];
      const label = s.y_var ?? `series ${si + 1}`;
      const legendY = M.top + 14 + si * 14;
      svg.appendChild(svgEl('line', {
        x1: LEGEND_X, y1: legendY - 3, x2: LEGEND_X + 10, y2: legendY - 3,
        stroke: color, 'stroke-width': '1.5',
      }));
      const lt = svgEl('text', { x: LEGEND_X + 13, y: legendY, class: 'g-ticklabel', 'text-anchor': 'start' });
      lt.textContent = label;
      svg.appendChild(lt);
    }
    return svg;
  }

  const pts = block.points || [];
  const r = pts.length > 2000 ? 1.3 : pts.length > 500 ? 1.8 : 2.4;
  for (const [x, y] of pts) {
    g.appendChild(svgEl('circle', { cx: sx(x), cy: sy(y), r, class: 'g-point' }));
  }
  svg.appendChild(g);
  return svg;
}

// ─── Histogram ────────────────────────────────────────────────────────────────
function renderHistogram(block) {
  const svg = newSvg();
  addTitle(svg, block.title);
  const xr = paddedRange(block.x_min, block.x_max, 0.02, 0.02);
  const yMax = block.y_max > 0 ? block.y_max : 1;
  const yr = [0, yMax * 1.08]; // bars sit on the baseline; headroom on top
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label, block.y_label);

  const baseY = sy(0);
  for (const b of (block.bins || [])) {
    const x0 = sx(b.x0), x1 = sx(b.x1);
    const yTop = sy(b.y);
    const w = Math.max(0.5, x1 - x0 - 0.6); // tiny gap between bars
    const h = Math.max(0, baseY - yTop);
    const rect = svgEl('rect', { x: x0 + 0.3, y: yTop, width: w, height: h, class: 'g-bar' });
    const tip = svgEl('title');
    tip.textContent = `[${fmtTick(b.x0)}, ${fmtTick(b.x1)})  n=${b.count}`;
    rect.appendChild(tip);
    svg.appendChild(rect);
  }
  return svg;
}

// ─── Line ─────────────────────────────────────────────────────────────────────
function renderLine(block) {
  const svg = newSvg();
  addTitle(svg, block.title);
  const xr = paddedRange(block.x_min, block.x_max);
  const yr = paddedRange(block.y_min, block.y_max);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label, block.y_label);

  const pts = block.points || [];
  if (pts.length) {
    const d = pts.map(([x, y], i) => `${i ? 'L' : 'M'}${sx(x).toFixed(2)} ${sy(y).toFixed(2)}`).join(' ');
    svg.appendChild(svgEl('path', { d, class: 'g-line', fill: 'none' }));
  }
  return svg;
}

// ─── Event-study plot (csdid estat event) ────────────────────────────────────
// Point estimate + 95% CI whisker per relative period; a horizontal zero line and
// a vertical marker just before treatment (between e=-1 and e=0). Reads the
// estat-event `event_study:[{e,att,se,ci_lo,ci_hi}]` array (CONTRACT B v1.3.0).
function renderEventStudy(es, title) {
  const svg = newSvg();
  addTitle(svg, title || 'Event-study (csdid)');

  const periods = es.map(d => Number(d.e));
  const lo = Math.min(...es.map(d => d.ci_lo));
  const hi = Math.max(...es.map(d => d.ci_hi));
  const xr = paddedRange(Math.min(...periods), Math.max(...periods), 0.08, 0.08);
  const yr = paddedRange(Math.min(lo, 0), Math.max(hi, 0), 0.08, 0.08);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, 'Time relative to treatment', 'ATT');

  // zero reference line
  svg.appendChild(svgEl('line', {
    x1: M.left, y1: sy(0), x2: M.left + PLOT_W, y2: sy(0), class: 'g-zero',
  }));
  // treatment marker between e=-1 and e=0
  if (Math.min(...periods) < 0 && Math.max(...periods) >= 0) {
    const xm = sx(-0.5);
    svg.appendChild(svgEl('line', { x1: xm, y1: M.top, x2: xm, y2: M.top + PLOT_H, class: 'g-treatline' }));
  }

  for (const d of es) {
    const x = sx(Number(d.e));
    // CI whisker
    svg.appendChild(svgEl('line', { x1: x, y1: sy(d.ci_lo), x2: x, y2: sy(d.ci_hi), class: 'g-whisker' }));
    svg.appendChild(svgEl('line', { x1: x - 3, y1: sy(d.ci_lo), x2: x + 3, y2: sy(d.ci_lo), class: 'g-whisker' }));
    svg.appendChild(svgEl('line', { x1: x - 3, y1: sy(d.ci_hi), x2: x + 3, y2: sy(d.ci_hi), class: 'g-whisker' }));
    // point estimate
    const pt = svgEl('circle', { cx: x, cy: sy(d.att), r: 2.6, class: 'g-point' });
    const tip = svgEl('title');
    tip.textContent = `e=${d.e}: ATT=${d.att.toFixed(4)} [${d.ci_lo.toFixed(3)}, ${d.ci_hi.toFixed(3)}]`;
    pt.appendChild(tip);
    svg.appendChild(pt);
  }
  return svg;
}

/** Build an event-study .block element from an estat-event estimation block. */
export function renderEventStudyBlock(estBlock) {
  const div = document.createElement('div');
  div.className = 'block block-graph';
  const es = estBlock.event_study;
  if (!Array.isArray(es) || es.length === 0) return div;
  let svg;
  try {
    svg = renderEventStudy(es, estBlock.title || `Event-study: ${estBlock.depvar ?? ''}`);
  } catch (err) {
    div.textContent = `(event-study render error: ${err.message})`;
    return div;
  }
  svg.setAttribute('aria-label', `event-study plot, ${es.length} relative periods`);
  div.appendChild(svg);
  const cap = document.createElement('div');
  cap.className = 'g-caption';
  const w = estBlock.window ? ` window [${estBlock.window[0]}, ${estBlock.window[1]}]` : '';
  cap.textContent = `event-study  ·  ${es.length} periods${w}`;
  div.appendChild(cap);
  return div;
}

// ─── xtline / tsline ─────────────────────────────────────────────────────────
// Both carry block.series[] = array of { id?, label?, var?, points: [x,y][] }.
// xtline: one series per panel; tsline: one series per variable.
// Contract B v1.8.0: series[], n_panels, panel_var, time_var.

// ─── Theme-aware color tokens (A3-01 JS side, WS-F2 mission 1) ───────────────
// The palette used to be a fixed hex array baked into this file; the tokens now
// live in piet.css as --graph-accent-1/-2 and --series-1..-8, scoped per
// mode ([data-mode]) and theme ([data-theme]) by WS-C1. Read them via
// getComputedStyle ONCE per render call (never per point/series — cheap enough
// that a fresh read on every render entry is simpler than caching + wiring a
// mode/theme-change listener). These arrays are also the fallback values used
// when a property is missing (e.g. no stylesheet loaded, as in some test
// harnesses) — they match the values Boring mode ships today.
//
// GRAPH GAP (P8-S9): renderTwoway()'s multi-layer compositor reads
// --graph-line-1/-2, NOT --graph-accent-1/-2 directly — piet.css aliases
// --graph-line-* to --graph-accent-* everywhere except the Pip-Boy compound
// ([data-theme="dark"][data-mode="destijl"]), where --graph-line-* repoints
// to the pale-green/phosphor hues while --graph-accent-* itself stays put
// (still consumed unscoped by #btn-run/.doedit-run/.find-go/.find-aa — see
// piet.css's §1 tokens block + §15b compound). Boring/De Stijl-Light/
// Boring-Dark are unaffected either way (the alias resolves to the same
// value --graph-accent-1/-2 already had there).
const FALLBACK_LINE_1 = '#1850a8'; // twoway/lfit/qfit/rarea strokes+fills
const FALLBACK_LINE_2 = '#d42a1c'; // (unused today — kept for readAccentColors() symmetry)
const FALLBACK_SERIES = [
  '#1850a8', // cobalt blue
  '#d42a1c', // mondrian red
  '#2e7d32', // forest green
  '#e65100', // orange
  '#6a1b9a', // purple
  '#00838f', // teal
  '#795548', // brown
  '#546e7a', // blue-grey
];

function cssVar(computed, name, fallback) {
  const v = computed.getPropertyValue(name).trim();
  return v || fallback;
}

/** Read --graph-line-1/-2 from :root, resolved for the CURRENT mode/theme. */
function readAccentColors() {
  const cs = getComputedStyle(document.documentElement);
  return {
    accent1: cssVar(cs, '--graph-line-1', FALLBACK_LINE_1),
    accent2: cssVar(cs, '--graph-line-2', FALLBACK_LINE_2),
  };
}

/** Read --series-1..--series-8 from :root, resolved for the CURRENT mode/theme. */
function readSeriesColors() {
  const cs = getComputedStyle(document.documentElement);
  return FALLBACK_SERIES.map((fb, i) => cssVar(cs, `--series-${i + 1}`, fb));
}

function renderXtline(block) {
  const series = block.series || [];
  if (!series.length) return newSvg();

  const svg = newSvg();
  addTitle(svg, block.title);
  const seriesColors = readSeriesColors();

  // Derive ranges from provided x/y min/max or from the data itself.
  let xLo = block.x_min, xHi = block.x_max, yLo = block.y_min, yHi = block.y_max;
  if (!isFinite(xLo) || !isFinite(xHi) || !isFinite(yLo) || !isFinite(yHi)) {
    for (const s of series) {
      for (const [x, y] of (s.points || [])) {
        if (!isFinite(xLo) || x < xLo) xLo = x;
        if (!isFinite(xHi) || x > xHi) xHi = x;
        if (!isFinite(yLo) || y < yLo) yLo = y;
        if (!isFinite(yHi) || y > yHi) yHi = y;
      }
    }
  }

  const xr = paddedRange(xLo, xHi, 0.02, 0.02);
  const yr = paddedRange(yLo, yHi, 0.04, 0.06);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label || block.time_var || '', block.y_label || '');

  // Clip group
  const clip = svgEl('clipPath', { id: 'g-clip-xt' });
  clip.appendChild(svgEl('rect', { x: M.left, y: M.top, width: PLOT_W, height: PLOT_H }));
  svg.appendChild(clip);

  const g = svgEl('g', { 'clip-path': 'url(#g-clip-xt)' });

  for (let i = 0; i < series.length; i++) {
    const s = series[i];
    const pts = s.points || [];
    if (!pts.length) continue;
    const color = seriesColors[i % seriesColors.length];
    const d = pts.map(([x, y], j) => `${j ? 'L' : 'M'}${sx(x).toFixed(2)} ${sy(y).toFixed(2)}`).join(' ');
    g.appendChild(svgEl('path', {
      d,
      fill: 'none',
      stroke: color,
      'stroke-width': '1.5',
      'stroke-linejoin': 'round',
      'stroke-linecap': 'round',
    }));
  }
  svg.appendChild(g);

  // Legend: series labels on the right side of the plot (if any have labels).
  const LEGEND_X = M.left + PLOT_W + 4;
  let hasLegend = false;
  for (let i = 0; i < series.length; i++) {
    const s = series[i];
    const label = s.label ?? (s.id != null ? String(s.id) : null) ?? s.var ?? null;
    if (!label) continue;
    hasLegend = true;
    const color = seriesColors[i % seriesColors.length];
    const legendY = M.top + 14 + i * 14;
    svg.appendChild(svgEl('line', {
      x1: LEGEND_X, y1: legendY - 3,
      x2: LEGEND_X + 10, y2: legendY - 3,
      stroke: color, 'stroke-width': '1.5',
    }));
    const lt = svgEl('text', {
      x: LEGEND_X + 13,
      y: legendY,
      class: 'g-ticklabel',
      'text-anchor': 'start',
    });
    lt.textContent = label;
    svg.appendChild(lt);
  }

  // If legend is present, shift the right margin clip slightly.
  // (The viewBox is fixed at 520; legend text may clip — acceptable at this width.)
  void hasLegend;

  return svg;
}

// ─── Margins plot (marginsplot: dot-and-whisker AME chart) ───────────────────
// Points = AME estimates, whiskers = 95% CI; a horizontal zero reference line.
// Reads GraphResult(kind="marginsplot") with points:[{name,b,ci_lo,ci_hi},...].
function renderMarginsplot(block) {
  const pts = block.points || [];
  if (!pts.length) return newSvg();

  const svg = newSvg();
  addTitle(svg, block.title);

  // X axis: categorical — one slot per AME term, evenly spaced.
  // Map each name to a numeric x position [1 .. n].
  // suppressXTickLabels=true: we place variable-name labels ourselves below;
  // letting drawFrameAndAxes also emit numeric labels would produce "x11"/"x22" overlap.
  const n = pts.length;
  const xr = paddedRange(0.5, n + 0.5, 0.05, 0.05);
  const yr = paddedRange(block.y_min, block.y_max, 0.05, 0.05);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label || '', block.y_label, { suppressXTickLabels: true });

  // Zero reference line (horizontal).
  const y0 = sy(0);
  if (y0 >= M.top && y0 <= M.top + PLOT_H) {
    svg.appendChild(svgEl('line', {
      x1: M.left, y1: y0, x2: M.left + PLOT_W, y2: y0, class: 'g-zero',
    }));
  }

  // Variable name labels on x axis (rotated if many terms).
  const labelY = M.top + PLOT_H + 18;
  const rotate = n > 4;
  for (let i = 0; i < n; i++) {
    const x = sx(i + 1);
    const lab = svgEl('text', {
      x,
      y: labelY,
      class: 'g-ticklabel',
      'text-anchor': rotate ? 'end' : 'middle',
    });
    if (rotate) {
      lab.setAttribute('transform', `rotate(-35 ${x} ${labelY})`);
    }
    lab.textContent = pts[i].name;
    svg.appendChild(lab);
  }

  // Draw CI whiskers + point estimates.
  for (let i = 0; i < n; i++) {
    const d = pts[i];
    const x = sx(i + 1);

    // Vertical whisker spine.
    svg.appendChild(svgEl('line', {
      x1: x, y1: sy(d.ci_lo), x2: x, y2: sy(d.ci_hi), class: 'g-whisker',
    }));
    // CI cap ticks (horizontal serifs).
    const capLen = 4;
    svg.appendChild(svgEl('line', {
      x1: x - capLen, y1: sy(d.ci_lo), x2: x + capLen, y2: sy(d.ci_lo), class: 'g-whisker',
    }));
    svg.appendChild(svgEl('line', {
      x1: x - capLen, y1: sy(d.ci_hi), x2: x + capLen, y2: sy(d.ci_hi), class: 'g-whisker',
    }));

    // Point estimate dot.
    const pt = svgEl('circle', { cx: x, cy: sy(d.b), r: 3, class: 'g-point' });
    const tip = svgEl('title');
    tip.textContent =
      `${d.name}: AME=${d.b.toFixed(4)} [${d.ci_lo.toFixed(3)}, ${d.ci_hi.toFixed(3)}]`;
    pt.appendChild(tip);
    svg.appendChild(pt);
  }

  return svg;
}

// ─── Coefficient plot (coefplot) ──────────────────────────────────────────────
// Reads GraphResult(kind="coefplot") with
//   models:[{ name, coefs:[{ name, b, ci_lo, ci_hi }] }]  (+ x_min/x_max).
// Horizontal dot-and-CI plot (marginsplot transposed): coefficient value on the
// x axis, one y-row per coefficient (labelled by name; prefixed with the model
// name when more than one model is overlaid). A vertical zero reference line
// matches Stata coefplot's default xline(0). ci_lo/ci_hi may be null (no CI) —
// draw the point alone in that case.  (BUG-052: coefplot had no renderer at all
// and fell through to renderScatter → empty axes, `n = ?`.)
function renderCoefplot(block) {
  const models = block.models || [];
  const multi = models.length > 1;
  const rows = [];
  for (const m of models) {
    for (const c of (m.coefs || [])) {
      const lo = (c.ci_lo === null || c.ci_lo === undefined) ? null : c.ci_lo;
      const hi = (c.ci_hi === null || c.ci_hi === undefined) ? null : c.ci_hi;
      rows.push({ label: multi ? `${m.name}:${c.name}` : c.name, b: c.b, ci_lo: lo, ci_hi: hi });
    }
  }
  if (!rows.length) return newSvg();

  const svg = newSvg();
  addTitle(svg, block.title);

  const n = rows.length;
  // X: coefficient value; always include 0 so the zero reference line is in frame.
  let xlo = block.x_min, xhi = block.x_max;
  if (!isFinite(xlo) || !isFinite(xhi)) {
    const vals = rows.flatMap(r => [r.b, r.ci_lo, r.ci_hi]).filter(v => typeof v === 'number' && isFinite(v));
    xlo = Math.min(0, ...vals);
    xhi = Math.max(0, ...vals);
  } else {
    xlo = Math.min(0, xlo);
    xhi = Math.max(0, xhi);
  }
  const xr = paddedRange(xlo, xhi, 0.05, 0.05);
  const yr = paddedRange(0.5, n + 0.5, 0.05, 0.05);
  const { sx, sy } = drawFrameAndAxes(svg, xr, yr, block.x_label || 'Coefficient estimate', '',
    { suppressYTickLabels: true });

  // Vertical zero reference line (Stata coefplot xline(0)).
  const x0 = sx(0);
  if (x0 >= M.left && x0 <= M.left + PLOT_W) {
    svg.appendChild(svgEl('line', { x1: x0, y1: M.top, x2: x0, y2: M.top + PLOT_H, class: 'g-zero' }));
  }

  // Coefficient-name labels on the y axis (first coef listed → top row).
  for (let i = 0; i < n; i++) {
    const y = sy(n - i);
    const lab = svgEl('text', { x: M.left - 9, y: y + 3.5, class: 'g-ticklabel', 'text-anchor': 'end' });
    lab.textContent = rows[i].label;
    svg.appendChild(lab);
  }

  // CI whiskers (horizontal) + point estimates.
  for (let i = 0; i < n; i++) {
    const d = rows[i];
    const y = sy(n - i);
    const hasCI = d.ci_lo !== null && d.ci_hi !== null && isFinite(d.ci_lo) && isFinite(d.ci_hi);
    if (hasCI) {
      svg.appendChild(svgEl('line', { x1: sx(d.ci_lo), y1: y, x2: sx(d.ci_hi), y2: y, class: 'g-whisker' }));
      const capLen = 4;
      svg.appendChild(svgEl('line', { x1: sx(d.ci_lo), y1: y - capLen, x2: sx(d.ci_lo), y2: y + capLen, class: 'g-whisker' }));
      svg.appendChild(svgEl('line', { x1: sx(d.ci_hi), y1: y - capLen, x2: sx(d.ci_hi), y2: y + capLen, class: 'g-whisker' }));
    }
    if (typeof d.b === 'number' && isFinite(d.b)) {
      const pt = svgEl('circle', { cx: sx(d.b), cy: y, r: 3, class: 'g-point' });
      const tip = svgEl('title');
      tip.textContent = `${d.label}: b=${d.b.toFixed(4)}` +
        (hasCI ? ` [${d.ci_lo.toFixed(3)}, ${d.ci_hi.toFixed(3)}]` : '');
      pt.appendChild(tip);
      svg.appendChild(pt);
    }
  }

  return svg;
}

// ─── Twoway (multi-layer compositor) ─────────────────────────────────────────
// Renders a kind:"twoway" block — one SVG, shared scales, z-ordered layers.
// All layers share ONE coordinate system from block.x_min/x_max/y_min/y_max;
// the frontend NEVER recomputes scales per layer.  ViewBox is always 520×360
// (gallery + svgToPng depend on it).
function renderTwoway(block) {
  const svg = newSvg();
  addTitle(svg, block.title);
  // All three fallbacks below were literally '#1850a8' (accent-1) before this
  // change — same token, same value in Boring, now theme-aware in destijl/dark.
  const { accent1 } = readAccentColors();
  const seriesColors = readSeriesColors();

  const axes = block.axes || {};
  const xr = paddedRange(block.x_min, block.x_max);
  const yr = paddedRange(block.y_min, block.y_max);

  const { sx, sy } = drawFrameAndAxes(
    svg, xr, yr,
    axes.xlabel_none ? '' : (block.x_label || ''),
    axes.ylabel_none ? '' : (block.y_label || ''),
    {
      suppressXTickLabels: axes.xlabel_none || false,
      suppressYTickLabels: axes.ylabel_none || false,
      suppressFrame: (axes.xscale_noline && axes.yscale_noline) || false,
    },
  );

  // Unique clip id per render so combined/gallery panels don't collide.
  const clipId = 'g-clip-tw-' + Math.random().toString(36).slice(2, 8);
  const clip = svgEl('clipPath', { id: clipId });
  clip.appendChild(svgEl('rect', { x: M.left, y: M.top, width: PLOT_W, height: PLOT_H }));
  svg.appendChild(clip);
  const layerG = svgEl('g', { 'clip-path': `url(#${clipId})` });

  // Reference lines drawn UNDER layers (Stata puts them under data).
  for (const yv of (axes.yline || [])) {
    const yy = sy(yv);
    if (yy >= M.top && yy <= M.top + PLOT_H) {
      layerG.appendChild(svgEl('line', {
        x1: M.left, y1: yy, x2: M.left + PLOT_W, y2: yy, class: 'g-zero',
      }));
    }
  }
  for (const xv of (axes.xline || [])) {
    const xx = sx(xv);
    if (xx >= M.left && xx <= M.left + PLOT_W) {
      layerG.appendChild(svgEl('line', {
        x1: xx, y1: M.top, x2: xx, y2: M.top + PLOT_H, class: 'g-treatline',
      }));
    }
  }

  // Composite layers in array order (first = bottom).
  for (const lyr of (block.layers || [])) {
    const st = lyr.style || {};
    if (lyr.kind === 'rarea') {
      const band = lyr.band || [];
      if (band.length >= 2) {
        // Forward along yhi, backward along ylo → closed polygon.
        const fwd = band.map(([x, , yhi]) => [sx(x), sy(yhi)]);
        const back = band.slice().reverse().map(([x, ylo]) => [sx(x), sy(ylo)]);
        const pts = [...fwd, ...back];
        const d = pts.map(([px, py], i) =>
          `${i === 0 ? 'M' : 'L'}${px.toFixed(2)} ${py.toFixed(2)}`
        ).join(' ') + ' Z';
        const fcolor = st.fcolor || accent1;
        const opacity = st.opacity != null ? st.opacity : 0.3;
        layerG.appendChild(svgEl('path', {
          d,
          fill: fcolor === 'none' ? 'none' : fcolor,
          'fill-opacity': String(opacity),
          stroke: (st.lcolor && st.lcolor !== 'none') ? st.lcolor : 'none',
          'stroke-width': st.lwidth > 0 ? String(st.lwidth) : '0',
        }));
      }
    } else if (lyr.kind === 'line' || lyr.kind === 'connected' || lyr.kind === 'scatter' ||
               lyr.kind === 'lfit' || lyr.kind === 'qfit') {
      // BUG-099 (BETA E): native's twoway layer switch grew real "lfit"/"qfit"
      // kinds (a 2-point OLS line / a 100-point OLS-quadratic curve — see
      // graph_m4.cpp's layerLfit/layerQfit); this switch didn't know either
      // name, so an unrecognised lyr.kind fell through EVERY branch here
      // (not 'rarea', not this list, not 'unsupported' either) and silently
      // rendered nothing — no line, no error, no note. points[] is shaped
      // exactly like line/connected's, so folding them into this branch is
      // sufficient: a continuous stroke, no markers (the marker block below
      // only fires for 'connected'/'scatter'), color from the layer's own
      // style.color (native's layerStyle already rotates a distinct
      // per-layer color from the same kSeriesColors palette this file reads
      // via readSeriesColors — no separate default needed here).
      const pts = lyr.points || [];
      if (lyr.kind !== 'scatter' && pts.length >= 2) {
        const d = pts.map(([x, y], i) =>
          `${i === 0 ? 'M' : 'L'}${sx(x).toFixed(2)} ${sy(y).toFixed(2)}`
        ).join(' ');
        const pathAttrs = {
          d,
          fill: 'none',
          stroke: st.color || accent1,
          'stroke-width': String(st.lwidth != null ? st.lwidth : 1.5),
          'stroke-linejoin': 'round',
          'stroke-linecap': 'round',
        };
        if (st.lpattern) pathAttrs['stroke-dasharray'] = st.lpattern;
        layerG.appendChild(svgEl('path', pathAttrs));
      }
      // Markers for connected and scatter.
      if (lyr.kind === 'connected' || lyr.kind === 'scatter') {
        const r = st.msize != null ? st.msize : 2.2;
        const mcol = st.mcolor || st.color || accent1;
        const msym = st.msymbol || 'circle';
        const hollow = st.hollow || false;
        const markerR = pts.length > 2000 ? Math.min(r, 1.3)
                       : pts.length > 500 ? Math.min(r, 1.8) : r;
        for (const [x, y] of pts) {
          const cx = sx(x), cy = sy(y);
          if (msym === 'none') continue;
          if (msym === 'circle') {
            layerG.appendChild(svgEl('circle', {
              cx, cy, r: markerR,
              fill: hollow ? 'none' : mcol,
              stroke: mcol, 'stroke-width': hollow ? '0.8' : '0',
            }));
          } else if (msym === 'square') {
            const s2 = markerR * 1.4;
            layerG.appendChild(svgEl('rect', {
              x: (cx - s2).toFixed(2), y: (cy - s2).toFixed(2),
              width: (s2 * 2).toFixed(2), height: (s2 * 2).toFixed(2),
              fill: hollow ? 'none' : mcol,
              stroke: mcol, 'stroke-width': hollow ? '0.8' : '0',
            }));
          } else if (msym === 'diamond') {
            const dd = markerR * 1.5;
            layerG.appendChild(svgEl('polygon', {
              points: `${cx},${cy - dd} ${cx + dd},${cy} ${cx},${cy + dd} ${cx - dd},${cy}`,
              fill: hollow ? 'none' : mcol,
              stroke: mcol, 'stroke-width': hollow ? '0.8' : '0',
            }));
          } else if (msym === 'triangle') {
            const dt = markerR * 1.5;
            layerG.appendChild(svgEl('polygon', {
              points: `${cx},${cy - dt} ${cx + dt},${cy + dt} ${cx - dt},${cy + dt}`,
              fill: hollow ? 'none' : mcol,
              stroke: mcol, 'stroke-width': hollow ? '0.8' : '0',
            }));
          } else {
            // cross / x / fallback
            layerG.appendChild(svgEl('line', {
              x1: cx - markerR, y1: cy - markerR, x2: cx + markerR, y2: cy + markerR,
              stroke: mcol, 'stroke-width': '1',
            }));
            layerG.appendChild(svgEl('line', {
              x1: cx + markerR, y1: cy - markerR, x2: cx - markerR, y2: cy + markerR,
              stroke: mcol, 'stroke-width': '1',
            }));
          }
        }
      }
    }
  }
  svg.appendChild(layerG);

  // Legend (right side) from block.legend.order = [[layer_idx0, "label"], ...].
  const legend = block.legend || {};
  const legendOrder = legend.order || [];
  if (!legend.off && legendOrder.length > 0) {
    const layers = block.layers || [];
    const LEGEND_X = M.left + PLOT_W + 4;
    const smallFont = (legend.size === 'vsmall' || legend.size === 'small');
    // `row` counts the rows drawn: an index with no layer takes none (BUG-1308).
    // A key's text lines are joined by "\n", one row each; index -1 is text with
    // no key (`order(- "text")`, BUG-1311).
    let row = 0;
    for (let li = 0; li < legendOrder.length; li++) {
      const [layerIdx, label] = legendOrder[li];
      const lyr = layerIdx === -1 ? null : layers[layerIdx];
      if (layerIdx !== -1 && !lyr) continue;
      const legendY = M.top + 14 + row * (smallFont ? 12 : 14);
      if (lyr) {
        const lst = lyr.style || {};
        const color = lyr.kind === 'rarea'
          ? (lst.fcolor || seriesColors[layerIdx % seriesColors.length])
          : (lst.color || seriesColors[layerIdx % seriesColors.length]);
        if (lyr.kind === 'rarea') {
          svg.appendChild(svgEl('rect', {
            x: LEGEND_X, y: legendY - 7, width: 10, height: 8,
            fill: color, 'fill-opacity': String(lst.opacity != null ? lst.opacity : 0.3),
          }));
        } else {
          svg.appendChild(svgEl('line', {
            x1: LEGEND_X, y1: legendY - 3, x2: LEGEND_X + 10, y2: legendY - 3,
            stroke: color, 'stroke-width': '1.5',
          }));
        }
      }
      for (const line of String(label).split('\n')) {
        const lt = svgEl('text', {
          x: LEGEND_X + 13, y: M.top + 14 + row * (smallFont ? 12 : 14),
          class: 'g-ticklabel', 'text-anchor': 'start',
          'font-size': smallFont ? '8' : '9',
        });
        lt.textContent = line;
        svg.appendChild(lt);
        row++;
      }
    }
  }

  return svg;
}

// ─── Graph combine (panel grid) ───────────────────────────────────────────────
// Renders a kind:"graph_combine" block — viewBox = cols*520 × rows*360; each
// panel is rendered by its own renderer into a <g transform>.  Gallery and
// svgToPng read the viewBox automatically.
function renderCombine(block) {
  const grid = block.grid || { rows: 1, cols: 1 };
  const rows = grid.rows || 1;
  const cols = grid.cols || 1;
  const PW = W;   // 520 per panel
  const PH = H;   // 360 per panel
  const totalW = cols * PW;
  const totalH = rows * PH;

  const svg = svgEl('svg', {
    viewBox: `0 0 ${totalW} ${totalH}`,
    class: 'g-svg',
    xmlns: NS, role: 'img',
  });

  const bgColor = block.graphregion_color || 'white';
  if (bgColor && bgColor !== 'white') {
    svg.appendChild(svgEl('rect', { x: 0, y: 0, width: totalW, height: totalH, fill: bgColor }));
  }

  if (block.title) {
    const titleEl = svgEl('text', {
      x: totalW / 2, y: 16, class: 'g-title', 'text-anchor': 'middle',
    });
    titleEl.textContent = block.title;
    svg.appendChild(titleEl);
  }

  const panels = block.panels || [];
  for (let i = 0; i < panels.length; i++) {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const g = svgEl('g', { transform: `translate(${col * PW} ${row * PH})` });

    let panelSvg;
    const panel = panels[i];
    try {
      if (panel.kind === 'twoway')             panelSvg = renderTwoway(panel);
      else if (panel.kind === 'histogram')     panelSvg = renderHistogram(panel);
      else if (panel.kind === 'line')          panelSvg = renderLine(panel);
      else if (panel.kind === 'marginsplot')   panelSvg = renderMarginsplot(panel);
      else if (panel.kind === 'xtline' || panel.kind === 'tsline') panelSvg = renderXtline(panel);
      else                                     panelSvg = renderScatter(panel);
    } catch (err) {
      const errEl = svgEl('text', { x: PW / 2, y: PH / 2, 'text-anchor': 'middle', class: 'g-ticklabel' });
      errEl.textContent = `(panel ${i + 1} error: ${err.message})`;
      g.appendChild(errEl);
      svg.appendChild(g);
      continue;
    }

    // Move the panel SVG's children into the translate group (coords are 0,0-origin).
    while (panelSvg.firstChild) {
      g.appendChild(panelSvg.firstChild);
    }
    svg.appendChild(g);
  }

  return svg;
}

// ─── Public entry ─────────────────────────────────────────────────────────────
/** Render a GraphResult block → a .block element wrapping the SVG. */
export function renderGraph(block) {
  const div = document.createElement('div');
  div.className = 'block block-graph';

  let svg;
  try {
    if (block.kind === 'histogram')        svg = renderHistogram(block);
    else if (block.kind === 'line')        svg = renderLine(block);
    else if (block.kind === 'marginsplot') svg = renderMarginsplot(block);
    else if (block.kind === 'xtline')      svg = renderXtline(block);
    else if (block.kind === 'tsline')      svg = renderXtline(block); // same series[] shape
    else if (block.kind === 'twoway')      svg = renderTwoway(block);  // Stage-4 multi-layer
    else if (block.kind === 'graph_combine') svg = renderCombine(block); // Stage-4 grid
    else if (block.kind === 'coefplot')    svg = renderCoefplot(block); // M4a stored-estimate plot
    else                                   svg = renderScatter(block); // scatter / default
  } catch (err) {
    div.textContent = `(graph render error: ${err.message})`;
    return div;
  }

  // Contract B v1.8.0: backend emits n_obs; fall back to legacy n.
  const nObs = block.n_obs ?? block.n ?? '?';

  // Accessible caption + the SVG.
  svg.setAttribute('aria-label',
    `${block.kind} graph: ${block.title ?? ''} (n=${nObs})`);
  div.appendChild(svg);

  const cap = document.createElement('div');
  cap.className = 'g-caption';
  if (block.kind === 'coefplot') {
    // coefplot has no observation count of its own — report the coefficient
    // count instead of a meaningless `n = ?` (BUG-052 secondary symptom).
    const nc = (block.models || []).reduce((s, m) => s + (m.coefs || []).length, 0);
    cap.textContent = `coefplot  ·  ${nc} coefficient${nc === 1 ? '' : 's'}`;
  } else {
    cap.textContent = `${block.kind}  ·  n = ${nObs}`;
  }
  div.appendChild(cap);

  // BS-009 (U2): note() text, if the block carries one (empty/absent -> no-op).
  addNote(div, block.note);

  // L2-FIX-B (BUG-066 / C3-F5): honest live-view decimation caption. When the
  // kernel thinned the plotted points for the live view (`graph export` keeps
  // ALL points), state exactly how many of how many are shown so a sparser
  // scatter is never mistaken for the full data.
  if (typeof block.n_shown_points === 'number' &&
      typeof block.n_total_points === 'number' &&
      block.n_shown_points < block.n_total_points) {
    const dec = document.createElement('div');
    dec.className = 'g-decimation-note';
    dec.textContent =
      `(showing ${block.n_shown_points} of ${block.n_total_points} points)`;
    div.appendChild(dec);
  }

  return div;
}
