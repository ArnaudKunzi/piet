// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Restricted Python-expression evaluator — see pyeval.h. The grammar and
// semantics mirror CPython for the whitelisted-character subset that
// macros._eval_arith / postfile.eval_macro_expr can ever feed to eval().
#include "piet/eval/pyeval.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel: arithmetic on missing is `.`

#include "data/stata_dates.h"  // B3(b): the literal pseudo-function table + parser
#include "core/stata_arith.h"  // `^`/`**` with StataBE's bits
#include "core/ident.h"  // Unicode names: the shared identifier byte tests

#include "../dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace piet::eval {

namespace {

const double kNaN = std::nan("");

// ---------------------------------------------------------------------------
// Lexer

enum class Tok {
  End, Int, Float, Str, Name,
  Plus, Minus, Star, Slash, DblSlash, DblStar,
  LParen, RParen, Comma,
  Lt, Gt, Le, Ge, Eq, Ne,
  And, Or, Not,
  // BUG-660 fix round: Stata's own logical operators (`&`, `|`, bare `!`),
  // distinct from the Python-keyword And/Or/Not above — see the Node::T::Bool
  // eval() arm for why they cannot just reuse Tok::And/Tok::Or's semantics.
  // Bang (bare `!`) is ALSO distinct from Not (Python's `not` keyword): Stata's
  // `!` binds at unary-minus precedence (U 13.2.7: `^` binds tighter than `!`,
  // which binds tighter than `*`, `/`, `+`, `-`, comparisons, `&`, `|`; verified
  // against StataBE -- `!0^0` is 0 (`!(0^0)`), `!1 + 1` is 1 (`(!1)+1`)), while
  // Python's `not` binds looser than everything except `and`/`or`. Sharing one
  // token made parseNot() bind bare `!` at Python's loose precedence, giving
  // silent wrong answers on every evalArith surface (local/global/scalar/
  // display); see parseFactor() for where Bang is actually parsed.
  Amp, Pipe, Bang,
};

struct Token {
  Tok kind;
  std::string text;    // Name / Str content (Str: decoded)
  long long intVal = 0;
  double floatVal = 0.0;
};

struct Lexer {
  const std::string& src;
  size_t i = 0;
  explicit Lexer(const std::string& s) : src(s) {}

  [[noreturn]] void fail(const std::string& why) { throw PyExc("SyntaxError: " + why); }

  void skipWs() {
    while (i < src.size()) {
      char c = src[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        ++i;
      } else {
        break;
      }
    }
  }

  Token lexNumber() {
    // Python decimal literals: digits (with _ between), optional fraction,
    // optional exponent. Leading zeros in a nonzero int literal are a
    // SyntaxError ("05"). A '.' with digits on either side is a float.
    size_t start = i;
    bool isFloat = false;
    std::string digits;

    auto readDigits = [&]() {
      bool any = false;
      bool prevDigit = false;
      while (i < src.size()) {
        char c = src[i];
        if (c >= '0' && c <= '9') {
          digits.push_back(c);
          any = true;
          prevDigit = true;
          ++i;
        } else if (c == '_') {
          if (!prevDigit || i + 1 >= src.size() || !(src[i + 1] >= '0' && src[i + 1] <= '9')) {
            fail("bad underscore in number");
          }
          prevDigit = false;
          ++i;
        } else {
          break;
        }
      }
      return any;
    };

    std::string intPart;
    if (src[i] != '.') {
      readDigits();
      intPart = digits;
    }
    if (i < src.size() && src[i] == '.') {
      // Not a float if this '.' starts an attribute access — Python would
      // SyntaxError anyway for "1 .x"; treat '.' always as fraction here.
      isFloat = true;
      digits.push_back('.');
      ++i;
      readDigits();
    }
    if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
      // Exponent only if followed by digits (optionally signed) — otherwise
      // 'e' is a NAME token boundary error in Python ("2e" SyntaxError, but
      // "2 e" is INT then NAME). "2e3" float.
      size_t save = i;
      ++i;
      std::string expDigits = "e";
      if (i < src.size() && (src[i] == '+' || src[i] == '-')) {
        expDigits.push_back(src[i]);
        ++i;
      }
      if (i < src.size() && src[i] >= '0' && src[i] <= '9') {
        isFloat = true;
        digits += expDigits;
        readDigits();
      } else {
        // Python: "2e" / "2e+" -> SyntaxError (the tokenizer commits to an
        // exponent once it sees [eE] directly after a number).
        (void)save;
        fail("invalid decimal literal");
      }
    }
    if (digits.empty() || digits == ".") fail("invalid number");

    Token t;
    if (isFloat) {
      t.kind = Tok::Float;
      t.floatVal = std::strtod(digits.c_str(), nullptr);
    } else {
      // Leading-zero check (Python 3): "05" SyntaxError; "0", "00" fine.
      if (intPart.size() > 1 && intPart[0] == '0') {
        bool allZero = true;
        for (char c : intPart) {
          if (c != '0') {
            allZero = false;
            break;
          }
        }
        if (!allZero) fail("leading zeros in decimal integer literal");
      }
      t.kind = Tok::Int;
      errno = 0;
      t.intVal = std::strtoll(digits.c_str(), nullptr, 10);
      if (errno == ERANGE) {
        // Python would produce an exact bignum; documented divergence —
        // treat as float to stay closest in magnitude.
        t.kind = Tok::Float;
        t.floatVal = std::strtod(digits.c_str(), nullptr);
      }
    }
    (void)start;
    return t;
  }

  Token lexString(char quote) {
    ++i;  // skip opening quote
    std::string out;
    // BUG-502: STATA HAS NO ESCAPE SEQUENCES inside a double-quoted literal. A
    // backslash is an ordinary character, so `"a\nb"` is FOUR characters,
    // `"\begin"` is six and `"a\\b"` is four -- and `"say \"hi\""` ends at the
    // second quote, which is why Stata's own answer for its length is 5. Measured
    // in tests/eval/golden/aer_w1/w1b_braceliteral.log's neighbourhood; a `"`
    // inside a string needs Stata's compound quotes, not a backslash.
    //
    // This lexer was written to PYTHON semantics (its own comment said "Python
    // keeps unknown escapes literally"), which is right for a Python evaluator and
    // wrong for every caller it has: macros.cpp, postfile.cpp, loops.cpp,
    // strmutate.cpp and evalcase_ext.cpp all evaluate STATA expressions. The cost
    // was silent and everywhere a path or a LaTeX macro appears -- `"c:\data\new"`
    // lost two characters and `"\begin{tabular}"` one.
    while (i < src.size() && src[i] != quote) {
      out.push_back(src[i]);
      ++i;
    }
    if (i >= src.size()) fail("unterminated string literal");
    ++i;  // closing quote
    Token t;
    t.kind = Tok::Str;
    t.text = out;
    return t;
  }

