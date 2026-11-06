// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The one delimited-text reader (see include/piet/delimited_io.h for the rules
// and the oracle lines they rest on).
#include "piet/delimited_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/ident.h"
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"

namespace piet::delimited {

using ojson = nlohmann::ordered_json;

namespace {

bool isDelimAt(const std::string& text, size_t i, const ReadSpec& spec, size_t* len) {
  if (spec.asString) {
    if (!spec.delims.empty() && text.compare(i, spec.delims.size(), spec.delims) == 0) {
      *len = spec.delims.size();
      return true;
    }
    return false;
  }
  if (spec.delims.find(text[i]) != std::string::npos) {
    *len = 1;
    return true;
  }
  return false;
}

std::string postField(const std::string& raw, const ReadSpec& spec) {
  if (spec.strip == ReadSpec::Strip::kNo) return raw;
  if (spec.strip == ReadSpec::Strip::kYes) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw)
      if (c != '"') out += c;
    return out;
  }
  // Default. Under nobind quotes are data and stay (log:3221 ["plain"]).
  if (spec.bind == ReadSpec::Bind::kNoBind) return raw;
  if (raw.empty() || raw.front() != '"') return raw;  // log:3242 [ab"cd,7] as written
  std::string body = raw.substr(1);
  if (!body.empty() && body.back() == '"') body.pop_back();
  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '"' && i + 1 < body.size() && body[i + 1] == '"') {
      out += '"';
      ++i;
      continue;
    }
    out += body[i];
  }
  return out;
}

bool isBlankRecord(const std::vector<std::string>& rec) {
  return rec.empty() || (rec.size() == 1 && rec[0].empty());
}

std::string trimBlanks(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

// Words Stata will not take as a variable name.
bool isReservedName(const std::string& n) {
  static const std::set<std::string> kReserved = {
      "_all", "_b",    "byte", "_coef", "_cons", "double", "float", "if",   "in",
      "int",  "long",  "_n",   "_N",    "_pi",   "_pred",  "_rc",   "_skip", "strL",
      "using", "with"};
  if (kReserved.count(n)) return true;
  // str1 .. str2045
  if (n.size() > 3 && n.compare(0, 3, "str") == 0 &&
      n.find_first_not_of("0123456789", 3) == std::string::npos)
    return true;
  return false;
}

}  // namespace

std::vector<std::vector<std::string>> parse(const std::string& text, const ReadSpec& spec) {
  std::vector<std::vector<std::string>> out;
  std::vector<std::string> rec;
  std::string cur;
  bool inQuote = false;
  long long quoteLines = 0;
  const bool binds = spec.bind != ReadSpec::Bind::kNoBind;
  auto endField = [&]() {
    rec.push_back(postField(cur, spec));
    cur.clear();
  };
  auto endRecord = [&]() {
    endField();
    if (!(spec.skipEmptyLines && rec.size() == 1 && rec[0].empty())) out.push_back(rec);
    rec.clear();
    inQuote = false;
    quoteLines = 0;
  };
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\r' || c == '\n') {
      const size_t eolLen = (c == '\r' && i + 1 < n && text[i + 1] == '\n') ? 2 : 1;
      if (inQuote && spec.bind == ReadSpec::Bind::kStrict &&
          (spec.maxQuotedRows == 0 || quoteLines + 1 < spec.maxQuotedRows)) {
        // A quoted field spans the line break (log:3185 `line one\nline two`).
        cur += '\n';
        ++quoteLines;
        i += eolLen;
        continue;
      }
      endRecord();
      i += eolLen;
      continue;
    }
    if (binds && c == '"') {
      inQuote = !inQuote;
      cur += c;
      ++i;
      continue;
    }
    size_t dl = 0;
    if (!inQuote && isDelimAt(text, i, spec, &dl)) {
      endField();
      i += dl;
      if (spec.collapse) {
        size_t more = 0;
        while (i < n && isDelimAt(text, i, spec, &more)) i += more;
      }
      continue;
    }
    cur += c;
    ++i;
  }
  if (!cur.empty() || !rec.empty()) {
    // The last line had no terminator; under strict an open quote keeps what
    // it read (log:3185 `ab"cd,7\n`).
    endField();
    if (!(spec.skipEmptyLines && isBlankRecord(rec))) out.push_back(rec);
  }
  return out;
}

