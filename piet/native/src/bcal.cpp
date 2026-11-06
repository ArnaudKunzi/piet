// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/bcal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "piet/strutil.h"

namespace piet::bcal {
namespace {

// --- civil-date arithmetic (Howard Hinnant's algorithms, epoch 1970-01-01) --
constexpr std::int64_t kStataEpochShift = 3653;  // 01jan1960 = day -3653 of 1970

void civilFromDaysLocal(std::int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}

const char* kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                        "jul", "aug", "sep", "oct", "nov", "dec"};
const char* kDowOut[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
const char* kMonFull[12] = {"january", "february", "march",     "april",   "may",      "june",
                            "july",    "august",   "september", "october", "november", "december"};
const char* kDowFull[7] = {"sunday",   "monday", "tuesday", "wednesday",
                           "thursday", "friday", "saturday"};

std::string lowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, unsigned m) {
  static const int k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && isLeap(y)) return 29;
  return k[m - 1];
}

// Split a string on a single character, keeping empty fields.
std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// Tokenise on SPACES ONLY. A tab is deliberately NOT whitespace here: the
// loader treats `version<TAB>19.5` as ONE token and answers "version 19.5
// invalid stbcal command" (measured, gr_tabs/h_tabs), which is exactly what
// falls out of splitting on ' ' alone.
std::vector<std::string> tokensOf(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ' ') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string joinTokens(const std::vector<std::string>& t, size_t from) {
  std::string s;
  for (size_t i = from; i < t.size(); ++i) {
    if (!s.empty()) s += " ";
    s += t[i];
  }
  return s;
}

// --- date token parsing ----------------------------------------------------
struct PartialDate {
  bool starYear = false;
  int y = 0;
  unsigned m = 0, d = 0;
};

// Break "2020jan01" / "01jan2020" / "*jan01" / "15jun*" / "20200101" into its
// three fields, or return false.
bool splitDateToken(const std::string& tok, const std::string& order,
                    std::vector<std::string>* fields) {
  for (char sep : {'-', '/', '.'}) {
    if (tok.find(sep) != std::string::npos) {
      std::vector<std::string> parts = splitOn(tok, sep);
      if (parts.size() != 3) return false;
      *fields = parts;
      return true;
    }
  }
  // Runs of digits / letters / '*'.
  std::vector<std::string> runs;
  size_t i = 0;
  while (i < tok.size()) {
    char c = tok[i];
    size_t j = i + 1;
    if (std::isdigit(static_cast<unsigned char>(c))) {
      while (j < tok.size() && std::isdigit(static_cast<unsigned char>(tok[j]))) ++j;
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      while (j < tok.size() && std::isalpha(static_cast<unsigned char>(tok[j]))) ++j;
    } else if (c != '*') {
      return false;
    }
    runs.push_back(tok.substr(i, j - i));
    i = j;
  }
  if (runs.size() == 3) {
    *fields = runs;
    return true;
  }
  if (runs.size() == 1 && runs[0].size() == 8 &&
      std::isdigit(static_cast<unsigned char>(runs[0][0]))) {
    // A bare 8-digit date: widths follow the declared order (y is 4 wide).
    std::vector<std::string> out(3);
    size_t pos = 0;
    for (int k = 0; k < 3; ++k) {
      const size_t w = (order[static_cast<size_t>(k)] == 'y') ? 4u : 2u;
      out[static_cast<size_t>(k)] = runs[0].substr(pos, w);
      pos += w;
    }
    *fields = out;
    return true;
  }
  return false;
}