  Token next() {
    skipWs();
    if (i >= src.size()) return {Tok::End, "", 0, 0.0};
    char c = src[i];

    if ((c >= '0' && c <= '9') ||
        (c == '.' && i + 1 < src.size() && src[i + 1] >= '0' && src[i + 1] <= '9')) {
      return lexNumber();
    }
    if (c == '"' || c == '\'') return lexString(c);
    // EM3b: Stata's compound-quoted literal `"..."' -- the delimiter that can carry
    // a '"' -- nests (a `" opens a level, a "' closes one) and keeps its body
    // verbatim, inner quotes and nested spans included (w2af_macro_text_file.log
    // :1077 mt_fn_strlen `<7>` for `a "b" c`; :1197 mt_fn_stritrim).
    if (c == '`' && i + 1 < src.size() && src[i + 1] == '"') {
      int depth = 0;
      size_t j = i;
      while (j < src.size()) {
        if (j + 1 < src.size() && src[j] == '`' && src[j + 1] == '"') {
          ++depth;
          j += 2;
        } else if (j + 1 < src.size() && src[j] == '"' && src[j + 1] == '\'') {
          j += 2;
          if (--depth == 0) break;
        } else {
          ++j;
        }
      }
      if (depth != 0) fail("unterminated string literal");
      Token t;
      t.kind = Tok::Str;
      t.text = src.substr(i + 2, j - i - 4);
      i = j;
      return t;
    }
    // Unicode names (lane M): the shared rule's permissive byte tests
    // (src/core/ident.h). A byte >= 0x80 is an identifier byte, so `قيمة`,
    // `ملاحظات` and the mixed-script `دخل_2020` lex as ONE Name -- before, the
    // first Arabic byte fell to the `default:` "unexpected character" arm and
    // evalArith, evalBoolExpr and the string row evaluator all refused the
    // expression. A name is looked up byte-exactly, so no decoding is needed.
    if (cecon::ident::isIdentStartByte(c)) {
      size_t start = i;
      while (i < src.size() && cecon::ident::isIdentByte(src[i])) ++i;
      std::string name = src.substr(start, i - start);
      if (name == "and") return {Tok::And, name, 0, 0.0};
      if (name == "or") return {Tok::Or, name, 0, 0.0};
      if (name == "not") return {Tok::Not, name, 0, 0.0};
      // B3(b): the LITERAL pseudo-functions. td(15mar2020), tq(2005q1),
      // tc(12:00) and the single-letter synonyms take unquoted date TEXT, not an
      // expression -- Stata answers r(198) for a variable argument. The lift has
      // to happen in the lexer because by the time the parser runs, `15mar2020`
      // is already three tokens. Doing it here rather than as a pre-substitution
      // on the source is deliberate: the lexer has already stepped over string
      // literals, so this cannot reach inside one (the defect class of BUG-443).
      // Same table and same parser as cobra's copy, so the two evaluators cannot
      // disagree about what tq(2005q1) means.
      {
        cecon::LiteralScale ls;
        if (cecon::literal_scale_for(name, ls)) {
          size_t j = i;
          while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
          if (j < src.size() && src[j] == '(') {
            size_t q = j + 1;
            int depth = 1;
            while (q < src.size() && depth) {
              if (src[q] == '(') ++depth;
              else if (src[q] == ')') --depth;
              if (depth) ++q;
            }
            if (depth) fail("expected ')' after " + name + "(");
            const std::string raw = src.substr(j + 1, q - j - 1);
            const double lit = cecon::stata_datetime_literal(raw, ls);
            // Stata refuses an impossible literal with r(198) (tq(2010q5),
            // tm(2010m13), td(31feb2010): w2y_fn_datetime fd_tq_bad/fd_tm_bad/
            // fd_td_bad). The `r(198):` prefix is the channel evalArith forwards as
            // the exact code; a plain SyntaxError left `scalar s = tq(2010q5)` at
            // rc 0 holding the expression text.
            if (std::isnan(lit)) throw PyExc("r(198): " + name + "(" + raw + ") invalid");
            i = q + 1;
            Token lt;
            lt.kind = Tok::Float;
            lt.floatVal = lit;
            return lt;
          }
        }
      }
      Token t;
      t.kind = Tok::Name;
      t.text = name;
      return t;
    }
    switch (c) {
      case '+': ++i; return {Tok::Plus, "+", 0, 0.0};
      case '-': ++i; return {Tok::Minus, "-", 0, 0.0};
      case '*':
        if (i + 1 < src.size() && src[i + 1] == '*') {
          i += 2;
          return {Tok::DblStar, "**", 0, 0.0};
        }
        ++i;
        return {Tok::Star, "*", 0, 0.0};
      case '/':
        if (i + 1 < src.size() && src[i + 1] == '/') {
          i += 2;
          return {Tok::DblSlash, "//", 0, 0.0};
        }
        ++i;
        return {Tok::Slash, "/", 0, 0.0};
      case '(': ++i; return {Tok::LParen, "(", 0, 0.0};
      case ')': ++i; return {Tok::RParen, ")", 0, 0.0};
      case ',': ++i; return {Tok::Comma, ",", 0, 0.0};
      case '<':
        if (i + 1 < src.size() && src[i + 1] == '=') {
          i += 2;
          return {Tok::Le, "<=", 0, 0.0};
        }
        if (i + 1 < src.size() && src[i + 1] == '>') fail("'<>' is not valid Python 3");
        ++i;
        return {Tok::Lt, "<", 0, 0.0};
      case '>':
        if (i + 1 < src.size() && src[i + 1] == '=') {
          i += 2;
          return {Tok::Ge, ">=", 0, 0.0};
        }
        ++i;
        return {Tok::Gt, ">", 0, 0.0};
      case '=':
        if (i + 1 < src.size() && src[i + 1] == '=') {
          i += 2;
          return {Tok::Eq, "==", 0, 0.0};
        }
        fail("single '=' is not an expression operator");
      case '!':
        if (i + 1 < src.size() && src[i + 1] == '=') {
          i += 2;
          return {Tok::Ne, "!=", 0, 0.0};
        }
        // BUG-660 fix round: a bare `!` is Stata's logical NOT (`display
        // !.`, `local m = !.`) — was an unconditional fail() before, so
        // every macro-layer arm using it never even reached the evaluator.
        // Fix round 2: its own token (Tok::Bang), not Tok::Not (Python's
        // `not` keyword) -- see the Tok enum comment for the precedence bug
        // that sharing one token caused.
        ++i;
        return {Tok::Bang, "!", 0, 0.0};
      case '~':
        // Stata spells `!` and `!=` also as `~` and `~=` (operator.sthlp;
        // w2ac_expr_relational gen_tilde_pow0 / gen_tilde_ne_gt).
        if (i + 1 < src.size() && src[i + 1] == '=') {
          i += 2;
          return {Tok::Ne, "~=", 0, 0.0};
        }
        ++i;
        return {Tok::Bang, "~", 0, 0.0};
      case '&':
        // BUG-660 fix round: Stata's `&` (logical AND). Distinct token from
        // the "and" keyword's Tok::And — see parseStataAnd/Node::T::Bool.
        ++i;
        return {Tok::Amp, "&", 0, 0.0};
      case '|':
        // BUG-660 fix round: Stata's `|` (logical OR). Distinct token from
        // the "or" keyword's Tok::Or — see parseStataOr/Node::T::Bool.
        ++i;
        return {Tok::Pipe, "|", 0, 0.0};
      default:
        fail(std::string("unexpected character '") + c + "'");
    }
  }
};

// ---------------------------------------------------------------------------
// AST

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
  enum class T { Int, Float, Str, Name, Unary, Bin, Bool, Not, Compare, Call } t;
  // literals
  long long intVal = 0;
  double floatVal = 0.0;
  std::string text;  // Str content / Name
  // operators
  Tok op = Tok::End;
  NodePtr a, b;
  std::vector<NodePtr> parts;       // Bool operands / Call args
  std::vector<Tok> cmpOps;          // Compare operators
  std::vector<NodePtr> cmpOperands; // Compare right operands (a = first)

  // Prebound resolution (compilePreboundExpr) — meaningful only for T::Name.
  // Left Unresolved for the ordinary evalPyExpr path, which resolves names
  // against a live PyNamespace instead of these cached bindings.
  enum class Bind { Unresolved, Slot, Const, Fn };
  Bind bind = Bind::Unresolved;
  int slot = -1;     // Bind::Slot  -> index into the per-row slot vector
  PyValue constVal;  // Bind::Const -> constant captured at compile time
  PyFn fn;           // Bind::Fn    -> function copied from constNs.fns
};

struct Parser {
  Lexer lex;
  Token cur;
  explicit Parser(const std::string& s) : lex(s) { cur = lex.next(); }

  [[noreturn]] void fail(const std::string& why) { throw PyExc("SyntaxError: " + why); }

  void advance() { cur = lex.next(); }
  bool accept(Tok k) {
    if (cur.kind == k) {
      advance();
      return true;
    }
    return false;
  }
  void expect(Tok k, const char* what) {
    if (cur.kind != k) fail(std::string("expected ") + what);
    advance();
  }

  NodePtr parseExpr() { return parseStataOr(); }

