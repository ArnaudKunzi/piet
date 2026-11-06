// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M0 -- the native Mata statement subset. Grammar, scope and the namespace rule
// are in piet/mata_subset.h; the design is SPEC.md §5.3 / D2 / D3 of
// internal notes ml-getmata. Oracle:
// tests/eval/golden/aer_w2/w2c_ml_getmata.{do,log} families (8) and (9).
//
// Two phases per line, and the split is the point: every statement of a line
// (or of a folded block) is macro-expanded and PARSED before any of them runs.
// A construct outside M0 is a parse-time fact, so it refuses the whole block
// with nothing assigned; only genuine runtime errors (a join that does not
// conform, a name that is not defined) stop part-way, which is Stata's own
// behaviour for a `mata:` block.
#include "piet/mata_subset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "core/ident.h"
#include "core/stata_arith.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::mata {

using ojson = nlohmann::ordered_json;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------
// Values. EvalMatrix cannot say how many columns a 0-row matrix has, and
// select() on an all-missing vector yields exactly that (0 x 1), so evaluation
// runs on an explicit-shape matrix and converts at the store boundary.

struct MMat {
  size_t r = 0, c = 0;
  std::vector<double> v;  // row-major, r*c
  double at(size_t i, size_t j) const { return v[i * c + j]; }
};

// [M-5] invsym(): the generalized inverse of a symmetric matrix read from its
// upper triangle; an omitted column (and row) is 0, and any missing input
// makes every entry missing. The sweep takes the largest remaining pivot
// first and omits a column whose pivot is <= 1e-9 x its original diagonal or
// below 1e-19, or whose original diagonal is not positive -- the rule measured
// on StataBE for glm's rank (estimation_glm.cpp glmVceRank). StataBE 19.5
// (impl_A probe6.do): invsym((1,2\2,1)) = (1,0\0,0), invsym((-1,0\0,2)) =
// (0,0\0,.5), invsym((2,1\99,3)) = (.6,-.2\-.2,.4), invsym((1e-20,0\0,1)) =
// (0,0\0,1), and a missing cell makes all four cells missing.
MMat invsym(const MMat& in) {
  const size_t k = in.r;
  MMat a{k, k, std::vector<double>(k * k, 0.0)};
  for (size_t i = 0; i < k; ++i)
    for (size_t j = i; j < k; ++j) {
      const double x = in.at(i, j);
      if (std::isnan(x)) return MMat{k, k, std::vector<double>(k * k, kNaN)};
      a.v[i * k + j] = a.v[j * k + i] = x;
    }
  std::vector<double> d0(k);
  std::vector<char> left(k), swept(k, 0);
  for (size_t j = 0; j < k; ++j) {
    d0[j] = a.at(j, j);
    left[j] = d0[j] > 0;
  }
  for (;;) {
    size_t best = k;
    double pivot = 0.0;
    for (size_t j = 0; j < k; ++j) {
      if (!left[j]) continue;
      const double p = a.at(j, j);
      if (p <= 1e-9 * d0[j] || p < 1e-19) continue;
      if (best == k || p > pivot) {
        best = j;
        pivot = p;
      }
    }
    if (best == k) break;
    left[best] = 0;
    swept[best] = 1;
    // Stata's sweep arithmetic, bit for bit on StataBE (handlers_ml.cpp
    // invsymStata): symmetric storage; for i <= j, a_ij -= (a_ip/D)*a_jp,
    // a_ip = a_ip/D, a_pp = -1/D, negated at the end; no fused multiply-add.
    const size_t p = best;
    const double D = a.at(p, p);
    for (size_t i = 0; i < k; ++i) {
      if (i == p) continue;
      const double t = a.at(i, p) / D;
      for (size_t j = i; j < k; ++j) {
        if (j == p) continue;
        const double u = t * a.at(j, p);
        a.v[i * k + j] = a.v[j * k + i] = a.at(i, j) - u;
      }
    }
    for (size_t i = 0; i < k; ++i)
      if (i != p) a.v[i * k + p] = a.v[p * k + i] = a.at(i, p) / D;
    a.v[p * k + p] = -1.0 / D;
  }
  for (double& x : a.v) x = -x;
  for (size_t i = 0; i < k; ++i)
    if (!swept[i])
      for (size_t j = 0; j < k; ++j) a.v[i * k + j] = a.v[j * k + i] = 0.0;
  return a;
}

MMat scalarOf(double x) {
  MMat m;
  m.r = m.c = 1;
  m.v = {x};
  return m;
}

MMat fromEval(const eval::EvalMatrix& e) {
  MMat m;
  m.r = e.data.size();
  m.c = colsOf(e);
  m.v.reserve(m.r * m.c);
  for (const auto& row : e.data) {
    for (size_t j = 0; j < m.c; ++j) m.v.push_back(j < row.size() ? row[j] : kNaN);
  }
  return m;
}

eval::EvalMatrix toEval(const MMat& m) {
  eval::EvalMatrix e;
  e.data.assign(m.r, std::vector<double>(m.c));
  for (size_t i = 0; i < m.r; ++i)
    for (size_t j = 0; j < m.c; ++j) e.data[i][j] = m.at(i, j);
  e.colnames.assign(m.c, std::string());  // carries c for a void 0 x c matrix
  return e;
}

// Arithmetic on a missing operand is missing, and so is a non-finite result
// (Mata's 1:/0 and exp(1000) are both `.`). Canonicalising to the plain NaN
// keeps an extended-missing operand from leaking its payload into the result:
// `.a :- 1` is `.` in Mata, not `.a`.
double clean(double x) { return std::isfinite(x) ? x : kNaN; }