InsheetRows parseInsheet(const std::string& text, char delim) {
  InsheetRows out;
  size_t start = 0;
  const size_t n = text.size();
  while (start < n) {
    size_t eol = text.find('\n', start);
    if (eol == std::string::npos) eol = n;
    std::string line = text.substr(start, eol - start);
    start = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> f;
    std::vector<size_t> w;
    if (delim == 0) {
      f.push_back(trimBlanks(line));
      w.push_back(line.size());
    } else {
      size_t pos = 0;
      while (true) {
        size_t j = pos;
        while (j < line.size() && line[j] == ' ') ++j;
        if (j < line.size() && line[j] == '"') {
          size_t q = line.find('"', j + 1);
          // A quote still open at the end of the line runs on into the next
          // line that is not empty, the line end read as a blank. StataBE 19.5
          // insheet: `"multi` / `line",2` is one record with a = "multi line"
          // (strlen 10), `"p` / `` / `q",2` gives "p q", and a quote the file
          // never closes keeps that last blank (`1,"open` / `2,3` gives b =
          // "open 2,3 ", str9).
          while (q == std::string::npos) {
            line += ' ';
            std::string more;
            while (more.empty() && start < n) {
              size_t next = text.find('\n', start);
              if (next == std::string::npos) next = n;
              more = text.substr(start, next - start);
              start = next + 1;
              if (!more.empty() && more.back() == '\r') more.pop_back();
            }
            if (more.empty()) break;
            line += more;
            q = line.find('"', j + 1);
          }
          const std::string content =
              line.substr(j + 1, (q == std::string::npos ? line.size() : q) - (j + 1));
          size_t k = q == std::string::npos ? line.size() : q + 1;
          while (k < line.size() && line[k] == ' ') ++k;
          if (k >= line.size() || line[k] == delim) {
            f.push_back(content);
            w.push_back(content.size());
          } else {
            f.push_back(std::string());
            w.push_back(0);
            k = line.find(delim, k);
            if (k == std::string::npos) k = line.size();
          }
          if (k >= line.size()) break;
          pos = k + 1;
          if (pos >= line.size()) {  // a trailing delimiter: one more empty field
            f.push_back(std::string());
            w.push_back(0);
            break;
          }
          continue;
        }
        const size_t d = line.find(delim, pos);
        const std::string raw = line.substr(pos, (d == std::string::npos ? line.size() : d) - pos);
        f.push_back(trimBlanks(raw));
        w.push_back(raw.size());
        if (d == std::string::npos) break;
        pos = d + 1;
        if (pos >= line.size()) {
          f.push_back(std::string());
          w.push_back(0);
          break;
        }
      }
    }
    out.fields.push_back(std::move(f));
    out.widths.push_back(std::move(w));
  }
  return out;
}

char detectDelimiter(const std::string& text) {
  size_t eol = text.find_first_of("\r\n");
  const std::string first = text.substr(0, eol == std::string::npos ? text.size() : eol);
  static const char kCand[] = {',', '\t', ';', '|'};
  size_t counts[4] = {0, 0, 0, 0};
  bool inQ = false;
  for (char c : first) {
    if (c == '"') { inQ = !inQ; continue; }
    if (inQ) continue;
    for (int k = 0; k < 4; ++k)
      if (c == kCand[k]) ++counts[k];
  }
  int best = 0;
  for (int k = 1; k < 4; ++k)
    if (counts[k] > counts[best]) best = k;
  return counts[best] == 0 ? ',' : kCand[best];
}

bool isMissingToken(const std::string& field) {
  const std::string t = trimBlanks(field);
  if (t.empty() || t == ".") return true;
  return t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z';
}

bool parseNumber(const std::string& field, double* out) {
  const std::string t = trimBlanks(field);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[i] == '+' || t[i] == '-') ++i;
  size_t digits = 0;
  while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; ++digits; }
  if (i < t.size() && t[i] == '.') {
    ++i;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; ++digits; }
  }
  if (digits == 0) return false;
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    size_t j = i + 1;
    if (j < t.size() && (t[j] == '+' || t[j] == '-')) ++j;
    size_t ed = 0;
    while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) { ++j; ++ed; }
    if (ed == 0) return false;
    i = j;
  }
  if (i != t.size()) return false;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(v)) return false;
  *out = v;
  return true;
}

