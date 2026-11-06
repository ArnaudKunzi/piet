// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/twoway_parse.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "piet/strutil.h"

namespace piet::twoway {

namespace {

using eval::OptValue;
using eval::findUnquotedComma;
using eval::parseOpts;

bool isPySpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string lstrip(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && isPySpace(s[i])) ++i;
  return s.substr(i);
}

std::string rstrip(const std::string& s) {
  size_t j = s.size();
  while (j > 0 && isPySpace(s[j - 1])) --j;
  return s.substr(0, j);
}

std::string strip(const std::string& s) { return lstrip(rstrip(s)); }

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// above is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

// Trailing `in range` extraction: `\bin\s+(\S+)\s*$` (re.IGNORECASE).
const std::regex& reIn() {
  static const std::regex r(R"(\bin\s+(\S+)\s*$)", std::regex::icase);
  return r;
}

// `_extract_if_in(s)`: strip a trailing `if expr` (and optional `in range`)
// from *s*. Returns (cleaned, if_expr, in_range).
struct IfInResult {
  std::string cleaned;
  std::optional<std::string> ifExpr;
  std::optional<std::string> inRange;
};

IfInResult extractIfIn(const std::string& sIn) {
  std::string s = strip(sIn);
  std::optional<std::string> inRange;

  // BUG-115 class sweep: use the quote-aware search (eval::searchUnquoted)
  // so a qualifier-shaped substring inside a quoted literal (e.g. `scatter y
  // x, title("in progress")`) is never mistaken for a real `in`/`if`
  // qualifier — same fix as the main tokenizer's extractQualifiers.
  using eval::searchUnquoted;
  std::smatch mIn;
  if (searchUnquoted(s, reIn(), mIn)) {
    inRange = mIn[1].str();
    s = rstrip(s.substr(0, static_cast<size_t>(mIn.position(0))));
  }

  std::optional<std::string> ifExpr;
  // `re.split(r'(?:^|\s+)if\s+', s, maxsplit=1, IGNORECASE)` — match a
  // whitespace-bounded `if` keyword OR an `if` at the very start of the
  // (stripped) string.
  static const std::regex reIfSplit(R"((?:^|\s+)if\s+)", std::regex::icase);
  std::smatch mIf;
  if (searchUnquoted(s, reIfSplit, mIf)) {
    std::string before = s.substr(0, static_cast<size_t>(mIf.position(0)));
    std::string after = s.substr(static_cast<size_t>(mIf.position(0) + mIf.length(0)));
    s = rstrip(before);
    ifExpr = rstrip(after);
    // `in` written before `if`: Stata takes the two in either order (StataBE 19.5,
    // `twoway (scatter y x in 1/10 if z==1)` is rc 0), as extractQualifiers does.
    if (!inRange && searchUnquoted(s, reIn(), mIn)) {
      inRange = mIn[1].str();
      s = rstrip(s.substr(0, static_cast<size_t>(mIn.position(0))));
    }
  }

  return {strip(s), ifExpr, inRange};
}

// `_scan_toplevel_parens(s)`: [(start,end), ...] for every top-level (...)
// group. Depth tracking mirrors findUnquotedComma exactly (quote/compound-
// quote aware) so the two stay consistent — ported directly from the
// Python reference algorithm (not reusing findUnquotedComma itself, since
// that scans for commas not parens; the state machine shape is shared).
std::vector<std::pair<size_t, size_t>> scanToplevelParens(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> groups;
  int depth = 0;
  bool inQuote = false;
  int compound = 0;  // depth of `"..."' compound quotes
  long long groupStart = -1;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    if (compound > 0) {
      if (i + 1 < n && s[i] == '"' && s[i + 1] == '\'') {
        --compound;
        i += 2;
        continue;
      }
      if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
        ++compound;
        i += 2;
        continue;
      }
      ++i;
      continue;
    }
    if (inQuote) {
      if (s[i] == '"') inQuote = false;
      ++i;
      continue;
    }
    if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
      ++compound;
      i += 2;
      continue;
    }
    if (s[i] == '"') {
      inQuote = true;
      ++i;
      continue;
    }
    if (s[i] == '(') {
      if (depth == 0) groupStart = static_cast<long long>(i);
      ++depth;
      ++i;
      continue;
    }
    if (s[i] == ')') {
      if (depth > 0) {
        --depth;
        if (depth == 0 && groupStart >= 0) {
          groups.emplace_back(static_cast<size_t>(groupStart), i + 1);
          groupStart = -1;
        }
      }
      ++i;
      continue;
    }
    ++i;
  }
  return groups;
}

