// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata's string-to-date translation: date(), clock(), monthly() and friends.
//
// AER scope 2, B3 stage (a). These take a STRING first argument, so like the
// rest of the string family they live only in PIET -- cobra's expression
// grammar refuses a string operand outright. The numeric half of the answer is
// still built by the shared calendar in src/data/stata_dates.h, so a date
// parsed here and a date built by mdy() in the engine are the same number by
// construction.
//
// The rules were captured from StataNow 19.5 BE before they were written:
// tests/validate/b3_funcs_oracle.{do,json} (rounds 5-6), then aer_w2
// w2y_fn_datetime and its rule probe (internal capture
// w2y_fn_datetime_probe, 2026-10-14), which overturned four of the first
// round's rules. Following [D] Datetime conversion, "How the conversion
// functions interpret the mask":
//
//   * The string is cut into ELEMENTS: punctuation and blanks separate, and a
//     letter next to a digit is a boundary ("15jan2010" is 15 / jan / 2010).
//     The '.' before fractional seconds is kept for the s component.
//   * Each mask code takes the next element, in order. A month may be a number
//     or a name: any prefix of the English name of at least three letters
//     ("sept", "janu"; not "ja", not "juneteenth").
//   * '#' inside the mask skips exactly one element; a run of '#' that ENDS the
//     mask ignores whatever is left, including nothing: date("1/15/2020",
//     "MDY#") is 21929, date("1/15/2020","#MDY") is missing, and
//     clock("1/15/2020 10:30 PM","MDYhm#") is 10:30 -- the PM went to '#'.
//   * Without a trailing '#' the whole string must be used. The one exception
//     is a final a.m./p.m. marker when the mask has h: "10:30 PM" is 22:30,
//     "13:30 pm" stays 13:30, "12:30 am" is 00:30.
//   * date() and clock() split a run of digits that is longer than its code
//     across the codes that follow it, at their fixed widths (Y 4 or 2, the
//     others 2): "Jan152010" MDY, "20200115 103045" YMDhms. The period
//     translators (monthly() etc.) never split digits ("201706" is missing);
//     instead they drop ONE single letter that sits between two numbers and
//     touches the second ("2017m6", "2010 q3", "1q2010" -- not "2010q 3",
//     "q1 2010" or "2010 3q"). date() and clock() drop no letters, so
//     date("2017m6","YM") and clock("2020-01-15T10:30:45","YMDhms") are missing.
//   * A mask is case-sensitive for date() and clock() ("ymd" is missing; h m s
//     are the time codes). The period translators read it case-insensitively:
//     quarterly(s,"yq") is quarterly(s,"YQ").
//   * Y takes the year as written; a two-digit Y needs a topyear and then means
//     the largest such year not above it. 19Y/20Y prefix a two-digit year and
//     take a four-digit one as written ("15jan2017" DM19Y is 2017); any other
//     width is missing.
//   * date() with time codes still validates the time: date("1/15/2020
//     24:00","MDYhm") is missing.
//   * An unparseable string is MISSING with rc 0. It is not an error.
#ifndef PIET_NATIVE_EVAL_STATA_DATEMASK_H
#define PIET_NATIVE_EVAL_STATA_DATEMASK_H

#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "data/stata_dates.h"