// ---------------------------------------------------------------------------
// Errors. A construct outside M0 is a refusal BY NAME (r(199) through the
// errors.h builder); a Mata runtime error carries Mata's own code as an exact
// rc (`reason` "r(NNN)", which coarseRc honours before anything else).

ojson refuseOutside(const std::string& stmt, const std::string& construct, bool wholeBlock) {
  ojson b = ojson(eval::refusedCommand("mata", "statement outside PIET's native subset: " + stmt));
  std::string msg = b.value("message", std::string());
  msg += " PIET runs a native subset of Mata (M0) and has no Mata runtime; `" + stmt + "` uses " +
         construct + ", which is outside that subset.";
  if (wholeBlock) msg += " The whole `mata:` block was skipped and nothing in it was assigned.";
  b["message"] = msg;
  return b;
}

ojson mataRuntimeError(int rc, const std::string& what, const std::string& stmt) {
  // Stata prints `<istmt>:  3200  conformability error` and then r(3200).
  return ojson(eval::errorBlock("engine_error",
                                "<istmt>:  " + std::to_string(rc) + "  " + what, std::nullopt,
                                std::nullopt, stmt, "r(" + std::to_string(rc) + ")"));
}

// ---------------------------------------------------------------------------
// Lexer.

enum class Tk { Num, Dot, Name, Str, LParen, RParen, LBrack, RBrack, Comma, Backslash, Assign,
                ColonOp, Op, End };

struct Token {
  Tk kind;
  std::string text;
  double num = 0.0;
};

// Bytes >= 0x80 are name bytes (core/ident.h): a Unicode name stays whole.
bool isNameStart(char ch) { return cecon::ident::isIdentStartByte(ch); }
bool isNameChar(char ch) { return cecon::ident::isIdentByte(ch); }

// nullopt + *bad = the construct to name in the refusal.
std::optional<std::vector<Token>> lex(const std::string& s, std::string* bad) {
  std::vector<Token> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char ch = s[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    const bool dotDigit = ch == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1]));
    if (std::isdigit(static_cast<unsigned char>(ch)) || dotDigit) {
      size_t j = i;
      while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      if (j < n && s[j] == '.') {
        ++j;
        while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      }
      if (j < n && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
        if (k < n && std::isdigit(static_cast<unsigned char>(s[k]))) {
          while (k < n && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
          j = k;
        }
      }
      if (j < n && isNameChar(s[j])) {
        *bad = "the token `" + s.substr(i, j + 1 - i) + "`";
        return std::nullopt;
      }
      Token t{Tk::Num, s.substr(i, j - i)};
      t.num = std::strtod(t.text.c_str(), nullptr);
      out.push_back(t);
      i = j;
      continue;
    }
    if (ch == '.') {
      if (i + 1 < n && std::isalpha(static_cast<unsigned char>(s[i + 1]))) {
        *bad = "the extended missing value `." + std::string(1, s[i + 1]) + "`";
        return std::nullopt;
      }
      out.push_back({Tk::Dot, "."});
      ++i;
      continue;
    }
    if (isNameStart(ch)) {
      size_t j = i + 1;
      while (j < n && isNameChar(s[j])) ++j;
      out.push_back({Tk::Name, s.substr(i, j - i)});
      i = j;
      continue;
    }
    if (ch == '"') {
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) {
        *bad = "an unterminated string";
        return std::nullopt;
      }
      out.push_back({Tk::Str, s.substr(i + 1, close - i - 1)});
      i = close + 1;
      continue;
    }
    switch (ch) {
      case '(': out.push_back({Tk::LParen, "("}); ++i; continue;
      case ')': out.push_back({Tk::RParen, ")"}); ++i; continue;
      case '[': out.push_back({Tk::LBrack, "["}); ++i; continue;
      case ']': out.push_back({Tk::RBrack, "]"}); ++i; continue;
      case ',': out.push_back({Tk::Comma, ","}); ++i; continue;
      case '\\': out.push_back({Tk::Backslash, "\\"}); ++i; continue;
      default: break;
    }
    if (ch == '=') {
      if (i + 1 < n && s[i + 1] == '=') {
        out.push_back({Tk::Op, "=="});
        i += 2;
      } else {
        out.push_back({Tk::Assign, "="});
        ++i;
      }
      continue;
    }
    if (ch == ':') {
      // A colon operator is `:` glued to an arithmetic/relational operator.
      static const char* kColonOps[] = {":==", ":!=", ":>=", ":<=", ":+", ":-", ":*",
                                        ":/",  ":^",  ":>",  ":<",  ":&", ":|"};
      bool matched = false;
      for (const char* op : kColonOps) {
        const std::string o(op);
        if (s.compare(i, o.size(), o) == 0) {
          out.push_back({Tk::ColonOp, o});
          i += o.size();
          matched = true;
          break;
        }
      }
      if (matched) continue;
      out.push_back({Tk::Op, (i + 1 < n && s[i + 1] == ':') ? "::" : ":"});
      i += (i + 1 < n && s[i + 1] == ':') ? 2 : 1;
      continue;
    }
    // Everything else is an operator M0 does not have (or punctuation Mata
    // does not have); the parser names it if it is ever reached.
    std::string op(1, ch);
    if (i + 1 < n && (s.substr(i, 2) == "!=" || s.substr(i, 2) == ">=" || s.substr(i, 2) == "<=" ||
                      s.substr(i, 2) == "&&" || s.substr(i, 2) == "||" || s.substr(i, 2) == "..")) {
      op = s.substr(i, 2);
    }
    out.push_back({Tk::Op, op});
    i += op.size();
  }
  out.push_back({Tk::End, ""});
  return out;
}