  // BUG-660 fix round: Stata's `|`/`&` sit OUTSIDE the Python and/or/not
  // chain below, same relative precedence (`&` binds tighter than `|`, like
  // "and" binds tighter than "or") but a DIFFERENT node op (Tok::Pipe/
  // Tok::Amp, not Tok::Or/Tok::And) so eval() coerces every result to a
  // genuine 0/1 instead of returning the deciding operand the way Python's
  // `or`/`and` do — `. | 0` must render `1`, not the missing sentinel a
  // Python-style `or` would hand back untouched. Real Stata text never
  // contains the words "and"/"or"/"not" as operators (those are just names
  // there), so nesting these two levels outside parseOr cannot change any
  // existing expression's parse.
  NodePtr parseStataOr() {
    NodePtr n = parseStataAnd();
    if (cur.kind != Tok::Pipe) return n;
    auto out = std::make_unique<Node>();
    out->t = Node::T::Bool;
    out->op = Tok::Pipe;
    out->parts.push_back(std::move(n));
    while (accept(Tok::Pipe)) out->parts.push_back(parseStataAnd());
    return out;
  }

  NodePtr parseStataAnd() {
    NodePtr n = parseOr();
    if (cur.kind != Tok::Amp) return n;
    auto out = std::make_unique<Node>();
    out->t = Node::T::Bool;
    out->op = Tok::Amp;
    out->parts.push_back(std::move(n));
    while (accept(Tok::Amp)) out->parts.push_back(parseOr());
    return out;
  }

  NodePtr parseOr() {
    NodePtr n = parseAnd();
    if (cur.kind != Tok::Or) return n;
    auto out = std::make_unique<Node>();
    out->t = Node::T::Bool;
    out->op = Tok::Or;
    out->parts.push_back(std::move(n));
    while (accept(Tok::Or)) out->parts.push_back(parseAnd());
    return out;
  }

  NodePtr parseAnd() {
    NodePtr n = parseNot();
    if (cur.kind != Tok::And) return n;
    auto out = std::make_unique<Node>();
    out->t = Node::T::Bool;
    out->op = Tok::And;
    out->parts.push_back(std::move(n));
    while (accept(Tok::And)) out->parts.push_back(parseNot());
    return out;
  }

  NodePtr parseNot() {
    if (accept(Tok::Not)) {
      auto out = std::make_unique<Node>();
      out->t = Node::T::Not;
      out->a = parseNot();
      return out;
    }
    return parseComparison();
  }

  // FU-844: Stata, not Python, comparisons. Each operator is its own
  // left-associative level, tightest first != > < <= >= == (operator.sthlp;
  // oracle w2ac_expr_relational disp_/local_/scalar_/ifcmd_<o1>_<o2>), so
  // `0==0<0` is 0==(0<0) = 1 where Python's chain `0==0 and 0<0` is 0. Every
  // Compare node holds exactly one operator.
  static constexpr Tok kCmpLevel[6] = {Tok::Eq, Tok::Ge, Tok::Le, Tok::Lt, Tok::Gt, Tok::Ne};
  NodePtr parseComparison(int lvl = 0) {
    if (lvl == 6) return parseArith();
    NodePtr n = parseComparison(lvl + 1);
    while (cur.kind == kCmpLevel[lvl]) {
      auto out = std::make_unique<Node>();
      out->t = Node::T::Compare;
      out->a = std::move(n);
      out->cmpOps.push_back(cur.kind);
      advance();
      out->cmpOperands.push_back(parseComparison(lvl + 1));
      n = std::move(out);
    }
    return n;
  }

  // EM0: [U] 13.2.5 orders the binary operators ^, -(negation), /, *, -(subtraction), +,
  // each level left-associative: a+b-c is a+(b-c) and a*b/c is a*(b/c) (oracle
  // w2am_expr_binary_order eo_disp_pm: 0.1+0.2-0.3 is 2^-55; eo_disp_md). `//` stays
  // at the `/` level.
  NodePtr binLevel(NodePtr (Parser::*next)(), Tok t1, Tok t2) {
    NodePtr n = (this->*next)();
    while (cur.kind == t1 || cur.kind == t2) {
      Tok op = cur.kind;
      advance();
      auto out = std::make_unique<Node>();
      out->t = Node::T::Bin;
      out->op = op;
      out->a = std::move(n);
      out->b = (this->*next)();
      n = std::move(out);
    }
    return n;
  }
  NodePtr parseArith() { return binLevel(&Parser::parseSub, Tok::Plus, Tok::Plus); }
  NodePtr parseSub() { return binLevel(&Parser::parseTerm, Tok::Minus, Tok::Minus); }
  NodePtr parseTerm() { return binLevel(&Parser::parseDiv, Tok::Star, Tok::Star); }
  NodePtr parseDiv() { return binLevel(&Parser::parseFactor, Tok::Slash, Tok::DblSlash); }

  NodePtr parseFactor() {
    if (cur.kind == Tok::Plus || cur.kind == Tok::Minus) {
      Tok op = cur.kind;
      advance();
      auto out = std::make_unique<Node>();
      out->t = Node::T::Unary;
      out->op = op;
      out->a = parseFactor();
      return out;
    }
    // BUG-660 fix round 2: Stata's bare `!` is a unary operator at this same
    // precedence tier as unary +/- (looser than `^`, tighter than `*`/`/`/
    // `+`/`-`/comparisons/`&`/`|` -- see the Tok::Bang enum comment for the
    // StataBE measurements this rests on), NOT Python's low-precedence `not`
    // keyword (that one stays in parseNot()).
    if (cur.kind == Tok::Bang) return parseBang(&Parser::parseFactor);
    return parsePower();
  }

  // Stata's `!` (or `~`) over the operand `next` parses. A sign after it,
  // spaces or not, is invalid syntax in StataBE (w2ac_expr_relational
  // disp_not_gt `!-1>0`, gen_not_neg `!-t` r(198); w2ac_expr_valueref
  // disp/local/scalar_not_neg(_sp) `! -1`, _not_plus `!+1`, _tilde_neg). The
  // r(198) prefix is the exact-code channel, so `scalar s = !-1` is r(198) with
  // s unset (scalar_not_neg) instead of storing the expression text.
  NodePtr parseBang(NodePtr (Parser::*next)()) {
    advance();
    if (cur.kind == Tok::Minus || cur.kind == Tok::Plus) throw PyExc("r(198): invalid syntax");
    auto out = std::make_unique<Node>();
    out->t = Node::T::Not;
    out->op = Tok::Bang;  // Stata's `!`: a string operand is r(109) (eval())
    out->a = (this->*next)();
    return out;
  }

  // FU-843: Stata's `^` (here `**`) is LEFT-associative and its right operand
  // is the next atom with any leading signs / `!` (oracle w2ac_expr_precedence
  // disp_chain_const `2^3^2` = 64, disp_pow_neg_pow `2^-3^2` = (2^-3)^2), not
  // Python's right-associative `2**3**2` = 512 with a whole-factor rhs.
  NodePtr parsePower() {
    NodePtr n = parseAtomTrailer();
    while (accept(Tok::DblStar)) {
      auto out = std::make_unique<Node>();
      out->t = Node::T::Bin;
      out->op = Tok::DblStar;
      out->a = std::move(n);
      out->b = parsePowRhs();
      n = std::move(out);
    }
    return n;
  }

  NodePtr parsePowRhs() {
    if (cur.kind == Tok::Plus || cur.kind == Tok::Minus) {
      Tok op = cur.kind;
      advance();
      auto out = std::make_unique<Node>();
      out->t = Node::T::Unary;
      out->op = op;
      out->a = parsePowRhs();
      return out;
    }
    if (cur.kind == Tok::Bang) return parseBang(&Parser::parsePowRhs);
    return parseAtomTrailer();
  }