namespace {

struct CaseMapEntry {
  char32_t cp;
  char32_t out[3];  // 0-padded full mapping
};
struct CpRange {
  char32_t lo, hi;
};

const CaseMapEntry kLowerMap[] = {
#define UCASE_LOWER
#include "core/ucase.inc"
#undef UCASE_LOWER
};
const CaseMapEntry kUpperMap[] = {
#define UCASE_UPPER
#include "core/ucase.inc"
#undef UCASE_UPPER
};
const CpRange kCased[] = {
#define UCASE_CASED
#include "core/ucase.inc"
#undef UCASE_CASED
};
const CpRange kIgnorable[] = {
#define UCASE_IGNORABLE
#include "core/ucase.inc"
#undef UCASE_IGNORABLE
};
const CpRange kDigits[] = {
#define UCASE_DIGIT
#include "core/ucase.inc"
#undef UCASE_DIGIT
};

template <size_t N>
bool inRanges(const CpRange (&t)[N], char32_t cp) {
  size_t lo = 0, hi = N;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (cp < t[mid].lo) hi = mid;
    else if (cp > t[mid].hi) lo = mid + 1;
    else return true;
  }
  return false;
}

template <size_t N>
const CaseMapEntry* findMap(const CaseMapEntry (&t)[N], char32_t cp) {
  size_t lo = 0, hi = N;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (cp < t[mid].cp) hi = mid;
    else if (cp > t[mid].cp) lo = mid + 1;
    else return &t[mid];
  }
  return nullptr;
}

void appendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool isCasedCp(char32_t cp) {
  if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
  return inRanges(kCased, cp);
}

// Case-ignorable and not cased (a char that is both counts as cased: StataBE
// lowers "ʰΣ" to "ʰς").
bool isIgnorableCp(char32_t cp) {
  if (cp < 0x80) return cp == '\'' || cp == '.' || cp == ':' || cp == '^' || cp == '`';
  return inRanges(kIgnorable, cp);
}

constexpr char32_t kBadByte = 0xFFFFFFFFu;

}  // namespace

std::string caseMapUtf8(const std::string& s, Case c) {
  if (c == Case::kPreserve) return s;
  std::vector<char32_t> cps;  // kBadByte marks a malformed byte, copied through
  std::vector<unsigned char> raw;
  for (size_t i = 0; i < s.size();) {
    char32_t cp = 0;
    const size_t n = cecon::ident::decodeUtf8(s, i, &cp);
    if (n == 0) {
      cps.push_back(kBadByte);
      raw.push_back(static_cast<unsigned char>(s[i]));
      ++i;
    } else {
      cps.push_back(cp);
      raw.push_back(0);
      i += n;
    }
  }
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < cps.size(); ++i) {
    const char32_t cp = cps[i];
    if (cp == kBadByte) {
      out += static_cast<char>(raw[i]);
      continue;
    }
    if (cp < 0x80) {
      if (c == Case::kLower) out += static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
      else out += static_cast<char>(cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp);
      continue;
    }
    if (c == Case::kLower && cp == 0x3A3) {
      // GREEK CAPITAL SIGMA: final form after a cased letter and not before one.
      bool before = false;
      for (size_t j = i; j > 0;) {
        const char32_t p = cps[--j];
        if (p != kBadByte && isIgnorableCp(p)) continue;
        before = p != kBadByte && isCasedCp(p);
        break;
      }
      bool after = false;
      for (size_t k = i + 1; k < cps.size(); ++k) {
        if (cps[k] != kBadByte && isIgnorableCp(cps[k])) continue;
        after = cps[k] != kBadByte && isCasedCp(cps[k]);
        break;
      }
      appendUtf8(out, before && !after ? 0x3C2 : 0x3C3);
      continue;
    }
    const CaseMapEntry* e = c == Case::kLower ? findMap(kLowerMap, cp) : findMap(kUpperMap, cp);
    if (e == nullptr) {
      appendUtf8(out, cp);
    } else {
      for (char32_t o : e->out)
        if (o != 0) appendUtf8(out, o);
    }
  }
  return out;
}