namespace piet::eval::datemask {

enum class Comp { Year4, Year19, Year20, Month, Day, Hour, Minute, Second,
                  Quarter, Half, Week, Skip };

// What the caller wants built out of the components.
enum class Kind { Daily, Clock, ClockLeap, Monthly, Quarterly, Weekly,
                  Halfyearly, Yearly };

inline const double kNA = std::numeric_limits<double>::quiet_NaN();

inline bool is_period_kind(Kind k) {
  return k == Kind::Monthly || k == Kind::Quarterly || k == Kind::Weekly ||
         k == Kind::Halfyearly || k == Kind::Yearly;
}

// `fold`: read the mask case-insensitively (the period translators).
inline bool parse_mask(const std::string& mask, std::vector<Comp>& out, bool fold = false) {
  std::string m = mask;
  if (fold)
    for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (size_t i = 0; i < m.size();) {
    if (m[i] == ' ' || m[i] == '\t') { ++i; continue; }
    if (i + 2 < m.size() && m[i] == '1' && m[i + 1] == '9' && m[i + 2] == 'Y') {
      out.push_back(Comp::Year19); i += 3; continue;
    }
    if (i + 2 < m.size() && m[i] == '2' && m[i + 1] == '0' && m[i + 2] == 'Y') {
      out.push_back(Comp::Year20); i += 3; continue;
    }
    switch (m[i]) {
      case 'Y': out.push_back(Comp::Year4); break;
      case 'M': out.push_back(Comp::Month); break;
      case 'D': out.push_back(Comp::Day); break;
      case 'h': out.push_back(Comp::Hour); break;
      case 'm': out.push_back(Comp::Minute); break;
      case 's': out.push_back(Comp::Second); break;
      case 'Q': out.push_back(Comp::Quarter); break;
      case 'H': out.push_back(Comp::Half); break;
      case 'W': out.push_back(Comp::Week); break;
      case '#': out.push_back(Comp::Skip); break;
      default: return false;   // anything else, lowercase 'y' included (unless folded)
    }
    ++i;
  }
  return !out.empty();
}

struct Token {
  std::string text;
  bool numeric = false;
  bool dot_prefixed = false;  // immediately preceded by '.', for "56.500"
  bool glued = false;         // no separator between this and the previous element
};

inline std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  bool dot = false, sep = true;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const bool digit = std::isdigit(c) != 0;
    if (digit || std::isalpha(c)) {
      size_t j = i;
      while (j < s.size() &&
             (digit ? std::isdigit(static_cast<unsigned char>(s[j])) != 0
                    : std::isalpha(static_cast<unsigned char>(s[j])) != 0))
        ++j;
      out.push_back({s.substr(i, j - i), digit, dot, !sep && !out.empty()});
      dot = false;
      sep = false;
      i = j;
    } else {
      dot = (c == '.');
      sep = true;
      ++i;
    }
  }
  return out;
}

// Month names: any case, and any prefix of the English name that is at least
// three letters long (probe p_date_d21 "janu", p_date_d23 "sept"; p_date_d22
// "ja" and p_date_d24 "juneteenth" are missing).
inline int month_from_name(const std::string& w) {
  static const char* kNames[12] = {"january", "february", "march",     "april",
                                   "may",     "june",     "july",      "august",
                                   "september", "october", "november", "december"};
  if (w.size() < 3) return 0;
  std::string k;
  for (char c : w) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (int i = 0; i < 12; ++i) {
    const std::string full = kNames[i];
    if (k.size() <= full.size() && full.compare(0, k.size(), k) == 0) return i + 1;
  }
  return 0;
}

// The leftover elements are exactly one meridian marker: "pm", "PM", or the
// "p.m." spelling, which the tokenizer cuts into "p" and "m". 0 = no, 1 = am,
// 2 = pm.
inline int meridian_of(const std::vector<Token>& rest) {
  std::string w;
  for (const Token& t : rest) {
    if (t.numeric) return 0;
    w += t.text;
  }
  for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (w == "am") return 1;
  if (w == "pm") return 2;
  return 0;
}

inline bool is_year(Comp c) {
  return c == Comp::Year4 || c == Comp::Year19 || c == Comp::Year20;
}

// The widest element a code takes before a digit run counts as packed.
inline size_t max_width(Comp c) { return is_year(c) ? 4 : 2; }