  NodePtr parseAtomTrailer() {
    NodePtr n = parseAtom();
    while (cur.kind == Tok::LParen) {
      advance();
      auto call = std::make_unique<Node>();
      call->t = Node::T::Call;
      call->a = std::move(n);
      if (cur.kind != Tok::RParen) {
        call->parts.push_back(parseExpr());
        while (accept(Tok::Comma)) {
          if (cur.kind == Tok::RParen) break;  // trailing comma allowed in calls
          call->parts.push_back(parseExpr());
        }
      }
      expect(Tok::RParen, "')'");
      n = std::move(call);
    }
    return n;
  }

  NodePtr parseAtom() {
    auto out = std::make_unique<Node>();
    switch (cur.kind) {
      case Tok::Int:
        out->t = Node::T::Int;
        out->intVal = cur.intVal;
        advance();
        return out;
      case Tok::Float:
        out->t = Node::T::Float;
        out->floatVal = cur.floatVal;
        advance();
        return out;
      case Tok::Str: {
        out->t = Node::T::Str;
        out->text = cur.text;
        advance();
        // Python: adjacent string literals concatenate ("a" "b" == "ab").
        while (cur.kind == Tok::Str) {
          out->text += cur.text;
          advance();
        }
        return out;
      }
      case Tok::Name:
        out->t = Node::T::Name;
        out->text = cur.text;
        advance();
        return out;
      case Tok::LParen: {
        advance();
        NodePtr inner = parseExpr();
        if (cur.kind == Tok::Comma) fail("tuple expressions are not supported");
        expect(Tok::RParen, "')'");
        return inner;
      }
      default:
        fail("unexpected token");
    }
  }
};

// ---------------------------------------------------------------------------
// Evaluation values: PyValue or a function object (bare fn name / callee).

struct EvalVal {
  bool isFn = false;
  std::string fnName;
  const PyFn* fn = nullptr;
  PyValue v;

  static EvalVal of(PyValue val) {
    EvalVal e;
    e.v = std::move(val);
    return e;
  }
  static EvalVal ofFn(const std::string& name, const PyFn* f) {
    EvalVal e;
    e.isFn = true;
    e.fnName = name;
    e.fn = f;
    return e;
  }
  bool truthy() const { return isFn ? true : v.truthy(); }
};

// --- numeric helpers ---------------------------------------------------------

// int arithmetic with overflow fallback to double (documented divergence
// vs Python bignums; corpus keeps magnitudes < 2^53).
PyValue intAdd(long long x, long long y) {
  long long r;
  if (__builtin_add_overflow(x, y, &r)) {
    return PyValue::floating(static_cast<double>(x) + static_cast<double>(y));
  }
  return PyValue::integer(r);
}
PyValue intSub(long long x, long long y) {
  long long r;
  if (__builtin_sub_overflow(x, y, &r)) {
    return PyValue::floating(static_cast<double>(x) - static_cast<double>(y));
  }
  return PyValue::integer(r);
}
PyValue intMul(long long x, long long y) {
  long long r;
  if (__builtin_mul_overflow(x, y, &r)) {
    return PyValue::floating(static_cast<double>(x) * static_cast<double>(y));
  }
  return PyValue::integer(r);
}

long long asIntOperand(const PyValue& v) { return v.isFloat() ? 0 : v.intVal(); }

// Is this operand one of the expression layer's missing values (`.` is 2^1023,
// `.a`..`.z` sit just above it, the negative band is `.` too; exprfns.h)?
// Sign-blind, and only finite values count, so the inf/nan that postfile's
// Python namespace binds keep their IEEE arithmetic. c(maxdouble), one ulp
// under 2^1023, is a real number (missing_range_oracle.log sentinel-negated).
bool isStataMissingOperand(const PyValue& v) {
  if (!v.isFloat()) return false;
  const double d = v.floatVal();
  return std::isfinite(d) && std::fabs(d) >= kStataMissingSentinel;
}

PyValue evalBinNumericRaw(Tok op, const PyValue& a, const PyValue& b, bool floats);

// name-binding-display-literals: see StataDivisionScope (pyeval.h).
thread_local bool tl_stataDivision = false;

// On the Stata expression surfaces (StataDivisionScope) an operand that is not a
// finite number -- the +/-inf an overflowing + - * leaves -- reads as `.` to the
// operator that consumes it. A function argument is not read this way: Stata
// hands the infinity itself to the function (StataBE 19.5: `(1e300*1e10 == .)`
// is 1 and `(-1e300*1e10 < 0)` is 0, while `inlist(1e300*1e10, .)` is 0 and
// `missing(-1e300*1e10)` is 0).
bool isStataOverflowOperand(const PyValue& v) {
  return tl_stataDivision && v.isFloat() && !std::isfinite(v.floatVal());
}
EvalVal stataOperand(EvalVal v) {
  if (!v.isFn && isStataOverflowOperand(v.v)) return EvalVal::of(PyValue::floating(kStataMissingSentinel));
  return v;
}

PyValue evalBinNumeric(Tok op, const PyValue& a, const PyValue& b) {
  bool floats = a.isFloat() || b.isFloat();
  // Arithmetic on any missing is plain `.` in Stata: the kind is lost
  // ((.a + 1) == . is 1, src/data/missing.h), and `./2`, `.*0`, `.^2` are `.`,
  // not 2^1022, 0 or an overflow. Comparisons and direct reads keep the kind;
  // they never reach this function.
  if (floats && (isStataMissingOperand(a) || isStataMissingOperand(b) ||
                 isStataOverflowOperand(a) || isStataOverflowOperand(b))) {
    return PyValue::floating(kStataMissingSentinel);
  }
  // A finite result that lands in the band is plain `.` as well, never a kind
  // (`c(maxdouble)*1.5` would otherwise decode as `.a`..`.z`). Non-finite
  // results keep IEEE semantics: an overflow to +/-inf stays one, as in Stata,
  // until an operator reads it (isStataOverflowOperand). On the Stata expression
  // surfaces a NaN is never such a result -- it is `.` (BUG-1363).
  PyValue r = evalBinNumericRaw(op, a, b, floats);
  if (tl_stataDivision && r.isFloat() && std::isnan(r.floatVal())) {
    return PyValue::floating(kStataMissingSentinel);
  }
  if (r.isFloat() && std::isfinite(r.floatVal()) &&
      std::fabs(r.floatVal()) >= kStataMissingSentinel) {
    return PyValue::floating(kStataMissingSentinel);
  }
  return r;
}