// ---------------------------------------------------------------------------
// AST.

struct Node {
  enum class K { Num, Missing, Name, Neg, Bin, RowJoin, ColJoin, J, StMatrix, Exp, Select, Rows,
                 Cols, InvSym, Sub };
  K k;
  double num = 0.0;
  std::string text;  // Name / StMatrix name / Bin operator / Select name
  size_t rowSel = 0, colSel = 0;  // Sub: 0 = `.` (all)
  std::vector<std::unique_ptr<Node>> kids;
  explicit Node(K kind) : k(kind) {}
};
using NodeP = std::unique_ptr<Node>;

// A parse failure is ALWAYS "outside M0": PIET does not claim to know that a
// statement it cannot parse is invalid Mata, only that it cannot run it.
struct Parser {
  const std::vector<Token>& t;
  size_t p = 0;
  std::string bad;  // non-empty = the construct to name

  explicit Parser(const std::vector<Token>& toks) : t(toks) {}

  const Token& peek(size_t ahead = 0) const {
    return t[std::min(p + ahead, t.size() - 1)];
  }
  bool fail(const std::string& what) {
    if (bad.empty()) bad = what;
    return false;
  }
  static std::string describe(const Token& tok) {
    switch (tok.kind) {
      case Tk::End: return "an incomplete expression";
      case Tk::Op:
      case Tk::ColonOp: return "the operator `" + tok.text + "`";
      case Tk::Str: return "a string literal";
      case Tk::Assign: return "an assignment inside an expression";
      default: return "`" + tok.text + "` in that position";
    }
  }

  NodeP expr() { return rowjoin(); }

  NodeP rowjoin() {
    NodeP l = coljoin();
    if (!l) return nullptr;
    while (peek().kind == Tk::Backslash) {
      ++p;
      NodeP r = coljoin();
      if (!r) return nullptr;
      auto n = std::make_unique<Node>(Node::K::RowJoin);
      n->kids.push_back(std::move(l));
      n->kids.push_back(std::move(r));
      l = std::move(n);
    }
    return l;
  }

  NodeP coljoin() {
    NodeP l = addop();
    if (!l) return nullptr;
    while (peek().kind == Tk::Comma) {
      ++p;
      NodeP r = addop();
      if (!r) return nullptr;
      auto n = std::make_unique<Node>(Node::K::ColJoin);
      n->kids.push_back(std::move(l));
      n->kids.push_back(std::move(r));
      l = std::move(n);
    }
    return l;
  }

  // Inside a function's argument list a top-level comma separates arguments
  // ([M-2] op_join: "comma as a separator"); `\` still joins.
  NodeP argument() {
    NodeP l = addop();
    if (!l) return nullptr;
    while (peek().kind == Tk::Backslash) {
      ++p;
      NodeP r = addop();
      if (!r) return nullptr;
      auto n = std::make_unique<Node>(Node::K::RowJoin);
      n->kids.push_back(std::move(l));
      n->kids.push_back(std::move(r));
      l = std::move(n);
    }
    return l;
  }

  NodeP binaryLevel(bool additive) {
    NodeP l = additive ? binaryLevel(false) : unary();
    if (!l) return nullptr;
    for (;;) {
      const Token& tok = peek();
      const bool mine = tok.kind == Tk::ColonOp &&
                        (additive ? (tok.text == ":+" || tok.text == ":-")
                                  : (tok.text == ":*" || tok.text == ":/"));
      if (!mine) {
        // A binary operator M0 does not have sits exactly here: name it now,
        // rather than letting the caller report a vaguer "unexpected token".
        if (tok.kind == Tk::ColonOp && tok.text != ":+" && tok.text != ":-" &&
            tok.text != ":*" && tok.text != ":/") {
          fail("the operator `" + tok.text + "`");
          return nullptr;
        }
        if (tok.kind == Tk::Op) {
          fail("the operator `" + tok.text + "`");
          return nullptr;
        }
        return l;
      }
      ++p;
      NodeP r = additive ? binaryLevel(false) : unary();
      if (!r) return nullptr;
      auto n = std::make_unique<Node>(Node::K::Bin);
      n->text = tok.text;
      n->kids.push_back(std::move(l));
      n->kids.push_back(std::move(r));
      l = std::move(n);
    }
  }
  NodeP addop() { return binaryLevel(true); }

  NodeP unary() {
    if (peek().kind == Tk::Op && peek().text == "-") {
      ++p;
      NodeP x = unary();
      if (!x) return nullptr;
      auto n = std::make_unique<Node>(Node::K::Neg);
      n->kids.push_back(std::move(x));
      return n;
    }
    return postfix();
  }

  bool selector(size_t* out) {
    const Token& tok = peek();
    if (tok.kind == Tk::Dot) {
      ++p;
      *out = 0;
      return true;
    }
    if (tok.kind == Tk::Num && tok.num >= 1 && std::floor(tok.num) == tok.num &&
        tok.text.find_first_not_of("0123456789") == std::string::npos) {
      ++p;
      *out = static_cast<size_t>(tok.num);
      return true;
    }
    return fail("a subscript other than `.` or a positive integer");
  }