bool monthFromField(const std::string& raw, unsigned* out) {
  if (raw.empty()) return false;
  if (std::isdigit(static_cast<unsigned char>(raw[0]))) {
    for (char c : raw)
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    int v = std::atoi(raw.c_str());
    if (v < 1 || v > 12) return false;
    *out = static_cast<unsigned>(v);
    return true;
  }
  const std::string lo = lowerAscii(raw);
  if (lo.size() < 3) return false;
  for (unsigned k = 0; k < 12; ++k) {
    const std::string full = kMonFull[k];
    if (lo.size() <= full.size() && full.compare(0, lo.size(), lo) == 0) {
      *out = k + 1;
      return true;
    }
  }
  return false;
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// `allowStar` permits '*' in the YEAR position only (measured: `*jan01` under
// ymd is accepted, `jan01*` is not).
bool parseDateToken(const std::string& tok, const std::string& order, bool allowStar,
                    PartialDate* out) {
  std::vector<std::string> f;
  if (!splitDateToken(tok, order, &f)) return false;
  std::string ys, ms, ds;
  for (int k = 0; k < 3; ++k) {
    const char c = order[static_cast<size_t>(k)];
    const std::string& v = f[static_cast<size_t>(k)];
    if (c == 'y') ys = v;
    else if (c == 'm') ms = v;
    else ds = v;
  }
  PartialDate pd;
  if (ys == "*") {
    if (!allowStar) return false;
    pd.starYear = true;
  } else {
    if (!allDigits(ys)) return false;
    pd.y = std::atoi(ys.c_str());
  }
  if (!monthFromField(ms, &pd.m)) return false;
  if (!allDigits(ds)) return false;
  const int dd = std::atoi(ds.c_str());
  if (dd < 1 || dd > 31) return false;
  pd.d = static_cast<unsigned>(dd);
  if (!pd.starYear && pd.d > static_cast<unsigned>(daysInMonth(pd.y, pd.m))) return false;
  *out = pd;
  return true;
}

// --- if-clause -------------------------------------------------------------
struct IfClause {
  bool any = false;
  std::set<int> dows, months, years;
  bool hasDow = false, hasMonth = false, hasYear = false;
};

bool nameToDow(const std::string& raw, int* out) {
  const std::string lo = lowerAscii(raw);
  if (allDigits(lo)) {
    int v = std::atoi(lo.c_str());
    if (v < 0 || v > 6) return false;
    *out = v;
    return true;
  }
  if (lo.size() < 2) return false;  // one character is rejected (measured)
  for (int k = 0; k < 7; ++k) {
    const std::string full = kDowFull[k];
    if (lo.size() <= full.size() && full.compare(0, lo.size(), lo) == 0) {
      *out = k;
      return true;
    }
  }
  return false;
}

// Parse `if dow(..) & month(..) & year(..)` from the tail token list.
bool parseIfClause(const std::vector<std::string>& toks, size_t start, IfClause* out,
                   std::string* err) {
  const std::string rest = joinTokens(toks, start);
  // Split on '&'. '|' is refused (measured).
  if (rest.find('|') != std::string::npos) {
    *err = "only & may join if() conditions in an stbcal file";
    return false;
  }
  std::vector<std::string> parts = splitOn(rest, '&');
  for (std::string p : parts) {
    // trim
    size_t a = p.find_first_not_of(" ");
    size_t b = p.find_last_not_of(" ");
    if (a == std::string::npos) {
      *err = "empty condition in if";
      return false;
    }
    p = p.substr(a, b - a + 1);
    const size_t open = p.find('(');
    if (open == std::string::npos || p.back() != ')') {
      *err = "`" + p + "' where dow()/month()/year() expected";
      return false;
    }
    const std::string fn = lowerAscii(p.substr(0, open));
    const std::string inner = p.substr(open + 1, p.size() - open - 2);
    std::vector<std::string> items = tokensOf(inner);
    if (items.empty()) {
      *err = "empty " + fn + "() list";
      return false;
    }
    if (fn == "dow") {
      out->hasDow = true;
      for (const auto& it : items) {
        int v = 0;
        if (!nameToDow(it, &v)) {
          *err = "`" + it + "' is not a day of week";
          return false;
        }
        out->dows.insert(v);
      }
    } else if (fn == "month") {
      out->hasMonth = true;
      for (const auto& it : items) {
        unsigned v = 0;
        if (!monthFromField(it, &v)) {
          *err = "`" + it + "' is not a month";
          return false;
        }
        out->months.insert(static_cast<int>(v));
      }
    } else if (fn == "year") {
      out->hasYear = true;
      for (const auto& it : items) {
        if (!allDigits(it)) {
          *err = "`" + it + "' is not a year";
          return false;
        }
        out->years.insert(std::atoi(it.c_str()));
      }
    } else {
      *err = fn + "() is not allowed in an stbcal if";
      return false;
    }
  }
  out->any = true;
  return true;
}

}  // namespace

std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468 + kStataEpochShift;
}

int dowOf(std::int64_t td) {
  // 01jan1960 (td 0) is a Friday -> dow 5.
  std::int64_t r = (td + 5) % 7;
  if (r < 0) r += 7;
  return static_cast<int>(r);
}

std::string renderUnderDateformat(std::int64_t td, const std::string& dateformat) {
  int y;
  unsigned m, d;
  civilFromDaysLocal(td - kStataEpochShift, y, m, d);
  char yb[16], mb[8], db[8];
  std::snprintf(yb, sizeof(yb), "%04d", y);
  std::snprintf(mb, sizeof(mb), "%s", kMon[m - 1]);
  std::snprintf(db, sizeof(db), "%02u", d);
  std::string out;
  const std::string order = dateformat.empty() ? std::string("ymd") : dateformat;
  for (char c : order) {
    if (c == 'y') out += yb;
    else if (c == 'm') out += mb;
    else out += db;
  }
  return out;
}