// Widths for a packed digit run of length `len` over comps[at..]: each year code
// takes 4 or 2 digits (4 first), every other code 2, and the run must end
// exactly on a code boundary after at least two codes. '#' ends the search.
// The LAST piece may be shorter than its code: StataBE uni-oracle/dr.do,
// tm.do -- clock("4:009","hms") 04:00:09 (AER 007's trip times "4:009AM"),
// clock("1:234","hms") 01:23:04, date("2010115","YMD") 05nov2010,
// date("Jan15201","MDY") year 201; a one-digit year is not ("Jan152" MDY
// missing), and digits left over still are (clock("1:23456","hms")).
inline bool packed_widths(const std::vector<Comp>& comps, size_t at, size_t len,
                          std::vector<size_t>& out) {
  if (len == 0) return out.size() >= 2;
  if (at >= comps.size() || comps[at] == Comp::Skip) return false;
  if (len < 2 || (is_year(comps[at]) && len == 3)) {
    if (len == 1 && is_year(comps[at])) return false;
    out.push_back(len);
    if (out.size() >= 2) return true;
    out.pop_back();
    return false;
  }
  const size_t options[2] = {is_year(comps[at]) ? size_t{4} : size_t{2}, size_t{2}};
  const int n_opt = is_year(comps[at]) ? 2 : 1;
  for (int o = 0; o < n_opt; ++o) {
    const size_t w = options[o];
    if (w > len) continue;
    out.push_back(w);
    if (packed_widths(comps, at + 1, len - w, out)) return true;
    out.pop_back();
  }
  return false;
}