  NodeP postfix() {
    NodeP base = primary();
    if (!base) return nullptr;
    if (peek().kind != Tk::LBrack) return base;
    ++p;
    auto n = std::make_unique<Node>(Node::K::Sub);
    if (!selector(&n->rowSel)) return nullptr;
    if (peek().kind != Tk::Comma) {
      fail("a subscript that is not [row, column]");
      return nullptr;
    }
    ++p;
    if (!selector(&n->colSel)) return nullptr;
    if (peek().kind != Tk::RBrack) {
      fail("a subscript that is not [row, column]");
      return nullptr;
    }
    ++p;
    if (peek().kind == Tk::LBrack) {
      fail("a repeated subscript");
      return nullptr;
    }
    n->kids.push_back(std::move(base));
    return n;
  }

  bool expect(Tk kind, const std::string& what) {
    if (peek().kind == kind) {
      ++p;
      return true;
    }
    return fail(what);
  }

  // Parses `(a1, a2, ...)` after a function name into `into`.
  bool args(std::vector<NodeP>* into) {
    if (!expect(Tk::LParen, "a function call without parentheses")) return false;
    if (peek().kind == Tk::RParen) {
      ++p;
      return true;
    }
    for (;;) {
      NodeP a = argument();
      if (!a) return false;
      into->push_back(std::move(a));
      if (peek().kind == Tk::Comma) {
        ++p;
        continue;
      }
      return expect(Tk::RParen, describe(peek()));
    }
  }

  NodeP call(const std::string& fn) {
    if (fn == "st_matrix") {
      // Only the READ form with a literal name: st_matrix("NAME").
      ++p;  // (
      if (peek().kind != Tk::Str) {
        fail("st_matrix() with an argument other than a quoted matrix name");
        return nullptr;
      }
      auto n = std::make_unique<Node>(Node::K::StMatrix);
      n->text = strutil::trim(peek().text);
      ++p;
      if (peek().kind == Tk::Comma) {
        fail("st_matrix(name, value) (writing a Stata matrix)");
        return nullptr;
      }
      if (!expect(Tk::RParen, describe(peek()))) return nullptr;
      return n;
    }
    if (fn == "select") {
      // Exactly select(A, rowmissing(A):==0) with the same NAME both times.
      ++p;  // (
      const Token a = peek();
      const bool shape = a.kind == Tk::Name && peek(1).kind == Tk::Comma &&
                         peek(2).kind == Tk::Name && peek(2).text == "rowmissing" &&
                         peek(3).kind == Tk::LParen && peek(4).kind == Tk::Name &&
                         peek(4).text == a.text && peek(5).kind == Tk::RParen &&
                         peek(6).kind == Tk::ColonOp && peek(6).text == ":==" &&
                         peek(7).kind == Tk::Num && peek(7).num == 0.0 &&
                         peek(8).kind == Tk::RParen;
      if (!shape) {
        fail("select() with a selector other than rowmissing(A):==0 on the same A");
        return nullptr;
      }
      p += 9;
      auto n = std::make_unique<Node>(Node::K::Select);
      n->text = a.text;
      return n;
    }
    Node::K k;
    size_t arity;
    if (fn == "J") {
      k = Node::K::J;
      arity = 3;
    } else if (fn == "exp") {
      k = Node::K::Exp;
      arity = 1;
    } else if (fn == "rows") {
      k = Node::K::Rows;
      arity = 1;
    } else if (fn == "cols") {
      k = Node::K::Cols;
      arity = 1;
    } else if (fn == "invsym") {
      k = Node::K::InvSym;
      arity = 1;
    } else {
      fail("the function " + fn + "()");
      return nullptr;
    }
    auto n = std::make_unique<Node>(k);
    if (!args(&n->kids)) return nullptr;
    if (n->kids.size() != arity) {
      fail(fn + "() with " + std::to_string(n->kids.size()) + " argument(s)");
      return nullptr;
    }
    return n;
  }

  NodeP primary() {
    const Token& tok = peek();
    switch (tok.kind) {
      case Tk::Num: {
        ++p;
        auto n = std::make_unique<Node>(Node::K::Num);
        n->num = tok.num;
        return n;
      }
      case Tk::Dot: {
        ++p;
        return std::make_unique<Node>(Node::K::Missing);
      }
      case Tk::Name: {
        const std::string name = tok.text;
        if (peek(1).kind == Tk::LParen) {
          ++p;
          return call(name);
        }
        ++p;
        auto n = std::make_unique<Node>(Node::K::Name);
        n->text = name;
        return n;
      }
      case Tk::LParen: {
        ++p;
        NodeP e = expr();
        if (!e) return nullptr;
        if (!expect(Tk::RParen, describe(peek()))) return nullptr;
        return e;
      }
      default:
        fail(describe(tok));
        return nullptr;
    }
  }
};

// Mata keywords that open a statement form M0 does not have.
bool isMataKeyword(const std::string& w) {
  static const char* kWords[] = {"for",    "while",  "do",       "if",      "else",   "function",
                                 "void",   "real",   "string",   "numeric", "matrix", "scalar",
                                 "vector", "return", "pointer",  "struct",  "class",  "external",
                                 "transmorphic", "colvector", "rowvector", "break", "continue",
                                 "goto",   "version", "end",     "mata"};
  for (const char* k : kWords)
    if (w == k) return true;
  return false;
}

// The two harmless housekeeping statements the AER corpus opens its Mata with.
// Neither needs a Mata runtime: PIET's Mata store is the only state `mata clear`
// has to drop, and PIET has no .mlib libraries for `mata mlib index` to index.
enum class Housekeeping { None, Clear, MlibIndex };