namespace {

struct ParserState {
  std::string dateformat = "dmy";  // MEASURED loader default (the manual says
                                   // ymd and is wrong for this loader).
  bool haveRange = false, haveCenter = false;
  std::int64_t lo = 0, hi = 0, center = 0;
  std::set<std::int64_t> omitted;
};

bool inWindow(std::int64_t d, bool haveLo, std::int64_t wLo, bool haveHi, std::int64_t wHi) {
  if (haveLo && d < wLo) return false;
  if (haveHi && d > wHi) return false;
  return true;
}

bool ifAccepts(const IfClause& c, std::int64_t td) {
  if (!c.any) return true;
  int y;
  unsigned m, d;
  civilFromDaysLocal(td - kStataEpochShift, y, m, d);
  if (c.hasDow && c.dows.count(dowOf(td)) == 0) return false;
  if (c.hasMonth && c.months.count(static_cast<int>(m)) == 0) return false;
  if (c.hasYear && c.years.count(y) == 0) return false;
  return true;
}

// `and pmlist` -> the signed offsets. Accepts `and -1` and `and (-2 -1)`.
bool parseAndList(const std::vector<std::string>& toks, size_t start, size_t stop,
                  std::vector<int>* out, std::string* err) {
  std::string body = joinTokens(std::vector<std::string>(toks.begin(), toks.begin() + static_cast<long>(stop)), start);
  size_t a = body.find_first_not_of(" ");
  if (a == std::string::npos) {
    *err = "and: nothing to add";
    return false;
  }
  size_t b = body.find_last_not_of(" ");
  body = body.substr(a, b - a + 1);
  if (body.front() == '(' && body.back() == ')') body = body.substr(1, body.size() - 2);
  for (const auto& t : tokensOf(body)) {
    if (t.size() < 2 || (t[0] != '+' && t[0] != '-') || !allDigits(t.substr(1))) {
      *err = "`" + t + "' found where +# or -# expected";
      return false;
    }
    int v = std::atoi(t.substr(1).c_str());
    out->push_back(t[0] == '-' ? -v : v);
  }
  return !out->empty();
}

}  // namespace

