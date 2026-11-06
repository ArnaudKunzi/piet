// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/pyvalue.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace piet::eval {

bool PyValue::truthy() const {
  switch (kind_) {
    case Kind::Bool:
    case Kind::Int:
      return i_ != 0;
    case Kind::Float:
      // NaN is truthy in Python (bool(float('nan')) is True); only 0.0/-0.0
      // are falsy.
      return d_ != 0.0;
    case Kind::Str:
      return !s_.empty();
  }
  return false;
}

bool pyIsInteger(double v) { return std::isfinite(v) && v == std::floor(v); }

std::string pythonIntStrFromDouble(double v) {
  // %.0f prints the exact integer value of the double (libc conversion is
  // exact), matching Python's str(int(v)) for integer-valued floats — even
  // above 2^63 where long long would overflow.
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.0f", v);
  return std::string(buf);
}

std::string pythonFloatRepr(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v > 0 ? "inf" : "-inf";

  // Shortest round-trip digits via to_chars in scientific mode: "d.ddde±X".
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
  std::string sci(buf, res.ptr);

  bool neg = false;
  size_t pos = 0;
  if (sci[0] == '-') {
    neg = true;
    pos = 1;
  }
  // digits: first digit, optional ".ddd", then 'e'
  std::string digits;
  digits.push_back(sci[pos]);
  ++pos;
  if (pos < sci.size() && sci[pos] == '.') {
    ++pos;
    while (pos < sci.size() && sci[pos] != 'e') {
      digits.push_back(sci[pos]);
      ++pos;
    }
  }
  // exponent
  int exp10 = 0;
  {
    auto epos = sci.find('e', pos);
    exp10 = std::atoi(sci.c_str() + epos + 1);
  }

  std::string out;
  if (exp10 >= -4 && exp10 < 16) {
    // Fixed notation.
    if (exp10 >= static_cast<int>(digits.size()) - 1) {
      // All digits before the point; pad zeros; add ".0".
      out = digits + std::string(exp10 - (digits.size() - 1), '0') + ".0";
    } else if (exp10 >= 0) {
      out = digits.substr(0, exp10 + 1) + "." + digits.substr(exp10 + 1);
    } else {
      out = "0." + std::string(-exp10 - 1, '0') + digits;
    }
  } else {
    // Scientific: mantissa "d" or "d.ddd", exponent sign + >=2 digits.
    out = digits.substr(0, 1);
    if (digits.size() > 1) out += "." + digits.substr(1);
    out += "e";
    int ae = exp10;
    if (ae < 0) {
      out += "-";
      ae = -ae;
    } else {
      out += "+";
    }
    char ebuf[16];
    std::snprintf(ebuf, sizeof(ebuf), "%02d", ae);
    out += ebuf;
  }
  return neg ? "-" + out : out;
}

std::string pythonStr(const PyValue& v) {
  switch (v.kind()) {
    case PyValue::Kind::Bool:
      return v.boolVal() ? "True" : "False";
    case PyValue::Kind::Int:
      return std::to_string(v.intVal());
    case PyValue::Kind::Float:
      return pythonFloatRepr(v.floatVal());
    case PyValue::Kind::Str:
      return v.strVal();
  }
  return "";
}