struct Statement {
  std::string text;  // as expanded, for messages
  std::string target;
  NodeP rhs;
  bool stataScalar = false;
  Housekeeping housekeeping = Housekeeping::None;
};

// Parse one statement. nullopt + *construct = outside M0.
std::optional<Statement> parseStatement(const std::string& text, std::string* construct) {
  std::string lexBad;
  auto toks = lex(text, &lexBad);
  if (!toks) {
    *construct = lexBad;
    return std::nullopt;
  }
  const std::vector<Token>& t = *toks;
  // `mata clear` / `mata mlib index` as a statement, ahead of the keyword refusal.
  auto isName = [&](size_t i, const char* w) {
    return i < t.size() && t[i].kind == Tk::Name && t[i].text == w;
  };
  auto endsAt = [&](size_t i) { return i < t.size() && t[i].kind == Tk::End; };
  if (isName(0, "mata") && ((isName(1, "clear") && endsAt(2)) ||
                            (isName(1, "mlib") && isName(2, "index") && endsAt(3)))) {
    Statement st;
    st.text = text;
    st.housekeeping = isName(1, "clear") ? Housekeeping::Clear : Housekeeping::MlibIndex;
    return st;
  }
  if (t.size() >= 7 && t[0].kind == Tk::Name && t[0].text == "st_numscalar" &&
      t[1].kind == Tk::LParen && t[2].kind == Tk::Str && t[3].kind == Tk::Comma) {
    const std::string& name = t[2].text;
    if (!cecon::ident::isValidName(name)) {
      *construct = "an invalid Stata scalar name";
      return std::nullopt;
    }
    Parser ps(t);
    ps.p = 4;
    NodeP rhs = ps.expr();
    if (!rhs || ps.peek().kind != Tk::RParen || ps.p+1 >= t.size() || t[ps.p+1].kind != Tk::End) {
      *construct = "st_numscalar() without one scalar expression";
      return std::nullopt;
    }
    Statement st;
    st.text = text; st.target = name; st.rhs = std::move(rhs); st.stataScalar = true;
    return st;
  }
  if (t.size() >= 2 && t[0].kind == Tk::Name && isMataKeyword(t[0].text)) {
    *construct = "the Mata `" + t[0].text + "` statement";
    return std::nullopt;
  }
  if (!(t.size() >= 3 && t[0].kind == Tk::Name && t[1].kind == Tk::Assign)) {
    if (!t.empty() && t[0].kind == Tk::Name && t.size() >= 2 && t[1].kind == Tk::LParen) {
      *construct = "a call to " + t[0].text + "() as a statement";
    } else if (!t.empty() && t[0].kind == Tk::Name && t.size() >= 2 && t[1].kind == Tk::LBrack) {
      *construct = "a subscripted assignment";
    } else {
      *construct = "a statement that is not `name = expression`";
    }
    return std::nullopt;
  }
  if (cecon::ident::codePointCount(t[0].text) > 32) {
    *construct = "a name longer than 32 characters";
    return std::nullopt;
  }
  Parser ps(t);
  ps.p = 2;
  NodeP rhs = ps.expr();
  if (!rhs) {
    *construct = ps.bad.empty() ? "an expression PIET cannot parse" : ps.bad;
    return std::nullopt;
  }
  if (ps.peek().kind != Tk::End) {
    *construct = Parser::describe(ps.peek());
    return std::nullopt;
  }
  Statement st;
  st.text = text;
  st.target = t[0].text;
  st.rhs = std::move(rhs);
  return st;
}

// ---------------------------------------------------------------------------
// Evaluation.

struct EvalFail {
  bool refused = false;  // true = a runtime shape M0 does not cover (named)
  int rc = 0;
  std::string what;
};

class Evaluator {
 public:
  explicit Evaluator(SessionState& s) : session_(s) {}
  std::optional<EvalFail> fail;