ParseResult parseStbcal(const std::string& text, const std::string& name) {
  ParseResult res;
  auto fail = [&](const std::string& msg) {
    res.ok = false;
    res.err.rc = 197;
    res.err.message = msg;
    return res;
  };
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    return fail("stbcal file begins with a UTF-8 BOM");
  }
  ParserState st;

  // Every non-range/centerdate/omit line first; the omit lines are applied in a
  // SECOND pass because they need the range to be known (the loader requires
  // `range` to precede them anyway, so one pass would do -- two passes is only
  // how the "range must come first" rule is enforced without ordering games).
  struct OmitLine {
    std::vector<std::string> toks;
    bool haveWinLo = false, haveWinHi = false;
    std::int64_t winLo = 0, winHi = 0;
    bool windowed = false;
  };
  std::vector<OmitLine> omits;

  std::vector<std::string> lines;
  {
    std::string cur;
    for (char c : text) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else if (c != '\r') {
        cur += c;
      }
    }
    if (!cur.empty()) lines.push_back(cur);
  }

  for (const std::string& raw : lines) {
    // A line whose first non-blank character is `*` is a comment.
    size_t firstNb = raw.find_first_not_of(" \t");
    if (firstNb == std::string::npos) continue;  // blank
    if (raw[firstNb] == '*') continue;
    std::vector<std::string> t = tokensOf(raw);
    if (t.empty()) continue;

    // Optional `from A to B:` prefix.
    OmitLine win;
    size_t k = 0;
    if (lowerAscii(t[0]) == "from") {
      if (t.size() < 4 || lowerAscii(t[2]) != "to") return fail("from _ to _: malformed");
      std::string b = t[3];
      if (!b.empty() && b.back() == ':') {
        b.pop_back();
      } else if (t.size() > 4 && t[4] == ":") {
        // `from a to b :` -- the colon as its own token.
      } else {
        return fail("from _ to _ requires a trailing colon");
      }
      if (t[1] != ".") {
        PartialDate pd;
        if (!parseDateToken(t[1], st.dateformat, false, &pd))
          return fail("`" + t[1] + "' is not a " + st.dateformat + " date");
        win.haveWinLo = true;
        win.winLo = daysFromCivil(pd.y, pd.m, pd.d);
      }
      if (b != ".") {
        PartialDate pd;
        if (!parseDateToken(b, st.dateformat, false, &pd))
          return fail("`" + b + "' is not a " + st.dateformat + " date");
        win.haveWinHi = true;
        win.winHi = daysFromCivil(pd.y, pd.m, pd.d);
      }
      if (win.haveWinLo && win.haveWinHi && win.winHi < win.winLo)
        return fail("from _ to _: specified date range invalid");
      win.windowed = true;
      k = (t.size() > 4 && t[4] == ":") ? 5 : 4;
      if (k >= t.size()) return fail("from _ to _: nothing follows the colon");
    }

    const std::string kw = t[k];
    if (kw == "version") {
      if (k != 0) return fail("version cannot carry a from _ to _ prefix");
      if (t.size() != 2) return fail("version: one version number expected");
      const std::string& v = t[1];
      // 12 .. 19.5; a second dot (19.5.1) is refused.
      if (std::count(v.begin(), v.end(), '.') > 1) return fail("version " + v + " is not valid");
      char* endp = nullptr;
      const double num = std::strtod(v.c_str(), &endp);
      if (endp == v.c_str() || *endp != '\0') return fail("version " + v + " is not valid");
      if (num < 12.0 || num > 19.50 + 1e-9)
        return fail("version must be between 12 and 19.50");
      continue;
    }
    if (kw == "purpose") {
      if (k != 0) return fail("purpose cannot carry a from _ to _ prefix");
      std::string body = joinTokens(t, k + 1);
      if (body.size() >= 3 && body.compare(0, 2, "`\"") == 0 &&
          body.compare(body.size() - 2, 2, "\"'") == 0) {
        body = body.substr(2, body.size() - 4);
      } else if (body.size() >= 2 && body.front() == '"' && body.back() == '"') {
        body = body.substr(1, body.size() - 2);
      } else if (body.find(' ') != std::string::npos) {
        return fail("purpose: a multi-word purpose must be quoted");
      }
      if (body.size() > 63) return fail("purpose is limited to 63 characters");
      res.cal.purpose = body;
      continue;
    }
    if (kw == "dateformat") {
      if (k != 0) return fail("dateformat cannot carry a from _ to _ prefix");
      if (t.size() != 2) return fail("dateformat: one order expected");
      static const std::set<std::string> kOrders = {"ymd", "ydm", "myd", "mdy", "dym", "dmy"};
      if (kOrders.count(t[1]) == 0) return fail(t[1] + " found where dateformat expected");
      st.dateformat = t[1];
      continue;
    }
    if (kw == "range") {
      if (k != 0) return fail("range cannot carry a from _ to _ prefix");
      if (st.haveRange) return fail("range has already been set");
      if (t.size() != 3) return fail("range: two dates expected");
      PartialDate a, b;
      if (!parseDateToken(t[1], st.dateformat, false, &a))
        return fail("`" + t[1] + "' is not a " + st.dateformat + " date");
      if (!parseDateToken(t[2], st.dateformat, false, &b))
        return fail("`" + t[2] + "' is not a " + st.dateformat + " date");
      st.lo = daysFromCivil(a.y, a.m, a.d);
      st.hi = daysFromCivil(b.y, b.m, b.d);
      if (st.hi <= st.lo) return fail("top date must be after the bottom date in range");
      st.haveRange = true;
      continue;
    }
    if (kw == "centerdate") {
      if (k != 0) return fail("centerdate cannot carry a from _ to _ prefix");
      if (!st.haveRange) return fail("centerdate before range");
      if (st.haveCenter) return fail("centerdate has already been set");
      if (t.size() != 2) return fail("centerdate: one date expected");
      PartialDate c;
      if (!parseDateToken(t[1], st.dateformat, false, &c))
        return fail("`" + t[1] + "' is not a " + st.dateformat + " date");
      st.center = daysFromCivil(c.y, c.m, c.d);
      if (st.center < st.lo || st.center > st.hi) return fail("centerdate out of range");
      st.haveCenter = true;
      continue;
    }
    if (kw == "omit") {
      if (!st.haveRange) return fail("omit before range");
      win.toks.assign(t.begin() + static_cast<long>(k), t.end());
      omits.push_back(win);
      continue;
    }
    return fail(kw + " invalid stbcal command");
  }

  if (!st.haveRange) return fail("range undefined");
  if (!st.haveCenter) return fail("centerdate undefined");

  int rlY = 0, rhY = 0;
  {
    unsigned m, d;
    civilFromDaysLocal(st.lo - kStataEpochShift, rlY, m, d);
    civilFromDaysLocal(st.hi - kStataEpochShift, rhY, m, d);
  }

  // --- apply the omit lines ------------------------------------------------
  for (const OmitLine& ol : omits) {
    // A `from _ to _:` endpoint that lies outside the calendar's own range is
    // rc 197 "specified date range invalid" (measured, h_fromout: a 2019
    // window on a 2020-2021 calendar).
    if (ol.haveWinLo && (ol.winLo < st.lo || ol.winLo > st.hi))
      return fail("from _ to _: specified date range invalid");
    if (ol.haveWinHi && (ol.winHi < st.lo || ol.winHi > st.hi))
      return fail("from _ to _: specified date range invalid");
    const std::vector<std::string>& t = ol.toks;  // t[0] == "omit"
    if (t.size() < 2) return fail("omit: dayofweek | date | dowinmonth expected");
    const std::string what = t[1];
    // Split off a trailing `if ...`, and a trailing `and ...` before it.
    size_t ifAt = t.size(), andAt = t.size();
    for (size_t i = 2; i < t.size(); ++i) {
      if (t[i] == "if" && ifAt == t.size()) ifAt = i;
    }
    for (size_t i = 2; i < (ifAt == t.size() ? t.size() : ifAt); ++i) {
      if (t[i] == "and" && andAt == t.size()) andAt = i;
    }
    IfClause cond;
    if (ifAt != t.size()) {
      std::string err;
      if (!parseIfClause(t, ifAt + 1, &cond, &err)) return fail(err);
    }
    const size_t bodyEnd = std::min(andAt, ifAt);
    std::vector<int> offsets;
    if (andAt != t.size()) {
      std::string err;
      if (!parseAndList(t, andAt + 1, ifAt, &offsets, &err)) return fail(err);
    }

    if (what == "dayofweek") {
      if (andAt != t.size()) return fail("omit dayofweek does not take `and'");
      std::string body = joinTokens(std::vector<std::string>(t.begin(), t.begin() + static_cast<long>(bodyEnd)), 2);
      size_t a = body.find_first_not_of(" ");
      if (a == std::string::npos) return fail("omit dayofweek: no days named");
      size_t b = body.find_last_not_of(" ");
      body = body.substr(a, b - a + 1);
      const bool paren = body.front() == '(' && body.back() == ')';
      if (paren) body = body.substr(1, body.size() - 2);
      std::vector<std::string> names = tokensOf(body);
      if (names.size() > 1 && !paren)
        return fail("omit dayofweek: a list of days must be parenthesised");
      std::set<int> dows;
      for (const auto& nm : names) {
        if (nm.find(',') != std::string::npos)
          return fail("omit dayofweek: commas are not allowed");
        int v = 0;
        if (!nameToDow(nm, &v)) return fail("`" + nm + "' is not a day of week");
        // A repeated day in one list is rc 197 (measured, h_dowdup
        // `omit dayofweek (Sa Sa Su)`) -- unlike a repeated `omit date`, which
        // the manual explicitly allows.
        if (!dows.insert(v).second) return fail(std::string(kDowOut[v]) + " is repeated");
      }
      for (std::int64_t d = st.lo; d <= st.hi; ++d) {
        if (dows.count(dowOf(d)) == 0) continue;
        if (ol.windowed && !inWindow(d, ol.haveWinLo, ol.winLo, ol.haveWinHi, ol.winHi)) continue;
        if (!ifAccepts(cond, d)) continue;
        st.omitted.insert(d);
      }
      continue;
    }

    if (what == "date") {
      if (bodyEnd != 3) return fail("omit date: one date expected");
      PartialDate pd;
      if (!parseDateToken(t[2], st.dateformat, true, &pd))
        return fail("`" + t[2] + "' is not a " + st.dateformat + " date");
      if (!pd.starYear && ol.windowed) {
        // MEASURED (REFUTE-0 T6, z_win.stbcal): rc 197 -- "from _ to _ serves
        // no purpose ... specify a partial date with the year specified as *
        // or omit the from _ to _".
        return fail("from _ to _ serves no purpose on an `omit date' carrying a full date");
      }
      std::vector<std::int64_t> bases;
      if (pd.starYear) {
        for (int y = rlY; y <= rhY; ++y) {
          if (pd.d > static_cast<unsigned>(daysInMonth(y, pd.m))) continue;  // *feb29
          bases.push_back(daysFromCivil(y, pd.m, pd.d));
        }
      } else {
        bases.push_back(daysFromCivil(pd.y, pd.m, pd.d));
      }
      for (std::int64_t base : bases) {
        if (ol.windowed && !inWindow(base, ol.haveWinLo, ol.winLo, ol.haveWinHi, ol.winHi))
          continue;
        if (!ifAccepts(cond, base)) continue;
        // The base date and every `and` offset. Offsets may land OUTSIDE the
        // window -- measured (REFUTE-0 T7).
        if (base >= st.lo && base <= st.hi) st.omitted.insert(base);
        for (int off : offsets) {
          const std::int64_t o = base + off;
          if (o >= st.lo && o <= st.hi) st.omitted.insert(o);
        }
      }
      continue;
    }

    if (what == "dowinmonth") {
      // omit dowinmonth pm# dow [of monthlist] [and pmlist] [if ...]
      if (bodyEnd < 4) return fail("omit dowinmonth: `+# dow' expected");
      const std::string& pm = t[2];
      if (pm.size() < 2 || (pm[0] != '+' && pm[0] != '-') || !allDigits(pm.substr(1)))
        return fail("`" + pm + "' found where +# or -# expected");
      const int occ = (pm[0] == '-' ? -1 : 1) * std::atoi(pm.substr(1).c_str());
      int dow = 0;
      if (!nameToDow(t[3], &dow)) return fail("`" + t[3] + "' is not a day of week");
      std::set<int> months;
      if (bodyEnd > 4) {
        if (t[4] != "of") return fail("omit dowinmonth: `of' expected");
        std::string body = joinTokens(
            std::vector<std::string>(t.begin(), t.begin() + static_cast<long>(bodyEnd)), 5);
        size_t a = body.find_first_not_of(" ");
        if (a == std::string::npos) return fail("omit dowinmonth: `of' with no months");
        size_t b = body.find_last_not_of(" ");
        body = body.substr(a, b - a + 1);
        if (body.front() == '(' && body.back() == ')') body = body.substr(1, body.size() - 2);
        for (const auto& nm : tokensOf(body)) {
          unsigned mv = 0;
          if (!monthFromField(nm, &mv)) return fail("`" + nm + "' is not a month");
          months.insert(static_cast<int>(mv));
        }
      } else {
        for (int m = 1; m <= 12; ++m) months.insert(m);
      }
      // Walk every (year, month) the range touches.
      for (int y = rlY; y <= rhY; ++y) {
        for (int m : months) {
          const int dim = daysInMonth(y, static_cast<unsigned>(m));
          if (dim == 0) continue;
          std::vector<std::int64_t> hits;
          for (int d = 1; d <= dim; ++d) {
            const std::int64_t td = daysFromCivil(y, static_cast<unsigned>(m),
                                                  static_cast<unsigned>(d));
            if (dowOf(td) == dow) hits.push_back(td);
          }
          std::int64_t base = 0;
          if (occ > 0) {
            if (static_cast<size_t>(occ) > hits.size()) continue;  // +0/+6 omit nothing
            base = hits[static_cast<size_t>(occ - 1)];
          } else if (occ < 0) {
            if (static_cast<size_t>(-occ) > hits.size()) continue;
            base = hits[hits.size() - static_cast<size_t>(-occ)];
          } else {
            continue;  // +0 silently omits nothing (measured)
          }
          if (ol.windowed && !inWindow(base, ol.haveWinLo, ol.winLo, ol.haveWinHi, ol.winHi))
            continue;
          if (!ifAccepts(cond, base)) continue;
          if (base >= st.lo && base <= st.hi) st.omitted.insert(base);
          for (int off : offsets) {
            const std::int64_t o = base + off;
            if (o >= st.lo && o <= st.hi) st.omitted.insert(o);
          }
        }
      }
      continue;
    }
    return fail("omit " + what + " is not an stbcal omit form");
  }

  // --- materialise -----------------------------------------------------------
  if (st.omitted.count(st.lo) != 0)
    return fail("minimum date out of range: the date specified, " +
                renderUnderDateformat(st.lo, "dmy") + ", is a business holiday");
  if (st.omitted.count(st.hi) != 0)
    return fail("maximum date out of range: the date specified, " +
                renderUnderDateformat(st.hi, "dmy") + ", is a business holiday");
  if (st.omitted.count(st.center) != 0) return fail("calendar invalid: centerdate is omitted");

  res.cal.lo = st.lo;
  res.cal.hi = st.hi;
  res.cal.center = st.center;
  res.cal.name = name;
  res.cal.days.reserve(static_cast<size_t>(st.hi - st.lo + 1));
  for (std::int64_t d = st.lo; d <= st.hi; ++d) {
    if (st.omitted.count(d) != 0) continue;
    res.cal.idx.emplace(d, static_cast<std::int64_t>(res.cal.days.size()));
    res.cal.days.push_back(d);
  }
  auto it = res.cal.idx.find(st.center);
  if (it == res.cal.idx.end()) return fail("calendar invalid: centerdate is not a business day");
  res.cal.centerPos = it->second;
  res.cal.minTb = -res.cal.centerPos;
  res.cal.maxTb = static_cast<std::int64_t>(res.cal.days.size()) - 1 - res.cal.centerPos;
  res.ok = true;
  return res;
}