std::optional<double> pythonFloat(const std::string& s) {
  // Python float(): strip whitespace; optional sign; then either
  // inf/infinity/nan (case-insensitive) or a decimal literal with optional
  // exponent; underscores allowed BETWEEN digits.
  size_t a = 0, b = s.size();
  auto isSp = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  while (a < b && isSp(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && isSp(static_cast<unsigned char>(s[b - 1]))) --b;
  if (a >= b) return std::nullopt;
  std::string t = s.substr(a, b - a);

  std::string lower = t;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  double sign = 1.0;
  std::string body = lower;
  if (!body.empty() && (body[0] == '+' || body[0] == '-')) {
    if (body[0] == '-') sign = -1.0;
    body = body.substr(1);
  }
  if (body == "inf" || body == "infinity") return sign * HUGE_VAL;
  if (body == "nan") return std::nan("");

  // Validate decimal form with underscores-between-digits, then strip the
  // underscores and parse with strtod (C locale digits only).
  const std::string& d = body;
  size_t i = 0;
  bool sawDigit = false, sawDot = false, sawExp = false;
  bool prevDigit = false;
  std::string cleaned;
  cleaned.reserve(d.size());
  while (i < d.size()) {
    char c = d[i];
    if (c >= '0' && c <= '9') {
      sawDigit = true;
      prevDigit = true;
      cleaned.push_back(c);
    } else if (c == '_') {
      // Underscore must be BETWEEN digits.
      if (!prevDigit || i + 1 >= d.size() || !(d[i + 1] >= '0' && d[i + 1] <= '9')) {
        return std::nullopt;
      }
      prevDigit = false;
    } else if (c == '.') {
      if (sawDot || sawExp) return std::nullopt;
      sawDot = true;
      prevDigit = false;
      cleaned.push_back(c);
    } else if ((c == 'e' || c == 'E') && !sawExp) {
      if (!sawDigit) return std::nullopt;
      sawExp = true;
      prevDigit = false;
      cleaned.push_back(c);
      if (i + 1 < d.size() && (d[i + 1] == '+' || d[i + 1] == '-')) {
        cleaned.push_back(d[i + 1]);
        ++i;
      }
      // exponent must have at least one digit
      if (i + 1 >= d.size() || !(d[i + 1] >= '0' && d[i + 1] <= '9')) return std::nullopt;
      // digits consumed by loop
      sawDigit = false;  // require exponent digits
    } else {
      return std::nullopt;
    }
    ++i;
  }
  if (!sawDigit) return std::nullopt;
  if (!prevDigit && !cleaned.empty() && cleaned.back() == '.') {
    // trailing '.' is fine ("5." parses); handled below.
  }

  errno = 0;
  char* end = nullptr;
  double parsed = std::strtod(cleaned.c_str(), &end);
  if (end != cleaned.c_str() + cleaned.size()) return std::nullopt;
  // Python float("1e400") == inf (OverflowError only for int()); strtod
  // yields HUGE_VAL with ERANGE — accept, matching Python.
  return sign * parsed;
}

std::optional<double> pythonFloat(const PyValue& v) {
  switch (v.kind()) {
    case PyValue::Kind::Bool:
      return v.boolVal() ? 1.0 : 0.0;
    case PyValue::Kind::Int:
      return static_cast<double>(v.intVal());
    case PyValue::Kind::Float:
      return v.floatVal();
    case PyValue::Kind::Str:
      return pythonFloat(v.strVal());
  }
  return std::nullopt;
}

// --- UTF-8 helpers -----------------------------------------------------------

namespace {
inline bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }
}  // namespace

size_t utf8Len(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!isCont(static_cast<unsigned char>(s[i]))) ++n;
  }
  return n;
}

std::string utf8Slice(const std::string& s, size_t start, size_t stop) {
  if (start >= stop) return "";
  size_t cp = 0;
  size_t byteStart = s.size(), byteStop = s.size();
  bool haveStart = false;
  for (size_t i = 0; i <= s.size(); ++i) {
    bool boundary = (i == s.size()) || !isCont(static_cast<unsigned char>(s[i]));
    if (!boundary) continue;
    if (!haveStart && cp == start) {
      byteStart = i;
      haveStart = true;
    }
    if (cp == stop) {
      byteStop = i;
      break;
    }
    ++cp;
  }
  if (!haveStart) return "";
  return s.substr(byteStart, byteStop - byteStart);
}

long long utf8Find(const std::string& s, const std::string& sub) {
  size_t pos = s.find(sub);
  if (pos == std::string::npos) return -1;
  // Convert byte offset -> codepoint offset.
  long long cp = 0;
  for (size_t i = 0; i < pos; ++i) {
    if (!isCont(static_cast<unsigned char>(s[i]))) ++cp;
  }
  return cp;
}

}  // namespace piet::eval