  std::optional<MMat> eval(const Node& n) {
    switch (n.k) {
      case Node::K::Num: return scalarOf(n.num);
      case Node::K::Missing: return scalarOf(kNaN);
      case Node::K::Name: {
        const eval::EvalMatrix* m = findMataMatrix(session_.eval, n.text);
        if (m == nullptr) return runtime(3499, n.text + " not found");
        return fromEval(*m);
      }
      case Node::K::StMatrix: {
        // The Stata store ONLY (D3). A name that is not there is J(0,0,.) in
        // Stata ([M-5] st_matrix(): "returns J(0,0,.) if the matrix does not
        // exist"), not an error.
        for (const auto& [k, m] : session_.eval.matrices) {
          if (k == n.text) return fromEval(m);
        }
        return MMat{};
      }
      case Node::K::Neg: {
        auto x = eval(*n.kids[0]);
        if (!x) return std::nullopt;
        for (double& d : x->v) d = std::isnan(d) ? kNaN : -d;
        return x;
      }
      case Node::K::Bin: return binary(n);
      case Node::K::RowJoin:
      case Node::K::ColJoin: return join(n);
      case Node::K::J: return jfun(n);
      case Node::K::Exp: {
        auto x = eval(*n.kids[0]);
        if (!x) return std::nullopt;
        // Mata's exp() is the same as Stata's: 0 for an argument below -709
        // (StataBE 19.5, `mata: exp(-709.0000000000001)` 0, exp(-709) libm's
        // subnormal; core/stata_arith.h).
        for (double& d : x->v) d = std::isnan(d) ? kNaN : clean(cecon::stata_exp_value(d));
        return x;
      }
      case Node::K::InvSym: {
        auto x = eval(*n.kids[0]);
        if (!x) return std::nullopt;
        if (x->r != x->c) return runtime(3205, "square matrix required");
        return invsym(*x);
      }
      case Node::K::Rows:
      case Node::K::Cols: {
        auto x = eval(*n.kids[0]);
        if (!x) return std::nullopt;
        return scalarOf(static_cast<double>(n.k == Node::K::Rows ? x->r : x->c));
      }
      case Node::K::Select: {
        const eval::EvalMatrix* m = findMataMatrix(session_.eval, n.text);
        if (m == nullptr) return runtime(3499, n.text + " not found");
        const MMat a = fromEval(*m);
        MMat out;
        out.c = a.c;
        for (size_t i = 0; i < a.r; ++i) {
          bool anyMissing = false;
          for (size_t j = 0; j < a.c; ++j) anyMissing = anyMissing || std::isnan(a.at(i, j));
          if (anyMissing) continue;
          for (size_t j = 0; j < a.c; ++j) out.v.push_back(a.at(i, j));
          ++out.r;
        }
        return out;
      }
      case Node::K::Sub: {
        auto x = eval(*n.kids[0]);
        if (!x) return std::nullopt;
        if (n.rowSel > x->r || n.colSel > x->c) return runtime(3301, "subscript invalid");
        MMat out;
        const size_t r0 = n.rowSel ? n.rowSel - 1 : 0, r1 = n.rowSel ? n.rowSel : x->r;
        const size_t c0 = n.colSel ? n.colSel - 1 : 0, c1 = n.colSel ? n.colSel : x->c;
        out.r = r1 - r0;
        out.c = c1 - c0;
        for (size_t i = r0; i < r1; ++i)
          for (size_t j = c0; j < c1; ++j) out.v.push_back(x->at(i, j));
        return out;
      }
    }
    return runtime(3000, "invalid expression");
  }

 private:
  SessionState& session_;

  std::nullopt_t runtime(int rc, const std::string& what) {
    if (!fail) fail = EvalFail{false, rc, what};
    return std::nullopt;
  }
  std::nullopt_t refused(const std::string& what) {
    if (!fail) fail = EvalFail{true, 199, what};
    return std::nullopt;
  }

  // Colon operators are elementwise over c-conformable operands: each
  // dimension must match or be 1 on one side ([M-6] c-conformability).
  std::optional<MMat> binary(const Node& n) {
    auto a = eval(*n.kids[0]);
    if (!a) return std::nullopt;
    auto b = eval(*n.kids[1]);
    if (!b) return std::nullopt;
    auto dim = [](size_t x, size_t y, size_t* out) {
      if (x == y || y == 1) { *out = x; return true; }
      if (x == 1) { *out = y; return true; }
      return false;
    };
    MMat out;
    if (!dim(a->r, b->r, &out.r) || !dim(a->c, b->c, &out.c))
      return runtime(3200, "conformability error");
    out.v.resize(out.r * out.c);
    for (size_t i = 0; i < out.r; ++i) {
      for (size_t j = 0; j < out.c; ++j) {
        const double x = a->at(a->r == 1 ? 0 : i, a->c == 1 ? 0 : j);
        const double y = b->at(b->r == 1 ? 0 : i, b->c == 1 ? 0 : j);
        double z = kNaN;
        if (!std::isnan(x) && !std::isnan(y)) {
          if (n.text == ":+") z = x + y;
          else if (n.text == ":-") z = x - y;
          else if (n.text == ":*") z = x * y;
          else z = (y == 0.0) ? kNaN : x / y;
        }
        out.v[i * out.c + j] = clean(z);
      }
    }
    return out;
  }

  // `,` needs equal row counts, `\` equal column counts ([M-2] op_join's
  // conformability chart, which is exact about void operands too: a 0 x 0
  // does not join onto a 1 x 1).
  std::optional<MMat> join(const Node& n) {
    auto a = eval(*n.kids[0]);
    if (!a) return std::nullopt;
    auto b = eval(*n.kids[1]);
    if (!b) return std::nullopt;
    MMat out;
    if (n.k == Node::K::RowJoin) {
      if (a->c != b->c) return runtime(3200, "conformability error");
      out.r = a->r + b->r;
      out.c = a->c;
      out.v = a->v;
      out.v.insert(out.v.end(), b->v.begin(), b->v.end());
      return out;
    }
    if (a->r != b->r) return runtime(3200, "conformability error");
    out.r = a->r;
    out.c = a->c + b->c;
    for (size_t i = 0; i < out.r; ++i) {
      for (size_t j = 0; j < a->c; ++j) out.v.push_back(a->at(i, j));
      for (size_t j = 0; j < b->c; ++j) out.v.push_back(b->at(i, j));
    }
    return out;
  }