// --- index arithmetic -------------------------------------------------------
double bofd(const BusinessCalendar& cal, double d) {
  if (std::isnan(d)) return std::nan("");
  if (!std::isfinite(d)) return std::nan("");
  const double tr = std::trunc(d);
  if (std::fabs(tr) > 4.0e6) return std::nan("");
  const std::int64_t td = static_cast<std::int64_t>(tr);
  if (td < cal.lo || td > cal.hi) return std::nan("");
  auto it = cal.idx.find(td);
  if (it == cal.idx.end()) return std::nan("");  // an omitted day is missing
  return static_cast<double>(it->second - cal.centerPos);
}

double dofb(const BusinessCalendar& cal, double i) {
  if (std::isnan(i)) return std::nan("");
  if (!std::isfinite(i)) return std::nan("");
  const double tr = std::trunc(i);
  if (std::fabs(tr) > 4.0e9) return std::nan("");
  const std::int64_t pos = static_cast<std::int64_t>(tr) + cal.centerPos;
  if (pos < 0 || pos >= static_cast<std::int64_t>(cal.days.size())) return std::nan("");
  return static_cast<double>(cal.days[static_cast<size_t>(pos)]);
}

std::optional<std::int64_t> readWholeDate(const std::string& token, const std::string& dateformat) {
  PartialDate parsed;
  if (!parseDateToken(token, dateformat, /*allowStar=*/false, &parsed)) return std::nullopt;
  return daysFromCivil(parsed.y, parsed.m, parsed.d);
}

