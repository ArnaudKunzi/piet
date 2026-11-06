// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// util.js — number formatting and string padding helpers
// All formatters approximate Stata output conventions.

/**
 * Stata-style %9.0g general numeric format.
 * 7 significant figures, ≤9 chars; drops to 6 sig figs if 7 won't fit.
 * Suppresses leading zero for |x| < 1 (Stata style: .0751 not 0.0751).
 * Returns null for null input (collinear/omitted rows).
 */
export function fmtG(x) {
  if (x === null || x === undefined) return null;
  if (!Number.isFinite(x)) return x > 0 ? '+Inf' : '-Inf';
  if (x === 0) return '0';

  const neg = x < 0;
  const abs = Math.abs(x);

  function tryPrecision(sf) {
    let s = abs.toPrecision(sf);
    if (s.includes('e')) {
      // Scientific notation: Stata-style e+XX (2-digit zero-padded exponent)
      const [mant, expStr] = s.split('e');
      const e = parseInt(expStr);
      // Limit mantissa to min(sf,5) sig figs, then strip trailing zeros
      let m = parseFloat(mant).toPrecision(Math.min(sf, 5));
      m = m.replace(/\.?0+$/, '');
      const eSign = e >= 0 ? '+' : '-';
      const ePad  = String(Math.abs(e)).padStart(2, '0');
      return (neg ? '-' : '') + m + 'e' + eSign + ePad;
    }
    // Fixed notation
    if (s.includes('.')) {
      s = s.replace(/0+$/, '').replace(/\.$/, '');
    }
    // Stata: no leading zero before decimal
    s = s.replace(/^0\./, '.');
    return neg ? '-' + s : s;
  }

  const s7 = tryPrecision(7);
  if (s7.length <= 9) return s7;
  return tryPrecision(6);
}

/**
 * Format t-statistic: 2 decimal places (matches Stata's %8.2f for t column).
 */
export function fmtT(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return '';
  return x.toFixed(2);
}

/**
 * Format p-value: 3 decimal places, "0.000" for p < 0.0005.
 */
export function fmtP(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return '';
  if (x < 0.0005) return '0.000';
  if (x > 0.9995) return '1.000';
  return x.toFixed(3);
}

/**
 * Format an observation count with thousands commas: 1234567 → "1,234,567".
 */
export function fmtObs(n) {
  if (n === null || n === undefined) return '?';
  return Math.round(n).toLocaleString('en-US');
}

/**
 * Right-align string s in a field of width w.
 */
export function padL(s, w) {
  return String(s).padStart(w);
}

/**
 * Left-align (right-pad) string s in a field of width w.
 */
export function padR(s, w) {
  return String(s).padEnd(w);
}

/**
 * Escape HTML special characters for safe insertion via innerHTML.
 */
export function escHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

/**
 * Truncate string to maxLen chars, appending '…' if truncated.
 */
export function trunc(s, maxLen) {
  if (!s) return '';
  if (s.length <= maxLen) return s;
  return s.slice(0, maxLen - 1) + '…';
}
