// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// PyValue — a Python-object model for the M1 evaluator port.
//
// The Python backend evaluates macro `= expr` arithmetic and expression
// functions through eval() over dynamically-typed values (bool/int/float/
// str). Parity requires reproducing Python's TYPE behavior, not just the
// math: str(42) == "42" but str(42.0) == "42.0", bool is an int subclass,
// repr(float) is the shortest round-trip decimal, etc. PyValue is a tagged
// union carrying exactly that distinction.
//
// Kind propagation mirrors Python:
//   int op int -> int (except / which is always float)
//   bool participates in arithmetic as int (True+1 == 2)
//   str + str -> str; str * int -> str
//
// Overflow caveat: Python ints are arbitrary-precision; PyValue::Int is a
// 64-bit long long. Arithmetic that overflows 64 bits falls back to double
// (documented divergence — the differential corpus keeps integer magnitudes
// below 2^53 where Python and C++ agree exactly).
#ifndef PIET_NATIVE_EVAL_PYVALUE_H
#define PIET_NATIVE_EVAL_PYVALUE_H

#include <cstdint>
#include <optional>
#include <string>

namespace piet::eval {

// Exception stand-in for "Python raised" — pyeval/exprfns code that mirrors
// a Python `raise` throws this; call sites that mirror `except Exception`
// catch it. The message is diagnostic only (Python callers never surface it:
// every consumer catches broadly and falls back).
struct PyExc {
  std::string what;
  explicit PyExc(std::string w) : what(std::move(w)) {}
};

class PyValue {
 public:
  enum class Kind { Bool, Int, Float, Str };

  PyValue() : kind_(Kind::Int), i_(0) {}

  static PyValue boolean(bool b) {
    PyValue v;
    v.kind_ = Kind::Bool;
    v.i_ = b ? 1 : 0;
    return v;
  }
  static PyValue integer(long long i) {
    PyValue v;
    v.kind_ = Kind::Int;
    v.i_ = i;
    return v;
  }
  static PyValue floating(double d) {
    PyValue v;
    v.kind_ = Kind::Float;
    v.d_ = d;
    return v;
  }
  static PyValue str(std::string s) {
    PyValue v;
    v.kind_ = Kind::Str;
    v.s_ = std::move(s);
    return v;
  }

  Kind kind() const { return kind_; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isInt() const { return kind_ == Kind::Int; }
  bool isFloat() const { return kind_ == Kind::Float; }
  bool isStr() const { return kind_ == Kind::Str; }
  bool isNumeric() const { return kind_ != Kind::Str; }

  bool boolVal() const { return i_ != 0; }
  long long intVal() const { return i_; }
  double floatVal() const { return d_; }
  const std::string& strVal() const { return s_; }

  // Numeric value regardless of Bool/Int/Float kind (NOT for Str).
  double numAsDouble() const { return kind_ == Kind::Float ? d_ : static_cast<double>(i_); }

  // Python truthiness: bool(x). NaN is truthy; "" and 0/0.0 are falsy.
  bool truthy() const;

 private:
  Kind kind_;
  long long i_ = 0;
  double d_ = 0.0;
  std::string s_;
};

// --- Python-exact conversions & formatting ---------------------------------

// str(float) / repr(float): shortest round-trip decimal, fixed notation for
// decimal exponent in [-4, 16), otherwise scientific with sign and >=2
// exponent digits. Integer-valued floats get a ".0" suffix in fixed form.
// nan -> "nan", inf -> "inf"/"-inf", -0.0 -> "-0.0".
std::string pythonFloatRepr(double v);

// str(int(v)) for a float known to be integer-valued (exact, even > 2^63).
std::string pythonIntStrFromDouble(double v);

// str(x) for any PyValue: Bool -> "True"/"False", Int -> decimal,
// Float -> pythonFloatRepr, Str -> the string itself.
std::string pythonStr(const PyValue& v);

// float(str) — Python's float() parsing: optional whitespace, sign,
// inf/infinity/nan (case-insensitive), decimal/exponent forms, underscores
// between digits. nullopt == ValueError.
std::optional<double> pythonFloat(const std::string& s);

// float(x) for any PyValue. nullopt == TypeError/ValueError.
std::optional<double> pythonFloat(const PyValue& v);

// float.is_integer()
bool pyIsInteger(double v);

// --- UTF-8 codepoint helpers (Python str ops are codepoint-based) ----------

// len(s) in codepoints (bytes count as 1 each on invalid UTF-8, mirroring
// nothing in particular — corpus inputs are valid UTF-8).
size_t utf8Len(const std::string& s);

// Python s[start:stop] with codepoint indices (already clamped Python-style
// by the caller or here: negative not supported — callers never pass
// negatives in the mirrored code paths).
std::string utf8Slice(const std::string& s, size_t start, size_t stop);

// Python s.find(sub) in codepoints; -1 when absent.
long long utf8Find(const std::string& s, const std::string& sub);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_PYVALUE_H