// --- `bcal create` ----------------------------------------------------------
namespace {

// The %td day a date value displays as. A value need not be whole.
std::int64_t dayOfValue(double value) { return static_cast<std::int64_t>(std::floor(value)); }

// `omit dayofweek` writes its codes Monday first and Sunday last.
constexpr struct {
  int dow;
  const char* code;
} kWeekOrder[] = {{1, "Mo"}, {2, "Tu"}, {3, "We"}, {4, "Th"}, {5, "Fr"}, {6, "Sa"}, {0, "Su"}};

// What the sample says about the days between its values. Holes are read off the
// differences between neighbouring sorted values, so on non-whole data a day can
// be a "hole" although a value falls on it, and the reverse.
class HoleFinder {
 public:
  explicit HoleFinder(std::int64_t maxgap) : widest_(static_cast<double>(maxgap) + 1.0) {}

  // Every day inside the hole of `width` days that ends at `anchor`, nearest the
  // earlier edge first. The anchor's own day is a business day.
  void addHole(double anchor, double width) {
    if (width > widest_) {
      tooWide_ = true;
      return;
    }
    for (double back = width; back > 1.0; back -= 1.0) days_.push_back(dayOfValue(anchor - back + 1.0));
  }

  bool tooWide() const { return tooWide_; }
  const std::vector<std::int64_t>& days() const { return days_; }