NameResult legalNames(const std::vector<std::string>& cells, Case c, Fallback f) {
  namespace id = cecon::ident;
  NameResult r;
  std::set<std::string> taken;
  for (size_t k = 0; k < cells.size(); ++k) {
    const std::string& cell = cells[k];
    // StataBE 19.5: keep ASCII letters/digits/_ and any Unicode letter or
    // decimal digit; every other character (space, punctuation, symbols, marks,
    // format characters, malformed bytes) is dropped. Then leading digits of any
    // script go, the rest is cut to 32 CHARACTERS, THEN case-folded (a fold that
    // lengthens it past 32 -- ss for the sharp s -- makes it v<column>; it is not
    // re-cut).
    std::string s;
    bool leading = true;
    for (size_t i = 0; i < cell.size();) {
      char32_t cp = 0;
      const size_t n = id::decodeUtf8(cell, i, &cp);
      if (n == 0) {
        ++i;
        continue;
      }
      bool keep, digit;
      if (cp < 0x80) {
        digit = id::isAsciiDigit(static_cast<unsigned char>(cp));
        keep = id::isAsciiAlpha(static_cast<unsigned char>(cp)) || digit || cp == '_';
      } else {
        digit = inRanges(kDigits, cp);
        keep = digit || id::isUnicodeLetter(cp);
      }
      i += n;
      if (!keep || (leading && digit)) continue;
      leading = false;
      appendUtf8(s, cp);
    }
    s = id::truncateCodePoints(s, 32);
    s = caseMapUtf8(s, c);
    if (s.empty() || id::codePointCount(s) > 32 || isReservedName(s) || taken.count(s)) {
      const std::string v = c == Case::kUpper ? "V" : "v";
      s = v + std::to_string(k + 1);
      if (taken.count(s)) {
        // An earlier column holds v<column> (see Fallback).
        if (f == Fallback::kRefuse) r.clash = true;
        for (size_t j = 1; taken.count(s); ++j) s = v + std::to_string(j);
      }
    }
    taken.insert(s);
    r.names.push_back(s);
    r.labels.push_back(s == cell ? std::string() : cell);
  }
  return r;
}

ColumnType typeColumn(const std::vector<const std::string*>& cells, bool forceString,
                      bool forceNumeric, bool asDouble, size_t widthFloor,
                      bool precisionToDouble) {
  ColumnType t;
  size_t width = widthFloor;
  for (const std::string* s : cells) width = std::max(width, s->size());
  auto asString = [&]() {
    t.isString = true;
    t.stataType = width > 2045 ? std::string("strL")
                               : "str" + std::to_string(std::max<size_t>(1, width));
    return t;
  };
  if (forceString) return asString();
  bool allNum = true, anyNum = false, allInt = true, floatLoses = false;
  double lo = 0.0, hi = 0.0;
  for (const std::string* s : cells) {
    if (isMissingToken(*s)) continue;
    double v = 0.0;
    if (!parseNumber(*s, &v)) {
      allNum = false;
      continue;
    }
    if (!floatLoses && v != std::floor(v)) {
      // Does float storage keep every digit the TEXT gives? Compare v and
      // float(v) at the text's own count of significant digits: `7.0100002`
      // (a float written at 8 digits) survives, `-77.551724137931034` does not.
      int sig = 0;
      bool started = false;
      for (char ch : *s) {
        if (ch == 'e' || ch == 'E') break;
        if (!std::isdigit(static_cast<unsigned char>(ch))) continue;
        if (ch != '0') started = true;
        if (started) ++sig;
      }
      {
        // trailing zeros of the mantissa carry no precision
        std::string mant = *s;
        const size_t e = mant.find_first_of("eE");
        if (e != std::string::npos) mant.resize(e);
        if (mant.find('.') != std::string::npos) {
          size_t z = mant.size();
          while (z > 0 && (mant[z - 1] == '0' || mant[z - 1] == ' ')) { --z; if (sig > 1) --sig; }
        }
      }
      sig = std::max(1, std::min(sig, 17));
      char a[64], b[64];
      std::snprintf(a, sizeof(a), "%.*e", sig - 1, v);
      std::snprintf(b, sizeof(b), "%.*e", sig - 1, static_cast<double>(static_cast<float>(v)));
      if (std::string(a) != std::string(b)) floatLoses = true;
    }
    if (!anyNum) { lo = hi = v; anyNum = true; }
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    if (v != std::floor(v) || std::fabs(v) >= 9007199254740992.0) allInt = false;
  }
  if (!allNum) {
    if (!forceNumeric) return asString();
    t.stataType = asDouble ? "double" : "float";
    return t;
  }
  if (!anyNum) { t.stataType = "byte"; return t; }
  if (allInt) {
    if (lo >= -127 && hi <= 100) t.stataType = "byte";
    else if (lo >= -32767 && hi <= 32740) t.stataType = "int";
    else if (lo >= -2147483647.0 && hi <= 2147483620.0) t.stataType = "long";
    else t.stataType = "double";
    return t;
  }
  // A non-integer column whose magnitude passes float's exact-integer range
  // (2^24) is double whatever asfloat says (aer_w2 w2e_float_default
  // import_default_types `0.1,2.7,1e10` -> double, also under asfloat,
  // w2e_float_default.log:1979/:2089); smaller ones are float (log:3340).
  if (precisionToDouble && (std::max(std::fabs(lo), std::fabs(hi)) > 16777216.0 || floatLoses)) {
    // ...and so is one whose text carries more digits than float keeps
    // (internal probe BUG-406: -77.551724137931034 stays
    // exact).
    t.stataType = "double";
    return t;
  }
  t.stataType = asDouble ? "double" : "float";
  return t;
}