PyValue evalBinNumericRaw(Tok op, const PyValue& a, const PyValue& b, bool floats) {
  switch (op) {
    case Tok::Plus:
      if (!floats) return intAdd(asIntOperand(a), asIntOperand(b));
      return PyValue::floating(a.numAsDouble() + b.numAsDouble());
    case Tok::Minus:
      if (!floats) return intSub(asIntOperand(a), asIntOperand(b));
      return PyValue::floating(a.numAsDouble() - b.numAsDouble());
    case Tok::Star:
      if (!floats) return intMul(asIntOperand(a), asIntOperand(b));
      return PyValue::floating(a.numAsDouble() * b.numAsDouble());
    case Tok::Slash: {
      double db = b.numAsDouble();
      // name-binding-display-literals: under a StataDivisionScope (the Stata
      // expression surfaces) x/0 is missing, as in Stata, never an error.
      if (db == 0.0 && tl_stataDivision) return PyValue::floating(kStataMissingSentinel);
      if (db == 0.0) throw PyExc("ZeroDivisionError");
      return PyValue::floating(a.numAsDouble() / db);
    }
    case Tok::DblSlash: {
      if (!floats) {
        long long y = asIntOperand(b);
        if (y == 0) throw PyExc("ZeroDivisionError");
        long long x = asIntOperand(a);
        long long q = x / y;
        if ((x % y != 0) && ((x < 0) != (y < 0))) --q;  // Python floors
        return PyValue::integer(q);
      }
      double db = b.numAsDouble();
      if (db == 0.0) throw PyExc("ZeroDivisionError");
      return PyValue::floating(std::floor(a.numAsDouble() / db));
    }
    case Tok::DblStar: {
      if (!floats) {
        long long base = asIntOperand(a);
        long long exp = asIntOperand(b);
        if (exp >= 0) {
          // int ** nonneg int -> int (exact until 64-bit overflow, then
          // double fallback — documented divergence).
          long long result = 1;
          bool overflow = false;
          long long bb = base;
          long long e = exp;
          while (e > 0 && !overflow) {
            if (e & 1) {
              if (__builtin_mul_overflow(result, bb, &result)) overflow = true;
            }
            e >>= 1;
            if (e > 0) {
              if (__builtin_mul_overflow(bb, bb, &bb)) overflow = true;
            }
          }
          // Exact while |result| <= 2^53, where Stata's left-to-right double
          // product is exact too; beyond that the double Stata computes
          // (core/stata_arith.h), which is not the rounded exact integer.
          if (!overflow && result <= (1LL << 53) && result >= -(1LL << 53)) return PyValue::integer(result);
          return PyValue::floating(cecon::stata_pow_value(static_cast<double>(base), static_cast<double>(exp)));
        }
        // int ** negative int -> float; 0 ** -n -> ZeroDivisionError (Stata: `.`,
        // BUG-1065).
        if (base == 0) {
          if (tl_stataDivision) return PyValue::floating(kStataMissingSentinel);
          throw PyExc("ZeroDivisionError: 0 to a negative power");
        }
        return PyValue::floating(cecon::stata_pow_value(static_cast<double>(base), static_cast<double>(exp)));
      }
      double da = a.numAsDouble();
      double db = b.numAsDouble();
      // BUG-1065: Stata's `^` has no error. 0^-n, a negative base with a
      // fractional power and an overflowing power are all `.` (core/
      // stata_arith.h), so on the Stata expression surfaces (StataDivisionScope)
      // each of the three Python raises below is the missing sentinel instead.
      if (da == 0.0 && db < 0.0) {
        if (tl_stataDivision) return PyValue::floating(kStataMissingSentinel);
        throw PyExc("ZeroDivisionError: 0.0 to a negative power");
      }
      if (da < 0.0 && db != std::trunc(db) && std::isfinite(db)) {
        // Python returns a complex number here; every M1-scope consumer
        // then fails isinstance(int/float) EXCEPT postfile.eval_macro_expr
        // (documented divergence). Raise -> whole-expression failure.
        if (tl_stataDivision) return PyValue::floating(kStataMissingSentinel);
        throw PyExc("complex result from negative base ** fractional exponent");
      }
      // Stata's `^`: x^2 is x*x, x^-2 is (1/x)*(1/x), a non-integer power is
      // exp(y*ln(x)) (core/stata_arith.h); libm pow is off in the last bit.
      double r = cecon::stata_pow_value(da, db);
      // CPython 3.13: only ** raises OverflowError on float overflow
      // (verified against the venv; + - * / silently produce inf).
      // (CPython decides by libm pow: the clamp of Stata's own overflow edge in
      // stata_pow_value makes r a NaN where pow() overflowed.)
      if ((std::isinf(r) || (std::isnan(r) && std::isinf(std::pow(da, db)))) && std::isfinite(da) && std::isfinite(db)) {
        // Stata's `^` makes a positive overflow `.` but leaves a negative one
        // -inf: inlist(1e200^2, .) is 1, inlist((-1e200)^3, .) is 0.
        if (tl_stataDivision) return PyValue::floating(r < 0 ? r : kStataMissingSentinel);
        throw PyExc("OverflowError: (34, 'Result too large')");
      }
      return PyValue::floating(r);
    }
    default:
      throw PyExc("bad numeric op");
  }
}

// Python `==` semantics, except str-vs-num which is Stata's r(109) (BUG-456).
bool pyEquals(const EvalVal& a, const EvalVal& b) {
  if (a.isFn || b.isFn) {
    return a.isFn && b.isFn && a.fnName == b.fnName;  // identity: same ns entry
  }
  const PyValue& x = a.v;
  const PyValue& y = b.v;
  // BUG-456 (2026-09-08): Stata has no answer for a string compared with a
  // number -- `e(cmd)==1`, `e(nosuch)==""` (missing vs ""), `e(N)=="74"` are
  // all r(109) "type mismatch" on every surface (refute3_capture.log A9/A10/
  // D10/N1/M1-M7). Python's "unequal" answered 0 here, which made `local eq =
  // (e(cmd)==1)` and `if e(cmd)==1 {` silently wrong. Raised on the same
  // `r(NNN): ...` channel exprfns.cpp's string() uses (BUG-246/249), so
  // evalArith forwards it and classifyFailedEqExpr maps it to rc 109.
  if (x.isStr() != y.isStr()) throw PyExc("r(109): type mismatch");
  if (x.isStr()) return x.strVal() == y.strVal();
  double dx = x.numAsDouble(), dy = y.numAsDouble();
  return dx == dy;  // NaN != NaN naturally
}

// Python ordering (raises TypeError with fn objects; a string ordered against a
// number is Stata's r(109), below).
bool pyOrder(Tok op, const EvalVal& a, const EvalVal& b) {
  if (a.isFn || b.isFn) throw PyExc("TypeError: ordering not supported on function objects");
  const PyValue& x = a.v;
  const PyValue& y = b.v;
  if (x.isStr() && y.isStr()) {
    int c = x.strVal().compare(y.strVal());
    switch (op) {
      case Tok::Lt: return c < 0;
      case Tok::Gt: return c > 0;
      case Tok::Le: return c <= 0;
      case Tok::Ge: return c >= 0;
      default: break;
    }
    throw PyExc("bad cmp op");
  }
  // A string against a number is Stata's type mismatch, as for == above:
  // `display 3 > "a"`, `local m = 3 > "a"` and `scalar m = "a" < 3` are r(109)
  // in StataBE 19.5. The Python TypeError left display to echo `3 > "a"`.
  // BUG-1122: so are `generate z = (s > 1)`, `count if s >= 1` and `generate z
  // = x >= s` (StataBE 19.5, 2026-11-03); the TypeError gave them rc 1.
  if (x.isStr() || y.isStr()) throw PyExc("r(109): type mismatch");
  double dx = x.numAsDouble(), dy = y.numAsDouble();
  switch (op) {
    case Tok::Lt: return dx < dy;
    case Tok::Gt: return dx > dy;
    case Tok::Le: return dx <= dy;
    case Tok::Ge: return dx >= dy;
    default: break;
  }
  throw PyExc("bad cmp op");
}

bool pyCompareOne(Tok op, const EvalVal& a, const EvalVal& b) {
  switch (op) {
    case Tok::Eq: return pyEquals(a, b);
    case Tok::Ne: return !pyEquals(a, b);
    default: return pyOrder(op, a, b);
  }
}

struct Evaluator {
  // Exactly one of these is set. `ns` = ordinary evalPyExpr mode (resolve names
  // against a live namespace). `slots` = prebound mode (each Name node already
  // carries its resolution; a Bind::Slot reads slots[node.slot]).
  const PyNamespace* ns = nullptr;
  const std::vector<PyValue>* slots = nullptr;

  EvalVal evalName(const Node& n) {
    if (slots != nullptr) {
      switch (n.bind) {
        case Node::Bind::Slot:
          return EvalVal::of((*slots)[static_cast<size_t>(n.slot)]);
        case Node::Bind::Const:
          return EvalVal::of(n.constVal);
        case Node::Bind::Fn:
          return EvalVal::ofFn(n.text, &n.fn);
        case Node::Bind::Unresolved:
          break;  // fall through to the same NameError the namespace path raises
      }
      throw PyExc("NameError: name '" + n.text + "' is not defined");
    }
    // EXPRLAYER-1c: ALL consts (own before base), then ALL fns (own before
    // base). The two sweeps are not interleaved on purpose: the historical code
    // merged every level into ONE consts map and ONE fns map and looked in
    // consts first, so a base CONST outranks an own FN of the same name. Own
    // before base inside each sweep reproduces "later insertion wins", which is
    // what building the registry first and the per-call values after produced.
    // See PyNamespace's header comment.
    for (const PyNamespace* p = ns; p != nullptr; p = p->base) {
      auto ci = p->consts.find(n.text);
      if (ci != p->consts.end()) return EvalVal::of(ci->second);
    }
    for (const PyNamespace* p = ns; p != nullptr; p = p->base) {
      auto fi = p->fns.find(n.text);
      if (fi != p->fns.end()) return EvalVal::ofFn(n.text, &fi->second);
    }
    throw PyExc("NameError: name '" + n.text + "' is not defined");
  }