// serset-twoway PARB: the layer groups are the top-level `(...)` groups at PLOT
// BOUNDARIES only -- the region must begin with `(`, and each further group
// follows the previous one's `)` with nothing but blanks between. A `(` after
// any other text (a function call in an `if`, `function sqrt(x)`, a scatteri
// clock position `(9)`) is ordinary text: `twoway scatter y x if strpos(s,
// "c") > 0` is one plot (w2al tf_q_fu1120, log:4733); `(scatter y x) (lfit y
// x) if substr(s,1,1)=="b"` is two plots and a global if (tf_q_globalstr,
// log:4897-4903). The first text that is not a group ends the layer list.
std::vector<std::pair<size_t, size_t>> boundaryGroups(const std::string& region) {
  std::vector<std::pair<size_t, size_t>> all = scanToplevelParens(region);
  std::vector<std::pair<size_t, size_t>> out;
  size_t pos = 0;
  for (const auto& g : all) {
    size_t i = pos;
    while (i < g.first && isPySpace(region[i])) ++i;
    if (i != g.first) break;  // text between the previous group and this one
    out.push_back(g);
    pos = g.second;
  }
  return out;
}

// `_parse_layer_interior(interior)`: parse the inside of one (...) layer
// group — a mini-command `plottype varlist [if expr] [, layer_opts]`.
LayerSpec parseLayerInterior(const std::string& interiorIn) {
  std::string interior = strip(interiorIn);
  long long commaIdx = findUnquotedComma(interior);
  std::string pre, post;
  if (commaIdx >= 0) {
    pre = strip(interior.substr(0, static_cast<size_t>(commaIdx)));
    post = strip(interior.substr(static_cast<size_t>(commaIdx) + 1));
  } else {
    pre = interior;
    post = "";
  }

  // serset-twoway WT: the weight clause may stand before or after the if/in
  // (`lfit y x [pw=w] if g == 1`, `scatter y x if heter == 0 [aw = w]`); it is
  // cut out first so it never rides inside the if expression.
  WeightClause wc = cutWeightClause(pre);

  IfInResult ifin = extractIfIn(pre);
  pre = ifin.cleaned;

  LayerSpec spec;
  spec.weightType = wc.type;
  spec.weightExp = wc.exp;
  spec.inRange = ifin.inRange;
  // tokens = pre.split() — whitespace-delimited, Python semantics (any run
  // of whitespace is a separator, leading/trailing ignored).
  std::vector<std::string> tokens;
  {
    size_t i = 0, n = pre.size();
    while (i < n) {
      while (i < n && isPySpace(pre[i])) ++i;
      if (i >= n) break;
      size_t start = i;
      while (i < n && !isPySpace(pre[i])) ++i;
      tokens.push_back(pre.substr(start, i - start));
    }
  }
  if (tokens.empty()) {
    return spec;  // plottype == "" -> caller skips degenerate empty groups
  }
  spec.plottype = toLower(tokens[0]);
  // AER-SCOPE1 bundle A: `sc` is Stata's accepted truncation of `scatter`, and
  // the corpus writes `tw (sc x y) (sc x z)` -- a layer plottype is read here,
  // never through the command registry, so the registry abbreviation that makes
  // `tw` work does nothing for the layer heads inside it.
  if (spec.plottype == "sc") spec.plottype = "scatter";
  spec.varlist.assign(tokens.begin() + 1, tokens.end());
  spec.ifExpr = ifin.ifExpr;
  spec.opts = post.empty() ? std::vector<std::pair<std::string, OptValue>>() : parseOpts(post);
  {
    // the text after the plot type (function/scatteri read it)
    size_t i = 0;
    while (i < pre.size() && isPySpace(pre[i])) ++i;
    while (i < pre.size() && !isPySpace(pre[i])) ++i;
    spec.rawArgs = strip(pre.substr(i));
  }
  return spec;
}

}  // namespace