  std::optional<MMat> jfun(const Node& n) {
    auto r = eval(*n.kids[0]);
    if (!r) return std::nullopt;
    auto c = eval(*n.kids[1]);
    if (!c) return std::nullopt;
    auto v = eval(*n.kids[2]);
    if (!v) return std::nullopt;
    if (r->r != 1 || r->c != 1 || c->r != 1 || c->c != 1) return runtime(3200, "conformability error");
    const double rd = r->v[0], cd = c->v[0];
    if (std::isnan(rd) || std::isnan(cd) || rd < 0 || cd < 0 || std::floor(rd) != rd ||
        std::floor(cd) != cd)
      return runtime(3300, "argument out of range");
    if (v->r != 1 || v->c != 1) return refused("J() with a matrix third argument");
    if (rd * cd > 5.0e7) return runtime(3900, "unable to allocate real <tmp>[" +
                                                  std::to_string(static_cast<long long>(rd)) + "," +
                                                  std::to_string(static_cast<long long>(cd)) + "]");
    MMat out;
    out.r = static_cast<size_t>(rd);
    out.c = static_cast<size_t>(cd);
    out.v.assign(out.r * out.c, v->v[0]);
    return out;
  }
};

// Split one line into statements at top-level `;` (Mata's statement
// separator), outside string literals.
std::vector<std::string> splitSemicolons(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool inStr = false;
  for (char ch : s) {
    if (ch == '"') inStr = !inStr;
    if (ch == ';' && !inStr) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(ch);
  }
  out.push_back(cur);
  std::vector<std::string> kept;
  for (auto& x : out) {
    std::string t = strutil::trim(x);
    if (!t.empty()) kept.push_back(t);
  }
  return kept;
}

void assign(eval::EvalSession& ev, const std::string& name, eval::EvalMatrix m) {
  for (auto& [k, v] : ev.mataMatrices) {
    if (k == name) {
      v = std::move(m);
      return;
    }
  }
  ev.mataMatrices.emplace_back(name, std::move(m));
}