  EvalVal eval(const Node& n) {
    switch (n.t) {
      case Node::T::Int:
        return EvalVal::of(PyValue::integer(n.intVal));
      case Node::T::Float:
        return EvalVal::of(PyValue::floating(n.floatVal));
      case Node::T::Str:
        return EvalVal::of(PyValue::str(n.text));
      case Node::T::Name:
        return evalName(n);
      case Node::T::Not: {
        EvalVal a = eval(*n.a);
        // Review finding (v3): Stata's `!` over a string is r(109) type
        // mismatch (StataBE 19.5: `local j = !"a"` and `!""` both rc 109);
        // truthy() answered 0 for `!"a"`. Python's `not` keyword (op End)
        // keeps its own semantics.
        if (n.op == Tok::Bang && !a.isFn && a.v.isStr()) throw PyExc("r(109): type mismatch");
        return EvalVal::of(PyValue::boolean(!a.truthy()));
      }
      case Node::T::Bool: {
        // BUG-660 fix round: Tok::Amp/Tok::Pipe (Stata's `&`/`|`) ALWAYS
        // coerce to a genuine 0/1 boolean, short-circuit — unlike Python's
        // and/or below, which return the deciding OPERAND unchanged. Stata's
        // `. | 0` has to render `1`, not the missing sentinel a Python-style
        // `or` would hand back.
        //
        // Review finding (v3): every operand is evaluated and type-checked
        // first; there is no short-circuit. StataBE 19.5 measured: `"a" & 1`,
        // `"x" | 0`, `1 | "a"` and `0 & "a"` are all r(109) type mismatch
        // (truthy() had answered 1, 1, 1, 0), and `local x = 1 | runiform()`
        // consumes a draw (the next runiform() after `set seed 1` is the
        // second draw of the stream, not the first).
        if (n.op == Tok::Amp || n.op == Tok::Pipe) {
          const bool isOr = n.op == Tok::Pipe;
          bool anyTrue = false, allTrue = true;
          for (auto& part : n.parts) {
            const EvalVal v = eval(*part);
            if (!v.isFn && v.v.isStr()) throw PyExc("r(109): type mismatch");
            const bool t = v.truthy();
            anyTrue = anyTrue || t;
            allTrue = allTrue && t;
          }
          return EvalVal::of(PyValue::boolean(isOr ? anyTrue : allTrue));
        }
        // and/or return the deciding OPERAND (not a bool), short-circuit.
        if (n.op == Tok::Or) {
          EvalVal last = eval(*n.parts[0]);
          for (size_t k = 1; k < n.parts.size(); ++k) {
            if (last.truthy()) return last;
            last = eval(*n.parts[k]);
          }
          return last;
        }
        EvalVal last = eval(*n.parts[0]);
        for (size_t k = 1; k < n.parts.size(); ++k) {
          if (!last.truthy()) return last;
          last = eval(*n.parts[k]);
        }
        return last;
      }
      case Node::T::Compare: {
        EvalVal left = stataOperand(eval(*n.a));
        for (size_t k = 0; k < n.cmpOps.size(); ++k) {
          EvalVal right = stataOperand(eval(*n.cmpOperands[k]));
          if (!pyCompareOne(n.cmpOps[k], left, right)) {
            return EvalVal::of(PyValue::boolean(false));
          }
          left = std::move(right);
        }
        return EvalVal::of(PyValue::boolean(true));
      }
      case Node::T::Unary: {
        EvalVal a = eval(*n.a);
        if (a.isFn) throw PyExc("TypeError: bad operand for unary op");
        // BUG-1122: Stata's unary minus on a string is r(109) type mismatch
        // (`generate z = -s`, `display -"a"`: StataBE 19.5, measured
        // 2026-11-03); its unary plus on one is r(198), left to the generic
        // failure.
        if (a.v.isStr() && n.op == Tok::Minus) throw PyExc("r(109): type mismatch");
        if (a.v.isStr()) throw PyExc("TypeError: bad operand type for unary +: 'str'");
        if (n.op == Tok::Minus) {
          // `-.a` is `.`: negating a missing stand-in must not produce a huge
          // negative real number (see evalBinNumeric).
          if (isStataMissingOperand(a.v) || isStataOverflowOperand(a.v))
            return EvalVal::of(PyValue::floating(kStataMissingSentinel));
          if (a.v.isFloat()) return EvalVal::of(PyValue::floating(-a.v.floatVal()));
          // -bool -> int in Python (-True == -1)
          long long iv = a.v.intVal();
          if (iv == LLONG_MIN) return EvalVal::of(PyValue::floating(-static_cast<double>(iv)));
          return EvalVal::of(PyValue::integer(-iv));
        }
        // unary +: bool -> int, others unchanged
        if (a.v.isBool()) return EvalVal::of(PyValue::integer(a.v.intVal()));
        return a;
      }
      case Node::T::Bin: {
        EvalVal a = eval(*n.a);
        EvalVal b = eval(*n.b);
        if (a.isFn || b.isFn) throw PyExc("TypeError: unsupported operand (function object)");
        const PyValue& x = a.v;
        const PyValue& y = b.v;
        // String cases.
        if (x.isStr() || y.isStr()) {
          if (n.op == Tok::Plus && x.isStr() && y.isStr()) {
            return EvalVal::of(PyValue::str(x.strVal() + y.strVal()));
          }
          if (n.op == Tok::Star && (x.isStr() != y.isStr())) {
            // str * n (or n * str) repeats the string; bool counts as int. A
            // non-integer n counts its whole part and a missing one none:
            // StataBE 19.5 has 2.5*"ab" and 2.9999*"ab" abab, x[1]*"ab" with
            // x[1] = 1.5 ab, and .*"ab" "". A float past 2^31 (longer than any
            // Stata string) keeps the TypeError.
            const PyValue& s = x.isStr() ? x : y;
            const PyValue& m = x.isStr() ? y : x;
            long long times = m.intVal();
            if (m.isFloat()) {
              const double t = m.floatVal();
              const bool missing = !std::isfinite(t) || std::fabs(t) >= kStataMissingSentinel;
              if (!missing && t >= 2147483648.0)
                throw PyExc("TypeError: can't multiply sequence by float");
              times = !missing && t >= 1 ? static_cast<long long>(t) : 0;
            }
            std::string out;
            for (long long k = 0; k < times; ++k) out += s.strVal();
            return EvalVal::of(PyValue::str(out));
          }
          // BUG-456: `"5" + 1` (an e-macro that LOOKS numeric, in arithmetic) is
          // r(109) in Stata (refute3 Z01-Z40: `display e(m)+1` rc 109 for every
          // numeric-looking text). Announced on the r(NNN) channel so the
          // surfaces report 109, not the 198 a generic TypeError collapsed to.
          throw PyExc("r(109): type mismatch");
        }
        return EvalVal::of(evalBinNumeric(n.op, x, y));
      }
      case Node::T::Call: {
        EvalVal callee = eval(*n.a);
        if (!callee.isFn) throw PyExc("TypeError: object is not callable");
        // Stata evaluates a function's arguments right to left, every one of them
        // (StataBE 19.5, 2026-11-03): after `set seed 1`, `display
        // min(runiform(), runiform()+10)` and `cond(1, runiform(), runiform()+10)`
        // both show the SECOND draw, and `display max(foo, bar)` says "bar not
        // found". Binary operators stay left to right (Bin above).
        std::vector<PyValue> args(n.parts.size());
        for (size_t k = n.parts.size(); k-- > 0;) {
          EvalVal a = eval(*n.parts[k]);
          if (a.isFn) throw PyExc("TypeError: function object as argument");
          args[k] = a.v;
        }
        PyValue result = (*callee.fn)(args);
        // BUG-1358: on the Stata expression surfaces (StataDivisionScope) a
        // function's NaN -- the out-of-domain answer of invttail(5,2), mod(5,0),
        // max(.a,.b), year(.a) -- is the missing sentinel. A NaN prints `.` but
        // compares false, so `invttail(5,2) == .` was 0 and `invttail(5,2) > 5`
        // was 0 where Stata answers 1 and 1. An infinity a function passes
        // through stays one, as in Stata (missing(min(1,-1e300*1e10)) is 0); the
        // operator that reads it sees `.` (isStataOverflowOperand).
        if (tl_stataDivision && result.isFloat() && std::isnan(result.floatVal()))
          return EvalVal::of(PyValue::floating(kStataMissingSentinel));
        return EvalVal::of(std::move(result));
      }
    }
    throw PyExc("internal: bad node");
  }
};