ParseResult parseTwoway(const std::string& rawAfterCmd) {
  ParseResult result;
  std::string raw = strip(rawAfterCmd);
  if (raw.empty()) return result;

  // Step 1: split off the global option block at the first top-level comma.
  long long globalComma = findUnquotedComma(raw);
  std::string layerRegion, globalOptsStr;
  if (globalComma >= 0) {
    layerRegion = strip(raw.substr(0, static_cast<size_t>(globalComma)));
    globalOptsStr = strip(raw.substr(static_cast<size_t>(globalComma) + 1));
  } else {
    layerRegion = raw;
    globalOptsStr = "";
  }
  result.globalOpts = globalOptsStr.empty() ? std::vector<std::pair<std::string, OptValue>>()
                                             : parseOpts(globalOptsStr);

  // Step 2: scan for top-level parenthesised groups FIRST (a global if/in is
  // only valid OUTSIDE all groups).
  std::vector<std::pair<size_t, size_t>> groups = boundaryGroups(layerRegion);

  if (groups.empty()) {
    // Bare single-layer form (no parens): "line y x [if expr], opts".
    WeightClause wcBare = cutWeightClause(layerRegion);  // serset-twoway WT
    IfInResult ifin = extractIfIn(layerRegion);
    std::string bare = strip(ifin.cleaned);
    result.ifExpr = ifin.ifExpr;
    result.inRange = ifin.inRange;
    if (bare.empty()) return result;
    std::vector<std::string> tokens;
    {
      size_t i = 0, n = bare.size();
      while (i < n) {
        while (i < n && isPySpace(bare[i])) ++i;
        if (i >= n) break;
        size_t start = i;
        while (i < n && !isPySpace(bare[i])) ++i;
        tokens.push_back(bare.substr(start, i - start));
      }
    }
    LayerSpec spec;
    spec.plottype = toLower(tokens[0]);
  // AER-SCOPE1 bundle A: `sc` is Stata's accepted truncation of `scatter`, and
  // the corpus writes `tw (sc x y) (sc x z)` -- a layer plottype is read here,
  // never through the command registry, so the registry abbreviation that makes
  // `tw` work does nothing for the layer heads inside it.
  if (spec.plottype == "sc") spec.plottype = "scatter";
    spec.varlist.assign(tokens.begin() + 1, tokens.end());
    spec.ifExpr = std::nullopt;
    spec.opts = result.globalOpts;  // the global option block becomes the layer's own options
    spec.weightType = wcBare.type;
    spec.weightExp = wcBare.exp;
    {
      size_t i = 0;
      while (i < bare.size() && !isPySpace(bare[i])) ++i;
      spec.rawArgs = strip(bare.substr(i));
    }
    result.layers.push_back(std::move(spec));
    return result;
  }

  // Step 3: a global if/in may trail the LAST group at depth 0.
  size_t lastEnd = groups.back().second;
  std::string tail = layerRegion.substr(lastEnd);
  IfInResult tailIfin = extractIfIn(tail);
  result.ifExpr = tailIfin.ifExpr;
  result.inRange = tailIfin.inRange;

  // Step 4: parse each parenthesised group as an independent layer.
  for (const auto& [start, end] : groups) {
    std::string interior = layerRegion.substr(start + 1, (end - 1) - (start + 1));
    LayerSpec spec = parseLayerInterior(interior);
    if (!spec.plottype.empty()) result.layers.push_back(std::move(spec));
  }
  return result;
}