std::optional<ojson> stageAndAdopt(SessionState& session, const Table& t, const std::string& verb,
                                   const std::string& sourceResolved) {
  const std::string csvPath = piet::mintScratchPath(session.workdir(), "import_delim") + ".csv";
  registerScratchMint(csvPath);
  session.noteProjectionScratch(csvPath);
  const size_t k = t.names.size();
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return ojson(eval::engineError(verb + ": cannot stage the rows read", "data_io"));
    std::string line;
    for (size_t c = 0; c < k; ++c) {
      if (c) line += ',';
      line += t.names[c];
    }
    line += '\n';
    f.write(line.data(), static_cast<std::streamsize>(line.size()));
    char buf[64];
    for (const auto& row : t.rows) {
      line.clear();
      for (size_t c = 0; c < k; ++c) {
        if (c) line += ',';
        const std::string empty;
        const std::string& cell = c < row.size() ? row[c] : empty;
        if (t.types[c].isString) {
          // Always quoted: cobra convert trims an unquoted field, and the
          // blanks of `  pad  ` are data (log:2574).
          line += '"';
          for (char ch : cell) {
            if (ch == '"') line += "\"\"";
            else line += ch;
          }
          line += '"';
          continue;
        }
        double v = 0.0;
        if (isMissingToken(cell) || !parseNumber(cell, &v)) continue;  // missing
        if (t.types[c].stataType == "float") v = static_cast<double>(static_cast<float>(v));
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        line += buf;
      }
      line += '\n';
      f.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      return ojson(eval::engineError(verb + ": failed staging the rows read (disk full?)", "data_io"));
    }
  }
  std::string factorList;
  for (size_t c = 0; c < k; ++c) {
    if (!t.types[c].isString) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += t.names[c];
  }
  const std::string hbin = piet::mintScratchPath(session.workdir(), "import_delim");
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    cecon::native_unlink(hbin);
    std::string msg = cr.stderr_text.empty() ? cr.error : cr.stderr_text;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
      msg.pop_back();
    return ojson(eval::engineError(verb + ": staging failed: " + msg, "data_io"));
  }
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.setCurrentFile(sourceResolved);
  // setCurrentFile clears c(changed), as `use` does; data read by an importer
  // have not been saved, so it is 1 (StataBE 19.5, BUG-1478: c(changed) 1 after
  // import delimited, insheet, import excel, infix, infile with a dictionary and
  // describe, replace; 0 after use and sysuse).
  session.datasetChanged = true;
  session.invalidateDatastore();
  for (size_t c = 0; c < k; ++c) {
    session.implicitFloatVars.erase(t.names[c]);
    session.varMeta[t.names[c]].stataType = t.types[c].stataType;
    if (!t.labels[c].empty()) session.setVarLabel(t.names[c], t.labels[c]);
  }
  return std::nullopt;
}

}  // namespace piet::delimited