 private:
  double widest_;
  bool tooWide_ = false;
  std::vector<std::int64_t> days_;
};

}  // namespace

InduceResult induceFromDates(const std::vector<double>& valuesIn, const InduceOpts& opts) {
  InduceResult result;
  const auto refused = [&result](int rc, std::string message) {
    result.rc = rc;
    result.message = std::move(message);
    return result;
  };
  if (opts.maxgap < 1) return refused(198, "maxgap() should be at least 1");
  if (valuesIn.empty()) return refused(2000, "no observations found for calendar");

  // Ascending with missing last; a missing value then fails the upper bound.
  std::vector<double> dates = valuesIn;
  const auto firstMissing =
      std::partition(dates.begin(), dates.end(), [](double v) { return !std::isnan(v); });
  std::sort(dates.begin(), firstMissing);
  const double earliest = dates.front(), latest = dates.back();

  // Measured bounds. The lower one is not the 01jan0100 the message names.
  if (earliest < -676350.0) return refused(198, "Stata date cannot be before Jan 1, 0100");
  if (std::isnan(latest) || latest > 2936549.0)
    return refused(198, "Stata date cannot be after Dec 31, 9999");

  const std::string order = opts.dateformat.empty() ? std::string("ymd") : opts.dateformat;
  const std::int64_t rangeStart = opts.haveRangeLo ? opts.rangeLo : dayOfValue(earliest);
  const std::int64_t rangeEnd = opts.haveRangeHi ? opts.rangeHi : dayOfValue(latest);
  if (opts.haveRangeLo && static_cast<double>(rangeStart) > earliest)
    return refused(198, "first date in range() cannot be after first business date in dataset");
  if (opts.haveRangeHi && static_cast<double>(rangeEnd) < latest)
    return refused(198, "second date in range() cannot be before last business date in dataset");
  const std::int64_t center = opts.haveCenter ? opts.center : rangeStart;
  if (center < rangeStart || center > rangeEnd)
    return refused(198, "centerdate " + renderUnderDateformat(center, order) + " out of range");

  HoleFinder holes(opts.maxgap);
  const double lowEdge = static_cast<double>(rangeStart), highEdge = static_cast<double>(rangeEnd);
  if (earliest > lowEdge) holes.addHole(earliest, earliest - lowEdge);
  for (size_t i = 1; i < dates.size(); ++i) {
    // Only a non-whole last value with no range end can lie past the range; it
    // then takes no part in the walk at all.
    if (dates[i] < lowEdge || dates[i] > highEdge) continue;
    holes.addHole(dates[i], dates[i] - dates[i - 1]);
  }
  if (highEdge > latest) holes.addHole(highEdge, highEdge - latest);
  if (holes.tooWide()) {
    return refused(198, "gap in business calendar too large; maxgap() currently set to " +
                            std::to_string(opts.maxgap));
  }

  bool weekdaySeen[7] = {false, false, false, false, false, false, false};
  for (double v : dates) weekdaySeen[dowOf(dayOfValue(v))] = true;
  std::string absentCodes;
  for (const auto& weekday : kWeekOrder) {
    if (weekdaySeen[weekday.dow]) continue;
    if (!absentCodes.empty()) absentCodes += ' ';
    absentCodes += weekday.code;
  }

  std::string version = std::to_string(opts.stataVersion10 / 10);
  if (opts.stataVersion10 % 10 != 0) version += "." + std::to_string(opts.stataVersion10 % 10);

  std::ostringstream file;
  file << "* Business calendar \"" << opts.calName << "\" created by -bcal create-\n"
       << opts.todayLine << "\n\nversion " << version << "\n";
  if (!opts.purpose.empty()) file << "purpose \"" << opts.purpose << "\"\n";
  file << "dateformat " << order << "\n\nrange " << renderUnderDateformat(rangeStart, order) << " "
       << renderUnderDateformat(rangeEnd, order) << "\ncenterdate "
       << renderUnderDateformat(center, order) << "\n\n";
  if (!absentCodes.empty()) file << "omit dayofweek (" << absentCodes << ")\n";
  for (std::int64_t day : holes.days()) {
    if (!weekdaySeen[dowOf(day)]) continue;  // the weekday line already omits it
    file << "omit date " << renderUnderDateformat(day, order) << "\n";
  }

  result.ok = true;
  result.text = file.str();
  result.leadNote = earliest > lowEdge;
  result.trailNote = highEdge > latest;
  return result;
}

bool isValidCalName(const std::string& name) {
  // The calendar-name rule ([D] bcal: a valid Stata name of at most 10
  // characters). Stata applies it on the read path as well as on create
  // (see bcal.h for the measurement table).
  return strutil::isValidStataIdentifier(name) && name.size() <= 10;
}

