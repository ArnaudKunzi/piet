// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/twoway_opts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <regex>

#include "piet/strutil.h"

namespace piet::twoway {

namespace {

using eval::OptValue;

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// below is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  while (a < b && isSp(s[a])) ++a;
  while (b > a && isSp(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// A parsed opt value, in the shape Python's dict.get() would see: either
// unset (nullopt), a bare-flag True (isBool && boolValue), or a string.
// Mirrors `opts.get('key')` returning None | True | str.
struct MaybeOpt {
  bool present = false;
  bool isTrue = false;   // bare flag, e.g. `fcolor` with no `(...)`
  std::string str;       // the `(...)` value, when present and not a bare flag

  explicit operator bool() const { return present; }  // `if opts.get('key'):` truthiness
};

MaybeOpt getOpt(const std::vector<std::pair<std::string, OptValue>>& opts, const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return {true, v.boolValue, ""};
      return {true, false, v.strValue};
    }
  }
  return {false, false, ""};
}

// `opts.get(a) or opts.get(b) or default` — Python truthiness chain: a bare
// bool(False) or empty-string value is falsy and falls through; bool(True)
// or a non-empty string is truthy and wins.
struct ColorRaw {
  bool isSet = false;
  bool isTrueFlag = false;
  std::string str;
};

ColorRaw firstTruthy(std::initializer_list<MaybeOpt> candidates) {
  for (const auto& c : candidates) {
    if (!c.present) continue;
    if (c.isTrue) return {true, true, ""};
    if (!c.str.empty()) return {true, false, c.str};
    // present but falsy (empty string / bare-False) -- keep looking
  }
  return {false, false, ""};
}

const std::map<std::string, std::string>& stataColorPalette() {
  static const std::map<std::string, std::string> m = {
      {"blue", "#1850a8"},     {"red", "#d42a1c"},      {"green", "#2e7d32"},
      {"orange", "#e65100"},   {"purple", "#6a1b9a"},   {"teal", "#00838f"},
      {"brown", "#795548"},    {"gray", "#888888"},     {"grey", "#888888"},
      {"gs8", "#888888"},      {"gs6", "#999999"},      {"gs4", "#444444"},
      {"gs12", "#cccccc"},     {"gs14", "#dddddd"},     {"gs0", "#000000"},
      {"gs16", "#ffffff"},     {"black", "#000000"},    {"white", "#ffffff"},
      {"navy", "#001f5b"},     {"dknavy", "#0d1b4b"},   {"maroon", "#7b1020"},
      {"cranberry", "#99182c"},{"forest", "#1a5c2a"},   {"dkgreen", "#1b5e20"},
      {"sand", "#d4b483"},     {"khaki", "#b5a642"},    {"dkorange", "#bf360c"},
      {"gold", "#f9a825"},     {"yellow", "#f9a825"},   {"ltblue", "#90caf9"},
      {"midblue", "#1565c0"},  {"edkblue", "#0d47a1"},  {"lavender", "#b39ddb"},
      {"cyan", "#00838f"},     {"magenta", "#8b008b"},  {"sienna", "#9e4c2e"},
      {"lime", "#a8c74b"},     {"none", "none"},
      {"p1", kSeriesColors[0]}, {"p2", kSeriesColors[1]}, {"p3", kSeriesColors[2]},
      {"p4", kSeriesColors[3]}, {"p5", kSeriesColors[4]}, {"p6", kSeriesColors[5]},
      {"p7", kSeriesColors[6]}, {"p8", kSeriesColors[7]},
  };
  return m;
}

const std::map<std::string, double>& lwidthMap() {
  static const std::map<std::string, double> m = {
      {"none", 0.0},    {"vthin", 0.5},   {"thin", 0.7},    {"medthin", 1.0},
      {"medium", 1.2},  {"medthick", 1.8},{"thick", 2.6},   {"vthick", 3.5},
      {"vvthick", 4.5},
  };
  return m;
}

const std::map<std::string, std::optional<std::string>>& lpatternMap() {
  static const std::map<std::string, std::optional<std::string>> m = {
      {"solid", std::nullopt},        {"dash", std::string("6 4")},
      {"shortdash", std::string("3 3")}, {"longdash", std::string("10 4")},
      {"dot", std::string("1 3")},    {"shortdot", std::string("1 2")},
      {"longdash_dot", std::string("10 4 2 4")}, {"dash_dot", std::string("6 4 2 4")},
      {"_", std::nullopt},            {"-", std::string("6 4")},
      {".", std::string("1 3")},      {"l", std::nullopt},
  };
  return m;
}

struct MsymbolInfo {
  std::string shape;
  bool hollow;
};

// Single-char keys are case-SENSITIVE (O vs o); everything else is looked up
// lower-cased. Mirrors _MSYMBOL_MAP's dual-case Python dict (both 'O' and
// 'o' keys coexist) plus parse_msymbol()'s `s in map` (exact) -> lower fallback.
std::optional<MsymbolInfo> lookupMsymbol(const std::string& s) {
  static const std::map<std::string, MsymbolInfo> exact = {
      {"O", {"circle", false}}, {"o", {"circle", true}},
      {"D", {"diamond", false}}, {"d", {"diamond", true}},
      {"S", {"square", false}}, {"s", {"square", true}},
      {"T", {"triangle", false}}, {"t", {"triangle", true}},
      {"+", {"cross", false}},  {"X", {"x", false}},
      {"i", {"none", false}},
  };
  static const std::map<std::string, MsymbolInfo> lower = {
      {"circle", {"circle", false}},          {"circle_hollow", {"circle", true}},
      {"diamond", {"diamond", false}},        {"diamond_hollow", {"diamond", true}},
      {"square", {"square", false}},          {"square_hollow", {"square", true}},
      {"triangle", {"triangle", false}},      {"triangle_hollow", {"triangle", true}},
      {"cross", {"cross", false}},            {"none", {"none", false}},
      {"smcircle", {"circle", false}},        {"smcircle_hollow", {"circle", true}},
      {"smdiamond", {"diamond", false}},      {"smtriangle", {"triangle", false}},
      {"smsquare", {"square", false}},
  };
  auto it = exact.find(s);
  if (it != exact.end()) return it->second;
  auto it2 = lower.find(toLower(s));
  if (it2 != lower.end()) return it2->second;
  return std::nullopt;
}

const std::map<std::string, double>& msizeMap() {
  static const std::map<std::string, double> m = {
      {"vsmall", 1.6}, {"small", 2.2}, {"medium", 3.0}, {"large", 4.0},
      {"vlarge", 5.0}, {"tiny", 1.0},  {"zero", 0.0},
  };
  return m;
}

struct ParsedColor {
  std::string color;
  double opacity;
};

// parse_color(s, default): s is either unset (empty ColorRaw), a bare-True
// flag, or a string. Mirrors the Python function's `s is True` / `%NN`
// opacity-suffix / '#rrggbb' / palette-name branches exactly.
ParsedColor parseColor(const ColorRaw& raw, const std::string& dflt) {
  if (!raw.isSet || raw.isTrueFlag) return {dflt, 1.0};
  std::string s = strip(raw.str);
  double opacity = 1.0;
  static const std::regex reOpacity(R"(^(.+?)%(\d+)$)");
  std::smatch m;
  if (std::regex_match(s, m, reOpacity)) {
    // BUG-348: extract BOTH capture groups BEFORE touching `s`. `m` holds iterators
    // into `s`, so the old order — assign `s = m[1]`, then read `m[2]` — dereferenced
    // a match into a buffer that had just been overwritten (or freed, once the string
    // outgrew SSO). Undefined behaviour whose observable value depends on allocator
    // state: measured as a band rendering at 0.07 / 0.05 in 2 of 6 parallel corpus
    // arms and never in 8 serial ones, with the `catch (...)` below silently
    // degrading a failed parse to opacity 1.0 (which layerStyle then reports as its
    // 0.2 fallback). Same input, different answer, only under fan-out.
    const std::string stem = strip(m[1].str());
    const std::string pct = m[2].str();
    s = stem;
    try {
      opacity = std::stod(pct) / 100.0;
      opacity = std::max(0.0, std::min(1.0, opacity));
    } catch (...) {
    }
  }
  if (toLower(s) == "none") return {"none", 0.0};
  if (!s.empty() && s[0] == '#') return {toLower(s), opacity};
  auto it = stataColorPalette().find(toLower(s));
  if (it != stataColorPalette().end()) return {it->second, opacity};
  return {s, opacity};
}

// parse_lwidth(s): s unset -> 1.2 default; bare-True -> 1.2; else lookup /
// numeric-parse fallback.
double parseLwidth(const MaybeOpt& s) {
  if (!s.present || s.isTrue) return 1.2;
  std::string v = toLower(strip(s.str));
  auto it = lwidthMap().find(v);
  if (it != lwidthMap().end()) return it->second;
  try {
    return std::stod(v);
  } catch (...) {
    return 1.2;
  }
}

std::optional<std::string> parseLpattern(const MaybeOpt& s) {
  if (!s.present || s.isTrue) return std::nullopt;
  std::string v = toLower(strip(s.str));
  auto it = lpatternMap().find(v);
  if (it != lpatternMap().end()) return it->second;
  return std::nullopt;
}

MsymbolInfo parseMsymbol(const MaybeOpt& s) {
  if (!s.present || s.isTrue) return {"circle", false};
  std::string v = strip(s.str);
  if (auto found = lookupMsymbol(v)) return *found;
  return {"circle", false};
}

double parseMsize(const MaybeOpt& s) {
  if (!s.present || s.isTrue) return 2.2;
  std::string v = toLower(strip(s.str));
  auto it = msizeMap().find(v);
  if (it != msizeMap().end()) return it->second;
  try {
    return std::stod(v);
  } catch (...) {
    return 2.2;
  }
}

}  // namespace

nlohmann::ordered_json layerStyle(const std::vector<std::pair<std::string, OptValue>>& opts,
                                   int layerIdx, const std::string& plottype) {
  std::string defaultColor = kSeriesColors[static_cast<size_t>(
      ((layerIdx % static_cast<int>(kSeriesColors.size())) + static_cast<int>(kSeriesColors.size())) %
      static_cast<int>(kSeriesColors.size()))];

  nlohmann::ordered_json out;

  if (plottype == "rarea") {
    ColorRaw fcolorRaw = firstTruthy({getOpt(opts, "fcolor"), getOpt(opts, "color")});
    if (!fcolorRaw.isSet) fcolorRaw = {true, false, defaultColor};
    ColorRaw lcolorRaw = firstTruthy({getOpt(opts, "lcolor")});
    if (!lcolorRaw.isSet) lcolorRaw = {true, false, "none"};
    ParsedColor fci = parseColor(fcolorRaw, defaultColor);
    ParsedColor lci = parseColor(lcolorRaw, "none");
    double opacity = fci.opacity != 1.0 ? fci.opacity : 0.20;
    MaybeOpt lwOpt = getOpt(opts, "lwidth");
    if (!lwOpt.present) lwOpt = getOpt(opts, "lw");
    out["fcolor"] = fci.color;
    out["opacity"] = opacity;
    out["lcolor"] = lci.color;
    out["lwidth"] = parseLwidth(lwOpt);
    return out;
  }

  ColorRaw colorRaw = firstTruthy({getOpt(opts, "lcolor"), getOpt(opts, "color")});
  if (!colorRaw.isSet) colorRaw = {true, false, defaultColor};
  ParsedColor ci = parseColor(colorRaw, defaultColor);

  ColorRaw mcRaw = firstTruthy({getOpt(opts, "mcolor"), getOpt(opts, "mc")});
  if (!mcRaw.isSet) mcRaw = colorRaw;  // `color_raw` fallback (already-resolved raw)
  ParsedColor mci = parseColor(mcRaw, ci.color);

  MaybeOpt msymOpt = getOpt(opts, "msymbol");
  if (!msymOpt.present) msymOpt = getOpt(opts, "ms");
  MsymbolInfo msym = parseMsymbol(msymOpt);

  MaybeOpt lwOpt = getOpt(opts, "lwidth");
  if (!lwOpt.present) lwOpt = getOpt(opts, "lw");

  MaybeOpt lpOpt = getOpt(opts, "lpattern");
  if (!lpOpt.present) lpOpt = getOpt(opts, "lp");

  MaybeOpt msizeOpt = getOpt(opts, "msize");
  if (!msizeOpt.present) msizeOpt = getOpt(opts, "mz");
  if (!msizeOpt.present) msizeOpt = getOpt(opts, "msi");

  out["color"] = ci.color;
  out["opacity"] = ci.opacity;
  out["lwidth"] = parseLwidth(lwOpt);
  auto lp = parseLpattern(lpOpt);
  out["lpattern"] = lp.has_value() ? nlohmann::ordered_json(*lp) : nlohmann::ordered_json(nullptr);
  out["msymbol"] = msym.shape;
  out["msize"] = parseMsize(msizeOpt);
  out["hollow"] = msym.hollow;
  out["mcolor"] = mci.color;
  return out;
}

}  // namespace piet::twoway