// The whole translation. Returns the scale value, or missing.
inline double translate(const std::string& s, const std::string& mask, Kind kind,
                        bool have_top, double topyear) {
  const bool period = is_period_kind(kind);
  std::vector<Comp> comps;
  if (!parse_mask(mask, comps, period)) return kNA;

  std::vector<Token> toks = tokenize(s);

  // Period spellings carry the scale letter inside the string ("2017m6",
  // "2010 q3", "1q2010"): one letter between two numbers, touching the second.
  if (period) {
    std::vector<Token> kept;
    for (size_t i = 0; i < toks.size(); ++i) {
      const bool scale_letter = !toks[i].numeric && toks[i].text.size() == 1 && i > 0 &&
                                toks[i - 1].numeric && i + 1 < toks.size() &&
                                toks[i + 1].numeric && toks[i + 1].glued;
      if (!scale_letter) kept.push_back(toks[i]);
    }
    toks.swap(kept);
  }

  // A trailing run of '#' ignores the rest of the string; it is not matched.
  size_t body = comps.size();
  while (body > 0 && comps[body - 1] == Comp::Skip) --body;
  const bool ignore_rest = body < comps.size();

  double Y = kNA, Mo = 1, D = 1, h = 0, mi = 0, sec = 0, Q = kNA, Hf = kNA, W = kNA;
  double frac = 0.0;
  size_t year_digits = 0;
  Comp year_code = Comp::Year4;
  size_t ti = 0;
  for (size_t ci = 0; ci < body; ++ci) {
    const Comp c = comps[ci];
    if (ti >= toks.size()) return kNA;
    if (c == Comp::Skip) { ++ti; continue; }
    if (c == Comp::Month && !toks[ti].numeric) {
      const int m = month_from_name(toks[ti].text);
      if (m == 0) return kNA;
      Mo = m;
      ++ti;
      continue;
    }
    if (!toks[ti].numeric) return kNA;
    if (!period && toks[ti].text.size() > max_width(c)) {
      std::vector<size_t> widths;
      if (!packed_widths(comps, ci, toks[ti].text.size(), widths)) return kNA;
      std::vector<Token> pieces;
      size_t at = 0;
      for (size_t w : widths) {
        pieces.push_back({toks[ti].text.substr(at, w), true, false, true});
        at += w;
      }
      pieces.front().dot_prefixed = toks[ti].dot_prefixed;
      pieces.front().glued = toks[ti].glued;
      toks.erase(toks.begin() + static_cast<long>(ti));
      toks.insert(toks.begin() + static_cast<long>(ti), pieces.begin(), pieces.end());
    }
    const Token& t = toks[ti];
    double v = 0;
    for (char ch : t.text) v = v * 10 + (ch - '0');
    ++ti;
    switch (c) {
      case Comp::Year4: case Comp::Year19: case Comp::Year20:
        Y = v; year_digits = t.text.size(); year_code = c; break;
      case Comp::Month:  Mo = v; break;
      case Comp::Day:    D = v; break;
      case Comp::Hour:   h = v; break;
      case Comp::Minute: mi = v; break;
      case Comp::Second:
        sec = v;
        // "45.5": the fraction is the next element when a '.' introduced it.
        if (ti < toks.size() && toks[ti].numeric && toks[ti].dot_prefixed) {
          frac = std::stod("0." + toks[ti].text);
          ++ti;
        }
        break;
      case Comp::Quarter: Q = v; break;
      case Comp::Half:    Hf = v; break;
      case Comp::Week:    W = v; break;
      case Comp::Skip: break;
    }
  }

  int meridian = 0;
  if (!ignore_rest && ti < toks.size()) {
    bool has_hour = false;
    for (Comp c : comps) has_hour = has_hour || c == Comp::Hour;
    const std::vector<Token> rest(toks.begin() + static_cast<long>(ti), toks.end());
    meridian = has_hour ? meridian_of(rest) : 0;
    if (meridian == 0) return kNA;   // the whole string must be used
  }

  if (!std::isnan(Y)) {
    if (year_code == Comp::Year4) {
      if (year_digits == 2) {
        if (!have_top) return kNA;   // measured: missing without a topyear
        const double ty = std::floor(topyear);
        Y = ty - std::fmod(std::fmod(ty - Y, 100.0) + 100.0, 100.0);
      }
    } else if (year_digits == 2) {
      Y += (year_code == Comp::Year19) ? 1900 : 2000;
    } else if (year_digits != 4) {
      return kNA;                    // probe p_date_d14 "017", p_date_d15 "7"
    }
  }

  // A time-only mask has no year at all, and Stata answers a time-of-day on the
  // %tc line rather than missing: clock("13:45","hm") is 49500000, i.e. 13:45 on
  // 1960-01-01. Every other kind needs a year.
  bool has_year = false, has_time = false;
  for (Comp c : comps) {
    has_year = has_year || is_year(c);
    has_time = has_time || c == Comp::Hour || c == Comp::Minute || c == Comp::Second;
  }
  if (std::isnan(Y)) {
    if (!has_year && (kind == Kind::Clock || kind == Kind::ClockLeap)) {
      Y = 1960; Mo = 1; D = 1;
    } else {
      return kNA;
    }
  }
  if (meridian == 2 && h < 12) h += 12;
  if (meridian == 1 && h == 12) h = 0;

  switch (kind) {
    case Kind::Yearly:     return Y;
    case Kind::Monthly:    return cecon::stata_y_period_exact(Y, Mo, 12);
    case Kind::Quarterly:  return cecon::stata_y_period_exact(Y, Q, 4);
    case Kind::Halfyearly: return cecon::stata_y_period_exact(Y, Hf, 2);
    case Kind::Weekly:     return cecon::stata_y_period_exact(Y, W, 52);
    case Kind::Daily:
      if (has_time && std::isnan(cecon::stata_hms(h, mi, sec + frac))) return kNA;
      return cecon::stata_mdy(Mo, D, Y);
    case Kind::Clock:
    case Kind::ClockLeap: {
      const double day = cecon::stata_mdy(Mo, D, Y);
      if (std::isnan(day)) return kNA;
      const double within = cecon::stata_hms(h, mi, sec + frac);
      if (std::isnan(within)) return kNA;
      const double base = (kind == Kind::ClockLeap) ? cecon::stata_Cofd(day)
                                                    : cecon::stata_cofd(day);
      if (std::isnan(base)) return kNA;
      return base + within;
    }
  }
  return kNA;
}

}  // namespace piet::eval::datemask

#endif  // PIET_NATIVE_EVAL_STATA_DATEMASK_H