std::string searchDirFor(const std::string& sessionCwd) {
  if (!sessionCwd.empty()) return sessionCwd;
  std::error_code ec;
  std::filesystem::path p = std::filesystem::current_path(ec);
  return ec ? std::string(".") : p.u8string();
}

// --- the process cache ------------------------------------------------------
// THREADING INVARIANT: the PIET evaluator is single-threaded. These two
// function-local statics are mutated from dispatch (dispatcher.cpp, the
// depth-0 search-directory assignment), from `cd` (handlers_m3.cpp runCd) and
// from `bcal create`/`bcal load` (handlers_aer_util.cpp), and read from every
// expression surface -- with no mutex, because no evaluator TU starts a thread
// (`parallel` runs its workers as separate PROCESSES). If a future evalRows
// ever becomes threaded, this map is one of the things it has to carry a lock
// for; the note is here so that change trips over the invariant rather than
// over the map.
namespace {
std::string& searchDirStorage() {
  static std::string dir;
  return dir;
}
std::map<std::string, std::shared_ptr<const BusinessCalendar>>& cacheStorage() {
  static std::map<std::string, std::shared_ptr<const BusinessCalendar>> m;
  return m;
}
// Stata's cache is keyed by NAME ALONE, measured both directions (this
// review, /private/tmp/fix-bcal/st/cap.log): create `cdcal` in the cwd, then
// `cd sub` -> bofd is still 1 (B_in_sub), and after a `bcal create cdcal` in
// sub, `cd ..` answers the SUB calendar's -1 (B_back), not the cwd file's 1.
// Erasing the file changes nothing across a cd either (C_after_erase /
// C_in_sub / C_back all 1). Keying by (search dir, name) made a `cd` SILENTLY
// DROP a loaded calendar (missing at rc 0) and a `cd` back resurrect a stale
// one -- both Doctrine-(1) silent answers. Session isolation is NOT this key's
// job: SessionState's constructor clears the cache (session_state.cpp).
std::string cacheKey(const std::string& name) { return lowerAscii(name); }

std::string joinDir(const std::string& dir, const std::string& file) {
  if (dir.empty()) return file;
  if (dir.back() == '/' || dir.back() == '\\') return dir + file;
  return dir + "/" + file;
}
}  // namespace

void setSearchDir(std::string dir) { searchDirStorage() = std::move(dir); }
const std::string& searchDir() { return searchDirStorage(); }
void clearCache() { cacheStorage().clear(); }

std::string calNameFromFormat(const std::string& spec) {
  std::string s = spec;
  if (s.size() >= 3 && s[0] == '%') {
    // %tbNAME / %-9tbNAME
    size_t i = 1;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i + 1 < s.size() && s[i] == 't' && s[i + 1] == 'b') {
      s = s.substr(i + 2);
    } else {
      return std::string();
    }
  } else if (s.size() >= 3 && lowerAscii(s.substr(0, 3)) == "%tb") {
    s = s.substr(3);
  }
  const size_t colon = s.find(':');
  if (colon != std::string::npos) s = s.substr(0, colon);
  return s;
}

void install(const std::string& name, std::shared_ptr<const BusinessCalendar> cal) {
  cacheStorage()[cacheKey(name)] = std::move(cal);
}

std::shared_ptr<const BusinessCalendar> loadFile(const std::string& name, const std::string& path,
                                                 StbcalError* errOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errOut) {
      errOut->rc = 601;
      errOut->message = "file " + path + " not found";
    }
    return nullptr;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  ParseResult pr = parseStbcal(ss.str(), name);
  if (!pr.ok) {
    if (errOut) *errOut = pr.err;
    return nullptr;
  }
  pr.cal.fn = path;
  auto sp = std::make_shared<const BusinessCalendar>(std::move(pr.cal));
  cacheStorage()[cacheKey(name)] = sp;
  return sp;
}

std::shared_ptr<const BusinessCalendar> lookup(const std::string& nameIn) {
  std::string name = nameIn;
  // A leading %tb (with or without a width) names the same calendar.
  if (!name.empty() && name[0] == '%') {
    name = calNameFromFormat(name);
    if (name.empty()) return nullptr;
  }
  if (name.empty()) return nullptr;
  // A ".stbcal" SUFFIX is not a calendar name (measured: missing).
  if (name.size() > 7 && lowerAscii(name.substr(name.size() - 7)) == ".stbcal") return nullptr;
  // The calendar-NAME rule (bcal.h): a name Stata refuses can never resolve,
  // however readable the file next to it is. This is the data-plane half --
  // `bofd("elevenchars", d)` is MISSING with rc 0, measured -- and the command
  // half lives in runBcal, which needs the r(198) message instead of a null.
  if (!isValidCalName(name)) return nullptr;
  auto& cache = cacheStorage();
  auto it = cache.find(cacheKey(name));
  if (it != cache.end()) return it->second;
  StbcalError err;
  return loadFile(name, joinDir(searchDirStorage(), name + ".stbcal"), &err);
}

}  // namespace piet::bcal