// Walk the AST once, resolving every Name node to a slot / captured const /
// captured fn (compilePreboundExpr). Mutates the tree in place. Precedence is
// rowVars (per-row-varying names) > constNs.consts > constNs.fns, mirroring
// evalPyExpr's contract that a per-row rebind shadows a same-named namespace
// entry (see PreboundExpr's header comment).
struct PreboundResolver {
  const PyNamespace& constNs;
  const std::set<std::string>& rowVars;
  std::vector<std::string>& slotNames;    // out: slot index -> name
  std::map<std::string, int> slotIndex;   // name -> slot (dedup repeats)

  int slotFor(const std::string& name) {
    auto it = slotIndex.find(name);
    if (it != slotIndex.end()) return it->second;
    int idx = static_cast<int>(slotNames.size());
    slotNames.push_back(name);
    slotIndex.emplace(name, idx);
    return idx;
  }

  void resolveName(Node& n) {
    const std::string& nm = n.text;
    if (rowVars.count(nm) != 0) {
      n.bind = Node::Bind::Slot;
      n.slot = slotFor(nm);
      return;
    }
    // EXPRLAYER-1c: same two-sweep base-chain walk as Evaluator::evalName —
    // both resolvers must agree or a prebound expression and an interpreted one
    // would bind the same name differently.
    for (const PyNamespace* p = &constNs; p != nullptr; p = p->base) {
      auto ci = p->consts.find(nm);
      if (ci != p->consts.end()) {
        n.bind = Node::Bind::Const;
        n.constVal = ci->second;
        return;
      }
    }
    for (const PyNamespace* p = &constNs; p != nullptr; p = p->base) {
      auto fi = p->fns.find(nm);
      if (fi != p->fns.end()) {
        n.bind = Node::Bind::Fn;
        n.fn = fi->second;  // copy the std::function so the compiled expr is self-contained
        return;
      }
    }
    n.bind = Node::Bind::Unresolved;  // NameError at eval, exactly as evalPyExpr
  }

  void walk(Node& n) {
    switch (n.t) {
      case Node::T::Name:
        resolveName(n);
        return;
      case Node::T::Unary:
      case Node::T::Not:
        if (n.a) walk(*n.a);
        return;
      case Node::T::Bin:
        if (n.a) walk(*n.a);
        if (n.b) walk(*n.b);
        return;
      case Node::T::Bool:
        for (auto& p : n.parts) walk(*p);
        return;
      case Node::T::Compare:
        if (n.a) walk(*n.a);
        for (auto& p : n.cmpOperands) walk(*p);
        return;
      case Node::T::Call:
        if (n.a) walk(*n.a);          // callee (a bare fn name)
        for (auto& p : n.parts) walk(*p);  // args
        return;
      case Node::T::Int:
      case Node::T::Float:
      case Node::T::Str:
        return;
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API

PyEvalOutcome evalPyExprDetailed(const std::string& expr, const PyNamespace& ns) {
  dprof::EScope _z(dprof::kEPyParseEval);
  PyEvalOutcome out;
  try {
    Parser p(expr);
    NodePtr root = p.parseExpr();
    if (p.cur.kind != Tok::End) throw PyExc("SyntaxError: trailing tokens");
    Evaluator ev;
    ev.ns = &ns;
    EvalVal v = ev.eval(*root);
    if (v.isFn) {
      out.status = PyEvalOutcome::Status::FnObject;
      out.fnName = v.fnName;
      return out;
    }
    out.status = PyEvalOutcome::Status::Value;
    out.value = v.v;
    return out;
  } catch (const PyExc& e) {
    out.status = PyEvalOutcome::Status::Raised;
    out.error = e.what;
    return out;
  }
}

std::optional<PyValue> evalPyExpr(const std::string& expr, const PyNamespace& ns) {
  PyEvalOutcome out = evalPyExprDetailed(expr, ns);
  if (out.status == PyEvalOutcome::Status::Value) return out.value;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Prebound (compile-once, evaluate-per-row) evaluation

struct PreboundExpr::Impl {
  NodePtr root;                        // the parsed AST, resolved in place
  std::vector<std::string> slotNames;  // slot index -> name (first-reference order)
  bool ok = false;
  std::string error;                   // parse-error diagnostic when !ok
};

PreboundExpr::PreboundExpr() = default;
PreboundExpr::~PreboundExpr() = default;
PreboundExpr::PreboundExpr(PreboundExpr&&) noexcept = default;
PreboundExpr& PreboundExpr::operator=(PreboundExpr&&) noexcept = default;

bool PreboundExpr::ok() const { return impl_ && impl_->ok; }

const std::string& PreboundExpr::error() const {
  static const std::string kEmpty;
  return impl_ ? impl_->error : kEmpty;
}

const std::vector<std::string>& PreboundExpr::rowSlotNames() const {
  static const std::vector<std::string> kEmpty;
  return impl_ ? impl_->slotNames : kEmpty;
}

std::optional<PyValue> PreboundExpr::eval(const std::vector<PyValue>& slots) const {
  if (!impl_ || !impl_->ok) return std::nullopt;
  try {
    Evaluator ev;
    ev.slots = &slots;
    EvalVal v = ev.eval(*impl_->root);
    if (v.isFn) return std::nullopt;  // bare function object -> nullopt (mirrors evalPyExpr)
    return v.v;
  } catch (const PyExc&) {
    return std::nullopt;
  }
}

PreboundExpr compilePreboundExpr(const std::string& expr, const PyNamespace& constNs,
                                 const std::set<std::string>& rowVars) {
  PreboundExpr pe;
  pe.impl_ = std::make_unique<PreboundExpr::Impl>();
  try {
    Parser p(expr);
    NodePtr root = p.parseExpr();
    if (p.cur.kind != Tok::End) throw PyExc("SyntaxError: trailing tokens");
    PreboundResolver res{constNs, rowVars, pe.impl_->slotNames, {}};
    res.walk(*root);
    pe.impl_->root = std::move(root);
    pe.impl_->ok = true;
  } catch (const PyExc& e) {
    pe.impl_->ok = false;
    pe.impl_->error = e.what;
  }
  return pe;
}

// ---------------------------------------------------------------------------
// Math builtins with Python exception semantics

namespace {

double numArg(const PyValue& v, const char* fn) {
  if (v.isStr()) throw PyExc(std::string(fn) + ": TypeError (str)");
  return v.numAsDouble();
}

void arity(const std::vector<PyValue>& args, size_t lo, size_t hi, const char* fn) {
  if (args.size() < lo || args.size() > hi) throw PyExc(std::string(fn) + ": TypeError arity");
}

PyValue mSqrt(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "sqrt");
  double x = numArg(a[0], "sqrt");
  if (x < 0) throw PyExc("ValueError: math domain error");
  return PyValue::floating(std::sqrt(x));
}

double logChecked(double x) {
  if (x <= 0) throw PyExc("ValueError: math domain error");
  return std::log(x);
}

PyValue mLog(const std::vector<PyValue>& a) {
  arity(a, 1, 2, "log");
  double x = numArg(a[0], "log");
  if (a.size() == 1) return PyValue::floating(logChecked(x));
  double base = numArg(a[1], "log");
  double den = logChecked(base);
  if (den == 0.0) throw PyExc("ZeroDivisionError: log base 1");
  return PyValue::floating(logChecked(x) / den);
}

PyValue mLog2(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "log2");
  double x = numArg(a[0], "log2");
  if (x <= 0) throw PyExc("ValueError: math domain error");
  return PyValue::floating(std::log2(x));
}

PyValue mLog10(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "log10");
  double x = numArg(a[0], "log10");
  if (x <= 0) throw PyExc("ValueError: math domain error");
  return PyValue::floating(std::log10(x));
}

PyValue mExp(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "exp");
  double x = numArg(a[0], "exp");
  double r = std::exp(x);
  if (std::isinf(r) && std::isfinite(x)) throw PyExc("OverflowError: math range error");
  return PyValue::floating(r);
}

PyValue mAbs(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "abs");
  const PyValue& v = a[0];
  if (v.isStr()) throw PyExc("TypeError: bad operand type for abs(): 'str'");
  if (v.isFloat()) return PyValue::floating(std::fabs(v.floatVal()));
  long long iv = v.intVal();
  if (iv == LLONG_MIN) return PyValue::floating(-static_cast<double>(iv));
  return PyValue::integer(iv < 0 ? -iv : iv);  // bool -> int (abs(True) == 1)
}