std::string expandDoublePipes(const std::string& rawArgs) {
  std::vector<size_t> splitPositions;
  int depth = 0;
  bool inQuote = false;
  size_t n = rawArgs.size();
  for (size_t i = 0; i + 1 < n;) {
    char c = rawArgs[i];
    if (inQuote) {
      if (c == '"') inQuote = false;
      ++i;
      continue;
    }
    if (c == '"') {
      inQuote = true;
      ++i;
      continue;
    }
    if (c == '(') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      ++i;
      continue;
    }
    if (depth == 0 && c == '|' && rawArgs[i + 1] == '|') {
      splitPositions.push_back(i);
      i += 2;
      continue;
    }
    ++i;
  }
  if (splitPositions.empty()) return rawArgs;

  std::vector<std::string> segments;
  size_t prev = 0;
  for (size_t pos : splitPositions) {
    segments.push_back(rawArgs.substr(prev, pos - prev));
    prev = pos + 2;
  }
  segments.push_back(rawArgs.substr(prev));

  std::string out;
  // serset-twoway PARA: a segment with no plot but options (`... ||, ysize(8)
  // legend(...)`, w2al tf_tw_073hist) holds twoway options only; they become the
  // global option block after the last group instead of a plot with no type.
  std::string globalTail;
  for (auto& seg : segments) {
    std::string trimmed = strip(seg);
    if (trimmed.empty()) continue;
    if (trimmed.front() == ',') {
      std::string o = strip(trimmed.substr(1));
      if (!o.empty()) globalTail += (globalTail.empty() ? "" : " ") + o;
      continue;
    }
    out += "(" + trimmed + ") ";
  }
  if (!out.empty()) out.pop_back();
  if (!globalTail.empty()) out += ", " + globalTail;
  return out;
}

WeightClause cutWeightClause(std::string& text, const std::string& defaultType) {
  WeightClause out;
  int depth = 0;
  bool inQuote = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (inQuote) {
      if (c == '"') inQuote = false;
      continue;
    }
    if (c == '"') {
      inQuote = true;
      continue;
    }
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    if (c != '[' || depth != 0) continue;
    if (i > 0 && !isPySpace(text[i - 1])) continue;  // a subscript, y[_n-1]
    const size_t close = text.find(']', i + 1);
    if (close == std::string::npos) break;
    const std::string inner = text.substr(i + 1, close - i - 1);
    const size_t eq = inner.find('=');
    if (eq == std::string::npos) continue;
    const std::string word = toLower(strip(inner.substr(0, eq)));
    std::string type;
    if (word == "fw" || word == "fweight" || word == "frequency") type = "fweight";
    else if (word == "aw" || word == "aweight" || word == "cellsize") type = "aweight";
    else if (word == "pw" || word == "pweight") type = "pweight";
    else if (word == "iw" || word == "iweight") type = "iweight";
    else if (word == "w" || word == "weight") type = defaultType;
    else continue;
    out.type = type;
    out.typed = word;
    out.exp = strip(inner.substr(eq + 1));
    text = rstrip(text.substr(0, i)) + " " + lstrip(text.substr(close + 1));
    text = strip(text);
    break;
  }
  return out;
}

bool hasToplevelParens(const std::string& rawAfterCmd) {
  std::string raw = strip(rawAfterCmd);
  if (raw.empty()) return false;
  long long globalComma = findUnquotedComma(raw);
  std::string region = globalComma >= 0 ? raw.substr(0, static_cast<size_t>(globalComma)) : raw;
  return !boundaryGroups(region).empty();
}

}  // namespace piet::twoway