// Expand, parse ALL, then run in order. `wholeBlock` only changes the refusal
// wording (a one-liner has no block to skip).
ojson runStatements(const std::vector<std::string>& raw, SessionState& session, bool wholeBlock) {
  std::vector<Statement> parsed;
  parsed.reserve(raw.size());
  for (const auto& line : raw) {
    std::string text = line;
    if (eval::hasMacroRefs(text)) {
      eval::ExpandResult er = eval::expandMacros(text, session.eval);
      if (er.isError) return er.error;
      text = er.line;
    }
    for (const auto& one : splitSemicolons(text)) {
      std::string construct;
      auto st = parseStatement(one, &construct);
      if (!st) return refuseOutside(one, construct, wholeBlock);
      parsed.push_back(std::move(*st));
    }
  }
  bool indexNote = false;
  for (const auto& st : parsed) {
    if (st.housekeeping == Housekeeping::Clear) {
      session.eval.mataMatrices.clear();  // silent, as in Stata
      continue;
    }
    if (st.housekeeping == Housekeeping::MlibIndex) {
      indexNote = true;
      continue;
    }
    Evaluator ev(session);
    auto value = ev.eval(*st.rhs);
    if (!value) {
      const EvalFail f = ev.fail.value_or(EvalFail{false, 3000, "invalid expression"});
      if (f.refused) return refuseOutside(st.text, f.what, false);
      return mataRuntimeError(f.rc, f.what, st.text);
    }
    if (st.stataScalar) {
      if (value->r != 1 || value->c != 1)
        return mataRuntimeError(3200,"conformability error: st_numscalar() requires a scalar",st.text);
      session.eval.scalars.set(st.target,std::isfinite(value->v[0]) ? eval::pythonFloatRepr(value->v[0]) : ".");
      // One namespace with Stata's matrices ([P] matrix define, "Namespace").
      auto& mats = session.eval.matrices;
      mats.erase(std::remove_if(mats.begin(), mats.end(),
                                [&st](const auto& p) { return p.first == st.target; }),
                 mats.end());
    } else assign(session.eval, st.target, toEval(*value));
  }
  if (indexNote) {
    ojson note;
    note["type"] = "echo";
    note["text"] = "(mata mlib index: PIET has no Mata libraries -- nothing to index)";
    return note;
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// Line classification.

// `mata` followed by what? Returns false when the head word is not `mata`.
struct MataHead {
  bool colon = false;
  std::string rest;  // after `mata` / `mata:`, untrimmed
};

std::optional<MataHead> mataHead(const std::string& raw) {
  size_t b = 0;
  while (b < raw.size() && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
  if (raw.compare(b, 4, "mata") != 0) return std::nullopt;
  size_t e = b + 4;
  if (e < raw.size() && isNameChar(raw[e])) return std::nullopt;  // `matalist`, `mata_x`
  MataHead h;
  size_t k = e;
  while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) ++k;
  if (k < raw.size() && raw[k] == ':') {
    h.colon = true;
    h.rest = raw.substr(k + 1);
  } else {
    h.rest = raw.substr(e);
  }
  return h;
}

const std::regex& reProgramHeadForSkip() {
  // Same LOOSE header captureInputBlocks uses (handlers_aer_reshape.cpp): its
  // only job is "do not fold inside here", and over-matching is the safe side.
  static const std::regex re(R"(^program\b(?!\s+(?:drop|dir|list)\b))", std::regex::icase);
  return re;
}

const std::regex& reInputHeadForSkip() {
  static const std::regex re(R"(^input\b)", std::regex::icase);
  return re;
}

const std::regex& reEnd() {
  static const std::regex re(R"(^end\s*$)");
  return re;
}

// A block opener: optional capture/quietly/noisily prefixes (the dispatcher's
// own spellings), then `mata` or `mata:` and nothing else on the line.
const std::regex& reMataOpener() {
  static const std::regex re(
      R"(^((?:(?:capture|captur|captu|capt|cap|quietly|quiet|quie|qui|noisily|noisil|noisi|nois|noi|no|n)\s+)*)mata\s*(:?)\s*$)");
  return re;
}

}  // namespace

size_t colsOf(const eval::EvalMatrix& m) {
  if (!m.data.empty()) return m.data[0].size();
  return m.colnames.size();
}

const eval::EvalMatrix* findMataMatrix(const eval::EvalSession& ev, const std::string& name) {
  for (const auto& [k, m] : ev.mataMatrices) {
    if (k == name) return &m;
  }
  return nullptr;
}

std::vector<std::string> captureMataBlocks(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = lines.size();
  while (i < n) {
    const std::string stripped = strutil::trim(lines[i]);
    // Pass `program ... end` and `input ... end` through verbatim, terminator
    // included: whichever fold reaches a shared `end` first would otherwise
    // swallow the rest of the file.
    // An `input` line that already carries 0x1F is a FOLDED block (if the input
    // pass ever runs first): it is one line, with no `end` of its own to skip to.
    const bool inputHead = std::regex_search(stripped, reInputHeadForSkip()) &&
                           stripped.find('\x1F') == std::string::npos;
    if (std::regex_search(stripped, reProgramHeadForSkip()) || inputHead) {
      out.push_back(lines[i]);
      ++i;
      while (i < n) {
        const std::string body = strutil::trim(lines[i]);
        out.push_back(lines[i]);
        ++i;
        if (std::regex_match(body, reEnd())) break;
      }
      continue;
    }
    std::smatch m;
    if (!std::regex_match(stripped, m, reMataOpener())) {
      out.push_back(lines[i]);
      ++i;
      continue;
    }
    std::string folded = m.str(1) + (m.str(2).empty() ? "mata" : "mata:");
    ++i;
    bool terminated = false;
    while (i < n) {
      const std::string body = strutil::trim(lines[i]);
      ++i;
      if (std::regex_match(body, reEnd())) {
        terminated = true;
        break;
      }
      if (body.empty()) continue;
      folded += kMataStmtSep;
      folded += body;
    }
    if (!terminated) folded += kMataUnterminated;
    out.push_back(folded);
  }
  return out;
}

bool isMataLine(const std::string& raw) { return mataHead(raw).has_value(); }

ojson runMataStatement(const std::string& stmt, SessionState& session) {
  return runStatements({stmt}, session, false);
}

ojson runMataLine(const std::string& raw, SessionState& session) {
  auto head = mataHead(raw);
  if (!head) return ojson(eval::refusedCommand("mata", "not a mata line"));  // not reached

  std::string rest = head->rest;
  const bool folded = rest.find(kMataStmtSep) != std::string::npos ||
                      rest.find(kMataUnterminated) != std::string::npos;
  bool unterminated = false;
  if (!rest.empty() && rest.back() == kMataUnterminated) {
    unterminated = true;
    rest.pop_back();
  }

  if (!head->colon && !folded) {
    // `mata clear` / `mata mata clear` / `mata [mata] mlib index` on one line:
    // the same two housekeeping statements, spelled without the colon.
    static const std::regex re(R"(^\s*(?:mata\s+)?(clear|mlib\s+index)\s*(?://.*)?$)");
    std::smatch m;
    if (std::regex_match(rest, m, re))
      return runStatements({m.str(1).front() == 'c' ? "mata clear" : "mata mlib index"}, session,
                           false);
  }

  if (!head->colon) {
    // `mata` without the colon enters Mata's interactive mode, where an error
    // does NOT abort the block -- a different contract from `mata:`, and one
    // PIET does not implement. Refused by name, block and one-liner alike.
    ojson b = ojson(eval::refusedCommand("mata", "mata_without_colon"));
    std::string msg = b.value("message", std::string());
    msg += " `mata` without a colon (Mata's interactive mode) is not available; PIET runs Mata "
           "statements only as `mata: statement` or a `mata:` ... `end` block.";
    if (folded) msg += " The whole block was skipped and nothing in it was assigned.";
    b["message"] = msg;
    return b;
  }

  if (!folded) {
    const std::string stmt = strutil::trim(rest);
    if (stmt.empty()) {
      ojson b = ojson(eval::refusedCommand("mata", "mata_interactive_session"));
      b["message"] = b.value("message", std::string()) +
                     " A bare `mata:` outside a do-file block opens an interactive Mata session, "
                     "which PIET does not have; write `mata: statement` instead.";
      return b;
    }
    return runStatements({stmt}, session, false);
  }

  if (unterminated) {
    return ojson(eval::errorBlock("syntax_error", "mata: block not terminated by `end`",
                                  std::string("mata")));
  }
  std::vector<std::string> stmts;
  size_t pos = 0;
  while (pos <= rest.size()) {
    size_t next = rest.find(kMataStmtSep, pos);
    if (next == std::string::npos) next = rest.size();
    const std::string one = strutil::trim(rest.substr(pos, next - pos));
    if (!one.empty()) stmts.push_back(one);
    pos = next + 1;
  }
  return runStatements(stmts, session, true);
}

std::string renderFoldedMataForEcho(const std::string& raw) {
  if (raw.find(kMataStmtSep) == std::string::npos &&
      raw.find(kMataUnterminated) == std::string::npos)
    return raw;
  std::string out;
  bool terminated = true;
  for (char ch : raw) {
    if (ch == kMataStmtSep) {
      out += "\n: ";
    } else if (ch == kMataUnterminated) {
      terminated = false;
    } else {
      out.push_back(ch);
    }
  }
  if (terminated) out += "\n: end";
  return out;
}

}  // namespace piet::mata