PyValue minMax(const std::vector<PyValue>& a, bool isMin, const char* fn) {
  if (a.empty()) throw PyExc(std::string(fn) + ": TypeError no args");
  if (a.size() == 1) {
    // Python min(x) requires an iterable -> TypeError for scalars.
    throw PyExc(std::string(fn) + ": TypeError: object is not iterable");
  }
  bool anyStr = false, anyNum = false;
  for (const auto& v : a) {
    (v.isStr() ? anyStr : anyNum) = true;
  }
  if (anyStr && anyNum) throw PyExc(std::string(fn) + ": TypeError mixed str/num");
  // CPython's loop: `if item < best` (min) / `if item > best` (max) — the
  // running best changes only on a strictly-ordered win, so a NaN in the
  // FIRST slot sticks (min(nan, 1) == nan) while later NaNs never win.
  size_t best = 0;
  for (size_t i = 1; i < a.size(); ++i) {
    bool win;
    if (anyStr) {
      int c = a[i].strVal().compare(a[best].strVal());
      win = isMin ? (c < 0) : (c > 0);
    } else {
      double di = a[i].numAsDouble(), db = a[best].numAsDouble();
      win = isMin ? (di < db) : (di > db);
    }
    if (win) best = i;
  }
  return a[best];
}

PyValue mMin(const std::vector<PyValue>& a) { return minMax(a, true, "min"); }
PyValue mMax(const std::vector<PyValue>& a) { return minMax(a, false, "max"); }

PyValue mRound(const std::vector<PyValue>& a) {
  arity(a, 1, 2, "round");
  const PyValue& v = a[0];
  if (v.isStr()) throw PyExc("TypeError: type str doesn't define __round__");
  if (a.size() == 1) {
    if (v.isFloat()) {
      double d = v.floatVal();
      if (std::isnan(d)) throw PyExc("ValueError: cannot convert float NaN to integer");
      if (std::isinf(d)) throw PyExc("OverflowError: cannot convert float infinity to integer");
      // Python round(): banker's rounding (half to even).
      double r = std::nearbyint(d);  // FE_TONEAREST default = half-even
      if (std::fabs(r) >= 9.3e18) return PyValue::floating(r);  // bignum divergence guard
      return PyValue::integer(static_cast<long long>(r));
    }
    // round(int) -> the int itself (bool -> int: round(True) == 1)
    return PyValue::integer(v.intVal());
  }
  // 2-arg form: ndigits must be an integer (bool ok).
  const PyValue& nd = a[1];
  if (nd.isStr() || nd.isFloat()) throw PyExc("TypeError: ndigits must be an integer");
  long long ndig = nd.intVal();
  if (v.isInt() || v.isBool()) {
    // round(int, n>=0) -> int unchanged; negative n rounds at powers of 10
    // (half-even). Negative-n int rounding is out of corpus scope.
    if (ndig >= 0) return PyValue::integer(v.intVal());
    throw PyExc("round(int, negative) not supported in port (documented)");
  }
  double d = v.floatVal();
  if (std::isnan(d) || std::isinf(d)) return PyValue::floating(d);  // round(nan,2) -> nan
  if (ndig < 0) throw PyExc("round(float, negative) not supported in port (documented)");
  if (ndig > 300) return PyValue::floating(d);
  // Correctly-rounded decimal rounding: format with %.{n}f (exact decimal
  // conversion + round-half-even on the true binary value) and parse back —
  // matches CPython's _Py_dg_dtoa-based round().
  char spec[16], buf[512];
  std::snprintf(spec, sizeof(spec), "%%.%lldf", ndig);
  std::snprintf(buf, sizeof(buf), spec, d);
  return PyValue::floating(std::strtod(buf, nullptr));
}

PyValue mFloor(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "floor");
  const PyValue& v = a[0];
  if (v.isStr()) throw PyExc("TypeError: must be real number");
  if (!v.isFloat()) return PyValue::integer(v.intVal());
  double d = v.floatVal();
  if (std::isnan(d)) throw PyExc("ValueError: cannot convert float NaN to integer");
  if (std::isinf(d)) throw PyExc("OverflowError: cannot convert float infinity to integer");
  double r = std::floor(d);
  if (std::fabs(r) >= 9.3e18) return PyValue::floating(r);
  return PyValue::integer(static_cast<long long>(r));
}

PyValue mCeil(const std::vector<PyValue>& a) {
  arity(a, 1, 1, "ceil");
  const PyValue& v = a[0];
  if (v.isStr()) throw PyExc("TypeError: must be real number");
  if (!v.isFloat()) return PyValue::integer(v.intVal());
  double d = v.floatVal();
  if (std::isnan(d)) throw PyExc("ValueError: cannot convert float NaN to integer");
  if (std::isinf(d)) throw PyExc("OverflowError: cannot convert float infinity to integer");
  double r = std::ceil(d);
  if (std::fabs(r) >= 9.3e18) return PyValue::floating(r);
  return PyValue::integer(static_cast<long long>(r));
}

}  // namespace

PyNamespace mathNamespace(bool withInfNan) {
  PyNamespace ns;
  ns.fns["sqrt"] = mSqrt;
  ns.fns["log"] = mLog;
  ns.fns["log2"] = mLog2;
  ns.fns["log10"] = mLog10;
  ns.fns["exp"] = mExp;
  ns.fns["abs"] = mAbs;
  ns.fns["min"] = mMin;
  ns.fns["max"] = mMax;
  ns.fns["round"] = mRound;
  ns.fns["floor"] = mFloor;
  ns.fns["ceil"] = mCeil;
  // BUG-1388: no bare `pi` or `e`. They were Python's math constants, so
  // `` `=e + 1' `` expanded to 3.718281828459045 and `display pi` printed
  // 3.1415927. Stata's constant is `_pi` (normalizeStataLiterals substitutes
  // it as text), and StataBE 19.5 answers `e not found` / `pi not found`
  // r(111) for the bare names on every surface.
  if (withInfNan) {
    ns.consts["inf"] = PyValue::floating(HUGE_VAL);
    ns.consts["nan"] = PyValue::floating(kNaN);
  }
  dprof::noteNsBuild(11);  // 11 fn-map insertions above
  return ns;
}

// EXPRLAYER-1c: the postfile/evalMacroExpr registry, built once per process.
// (The Stata-function registry lives in exprfns.cpp, next to addStataFns.)
const PyNamespace& postfileMathNamespace() {
  static const PyNamespace kNs = mathNamespace(/*withInfNan=*/true);
  return kNs;
}

StataDivisionScope::StataDivisionScope() : prev_(tl_stataDivision) { tl_stataDivision = true; }
StataDivisionScope::~StataDivisionScope() { tl_stataDivision = prev_; }

}  // namespace piet::eval
