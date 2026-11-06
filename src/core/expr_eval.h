// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — the shared scalar-expression evaluator.
//
// Promoted from src/estimators/nlcom.h's recursive-descent parser so `nl` and
// `gmm` can evaluate a user expression over DATA COLUMNS and {name=init}
// parameters without a second copy of the grammar. nlcom keeps its exact
// semantics by delegating here with a resolver that only knows `_b[...]`.
//
// WHY ONE EVALUATOR. A second copy of this grammar is a defect class this
// codebase has already paid for: PIET's CiParser shadowed the engine's
// expression semantics and a single-layer fix produced 3-of-7 silently wrong
// answers. So nlcom does not keep its own parser — it compiles through the SAME
// Program below with `allow_params`/`allow_vars` switched off, which is exactly
// how its old grammar behaved (`{` fell through to the number scan and errored;
// a bare identifier errored). Every arithmetic node is evaluated in the same
// order and associativity as before, so nlcom's numbers are bit-identical.
//
// GRAMMAR (unchanged from nlcom's, deliberately — widening it would widen
// nlcom's accepted input too):
//     expr   := term   (('+'|'-') term)*            left-associative
//     term   := unary  (('*'|'/') unary)*           left-associative
//     unary  := ('-'|'+') unary | factor
//     factor := base ('^' powrhs)*                  LEFT-associative
//     powrhs := ('-'|'+') powrhs | base
//     base   := '(' expr ')'
//             | '_b[' NAME ']'                       (allow_coefs)
//             | '{' NAME ['=' NUMBER] '}'            (allow_params)
//             | ('ln'|'log'|'exp'|'sqrt'|'abs'
//                |'invlogit'|'logit'|'normal'|'normalden') '(' expr ')'
//             | '_pi'                                 (nullary constant)
//             | IDENT                                (allow_vars)
//             | NUMBER
//
// NOTE the precedence of unary minus and '^' (FU-811), from the StataBE oracle
// w2ac_expr_precedence: negation binds looser than '^', so `-2^2` is -(2^2) = -4
// and `-_b[x]^2` is -(_b[x]^2) (disp_neg_pow, nlcom_negpow, nlcom_const); '^' is
// LEFT-associative, so `2^3^2` is (2^3)^2 = 64 and `_b[x]^2^3` is _b[x]^6
// (disp_chain_const, nlcom_chain); a sign right of '^' covers only the next base,
// so `2^-2` = .25, `-2^-b` = -(2^(-b)) (nlcom_negexp) and `2^-3^2` = (2^-3)^2
// (disp_pow_neg_pow). Until FU-811 the minus sat inside `base` (binding TIGHTER
// than '^') and '^' was right-associative: silent wrong answers in nlcom/nl/gmm.
// tests/unit/test_nlgmm.cpp pins the oracle's values.
//
// COMPILE ONCE, EVALUATE MANY. nlcom re-parsed the string for every evaluation,
// which is fine for a handful of delta-method perturbations but not for `nl`,
// which evaluates the same expression n * (1 + 2k) times PER ITERATION. So
// compile() builds an AST and eval() walks it. The parse is the identical
// recursive descent; only the thing it returns changed (a node instead of a
// double).
#pragma once
#include "core/distributions.h"
#include "core/ident.h"  // non-ASCII bytes are identifier bytes (Unicode names)
#include "core/stata_arith.h"

#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cecon {
namespace expr {

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// One `{name}` / `{name=init}` parameter, recorded in FIRST-APPEARANCE order.
// `init_set` distinguishes "the user wrote {b=0}" from "the user wrote {b}" —
// both start the solver at 0, but only the first is a stated starting value.
struct Param {
    std::string name;
    double init = 0.0;
    bool init_set = false;
};

// Resolve a name to a scalar FOR THE CURRENT ROW. Returns false when the name is
// unknown — the evaluator then throws. Returning 0 for an unknown name would be
// the silent-wrong-answer failure mode this whole file exists to avoid.
using Resolver = std::function<bool(const std::string& name, double& out)>;

// Where eval() reads bare-identifier (data column) values from. `values`, when
// non-null, is indexed by Program::variables() order — the fast path `nl`/`gmm`
// use, one pointer bump per node instead of a std::function call per node per
// row. `fn` is the by-name resolver form. Exactly one code path in eval() reads
// either; there is no second evaluator.
struct VarSource {
    const double* values = nullptr;
    const Resolver* fn = nullptr;
};

struct Options {
    std::string tag = "expr";   // error-message prefix ("nlcom" reproduces nlcom's text)
    bool allow_params = true;   // {name} / {name=init}
    bool allow_vars = true;     // bare identifiers, resolved per row by the caller
    bool allow_coefs = true;    // _b[name]
    // Appended to the "bare identifier" rejection when allow_vars is false. The
    // default is nlcom's exact wording, which is the only caller that turns
    // bare identifiers off.
    std::string bare_ident_hint = " (reference coefficients as _b[name])";
};

enum class Op {
    Num, Add, Sub, Mul, Div, Pow, Neg,
    Ln, Exp, Sqrt, Abs, Float, Cond,
    Invlogit, Logit, Normal, Normalden, Pi,
    ParamRef, VarRef, CoefRef,
};

struct Node;
using NodeP = std::unique_ptr<Node>;

struct Node {
    Op op = Op::Num;
    double num = 0.0;
    int idx = -1;       // ParamRef / VarRef: index into params()/variables()
    std::string name;   // VarRef / CoefRef: the name, for the resolver and diagnostics
    NodeP a, b, c;
};

class Program {
  public:
    // Parse `text`. Throws std::runtime_error (message prefixed with opts.tag)
    // on any malformed input; the parse is total, so a Program that exists is a
    // Program that can be evaluated.
    static Program compile(const std::string& text, const Options& opts);

    // `params` is indexed by params() order (may be null when there are none).
    // `coefs` resolves `_b[name]` (may be null when allow_coefs is false).
    double eval(const double* params, const VarSource& vars, const Resolver* coefs) const {
        return eval_node(*root_, params, vars, coefs);
    }

    // Convenience for callers with no data columns (nlcom).
    double eval(const double* params, const Resolver& coefs) const {
        return eval(params, VarSource{}, &coefs);
    }

    const std::vector<Param>& params() const { return params_; }
    const std::vector<std::string>& variables() const { return vars_; }
    const std::string& text() const { return text_; }

    // Every rejection in this file goes through here, so one tag reproduces the
    // caller's message prefix exactly (nlcom's messages are unchanged).
    [[noreturn]] void die(const std::string& msg) const {
        throw std::runtime_error(opts_.tag + ": " + msg);
    }

  private:
    friend struct Parser;
    Options opts_;
    std::string text_;
    NodeP root_;
    std::vector<Param> params_;
    std::vector<std::string> vars_;

    double eval_node(const Node& n, const double* params, const VarSource& vars,
                     const Resolver* coefs) const {
        switch (n.op) {
            case Op::Num: return n.num;
            case Op::Add: return eval_node(*n.a, params, vars, coefs) + eval_node(*n.b, params, vars, coefs);
            case Op::Sub: return eval_node(*n.a, params, vars, coefs) - eval_node(*n.b, params, vars, coefs);
            case Op::Mul: return eval_node(*n.a, params, vars, coefs) * eval_node(*n.b, params, vars, coefs);
            case Op::Div: return eval_node(*n.a, params, vars, coefs) / eval_node(*n.b, params, vars, coefs);
            case Op::Pow: {
                // Stata's `^` (core/stata_arith.h), as generate computes it: x^2 is
                // the correctly rounded x*x (this platform's pow(x, 2) misses it by 1 ulp
                // on ~0.13% of doubles; on StataBE 19.5's lf_052 lnf columns x*x matches
                // 1000/1000 values and pow() 998/1000, codex-recovery/shape-cross.log).
                const double x = eval_node(*n.a, params, vars, coefs);
                return stata_pow_value(x, eval_node(*n.b, params, vars, coefs));
            }
            case Op::Neg: return -eval_node(*n.a, params, vars, coefs);
            case Op::Ln:  return std::log(eval_node(*n.a, params, vars, coefs));
            case Op::Exp: return stata_exp_value(eval_node(*n.a, params, vars, coefs));
            case Op::Sqrt: return std::sqrt(eval_node(*n.a, params, vars, coefs));
            case Op::Abs: return std::fabs(eval_node(*n.a, params, vars, coefs));
            case Op::Float:
                return static_cast<double>(static_cast<float>(eval_node(*n.a, params, vars, coefs)));
            case Op::Cond:
                // Evaluate only the selected arm: 0 * NaN would not implement
                // a conditional, and LF programs often have branch-only domains.
                return eval_node(*n.a, params, vars, coefs) != 0.0
                    ? eval_node(*n.b, params, vars, coefs)
                    : eval_node(*n.c, params, vars, coefs);
            case Op::Invlogit: {
                // Stata's invlogit() is two-branched: 1/(1+exp(-x)) for x >= 0
                // and exp(x)/(1+exp(x)) for x < 0. StataBE 19.5 %21x grid of 801
                // z in [-8, 8] (2026-10-27, codex-recovery/invlogit-grid.log):
                // this form matches 801/801; the single 1/(1+exp(-x)) form
                // misses 177 of the 400 negative z by 1 ulp. Same as
                // src/data/frame.cpp stata_invlogit. exp() is Stata's
                // (core/stata_arith.h): 0 below -709, so invlogit(-717.5) is 0,
                // not a subnormal.
                return stata_invlogit_value(eval_node(*n.a, params, vars, coefs));
            }
            case Op::Logit: {
                // Stata missing outside the open interval (0,1); NaN in is NaN out.
                const double x = eval_node(*n.a, params, vars, coefs);
                if (!(x > 0.0 && x < 1.0)) return std::nan("");
                return std::log(x / (1.0 - x));
            }
            case Op::Normal:
                // Stata's normal(): Cody's ANORM, bit-identical to StataBE
                // (src/core/distributions.h stata_normal).
                return cecon::stata_normal(eval_node(*n.a, params, vars, coefs));
            case Op::Normalden: {
                // Bit-identical to src/estimators/probit.cpp:11's norm_pdf.
                const double z = eval_node(*n.a, params, vars, coefs);
                return 0.3989422804014327 * std::exp(-0.5 * z * z);
            }
            case Op::Pi: return M_PI;
            case Op::ParamRef:
                if (!params) die("no parameter values supplied for '{" + n.name + "}'");
                return params[n.idx];
            case Op::VarRef: {
                if (vars.values) return vars.values[n.idx];
                double v = 0;
                if (vars.fn && (*vars.fn)(n.name, v)) return v;
                die("unknown variable '" + n.name + "'");
            }
            case Op::CoefRef: {
                double v = 0;
                if (coefs && (*coefs)(n.name, v)) return v;
                die("unknown coefficient '" + n.name + "'");
            }
        }
        die("internal: unhandled node");
    }
};

// The recursive-descent parser. Structurally identical to the one it replaces
// (src/estimators/nlcom.h before the P7 promotion) so the produced tree has the
// same shape, associativity and evaluation order as the old direct evaluation.
struct Parser {
    const std::string& s;
    Program& p;
    size_t i = 0;
    int depth = 0;

    Parser(const std::string& str, Program& prog) : s(str), p(prog) {}

    // RAII recursion-depth guard: every nesting level passes through base(), and
    // every sign unary()/powrhs() consumes takes one more level, so a cap there
    // bounds total recursion and rejects pathological input before the call stack
    // overflows.
    struct DepthGuard {
        const Program& p;
        int& d;
        DepthGuard(const Program& prog, int& x) : p(prog), d(x) {
            if (++d > 1000) p.die("expression nested too deeply");
        }
        ~DepthGuard() { --d; }
    };

    static NodeP mk(Op op, NodeP a = nullptr, NodeP b = nullptr) {
        NodeP n(new Node());
        n->op = op;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }

    NodeP parse() {
        NodeP v = expr();
        ws();
        if (i != s.size()) p.die("trailing input near '" + s.substr(i) + "'");
        return v;
    }

    // EM0: [U] 13.2.5 orders the binary operators /, *, -(subtraction), +, each level
    // left-associative: a+b-c is a+(b-c), a*b/c is a*(b/c) (oracle w2am_expr_binary_order
    // eo_nl_pm; 0.1+0.2-0.3 = 2^-55).
    NodeP expr() {
        NodeP v = subx();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '+') {
                ++i;
                NodeP r = subx();
                v = mk(Op::Add, std::move(v), std::move(r));
            } else {
                break;
            }
        }
        return v;
    }

    NodeP subx() {
        NodeP v = term();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '-') {
                ++i;
                NodeP r = term();
                v = mk(Op::Sub, std::move(v), std::move(r));
            } else {
                break;
            }
        }
        return v;
    }

    NodeP term() {
        NodeP v = divx();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '*') {
                ++i;
                NodeP r = divx();
                v = mk(Op::Mul, std::move(v), std::move(r));
            } else {
                break;
            }
        }
        return v;
    }

    NodeP divx() {
        NodeP v = unary();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '/') {
                ++i;
                NodeP r = unary();
                v = mk(Op::Div, std::move(v), std::move(r));
            } else {
                break;
            }
        }
        return v;
    }

    // Negation binds looser than '^' (see the NOTE above).
    // Only a consumed sign recurses here, so only that path takes a guard level:
    // a parenthesis costs one level (in base()), as it did before FU-811.
    NodeP unary() {
        ws();
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            const bool minus = s[i++] == '-';
            DepthGuard g(p, depth);
            NodeP v = unary();
            return minus ? mk(Op::Neg, std::move(v)) : std::move(v);
        }
        return factor();
    }

    // '^' is LEFT-associative and its right operand is a signed base, so
    // 2^3^2 = (2^3)^2 and 2^-3^2 = (2^-3)^2 (see the NOTE above).
    NodeP factor() {
        NodeP v = base();
        for (;;) {
            ws();
            if (!(i < s.size() && s[i] == '^')) break;
            ++i;
            NodeP r = powrhs();
            v = mk(Op::Pow, std::move(v), std::move(r));
        }
        return v;
    }

    // The right operand of '^': leading signs apply to the next base only.
    NodeP powrhs() {
        ws();
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            const bool minus = s[i++] == '-';
            DepthGuard g(p, depth);
            NodeP v = powrhs();
            return minus ? mk(Op::Neg, std::move(v)) : std::move(v);
        }
        return base();
    }

    NodeP base() {
        DepthGuard g(p, depth);
        ws();
        if (i < s.size() && s[i] == '(') {
            ++i;
            NodeP v = expr();
            ws();
            if (i >= s.size() || s[i] != ')') p.die("missing ')'");
            ++i;
            return v;
        }
        if (p.opts_.allow_coefs && s.compare(i, 3, "_b[") == 0) {
            i += 3;
            size_t st = i;
            while (i < s.size() && s[i] != ']') ++i;
            if (i >= s.size()) p.die("missing ']'");
            NodeP n = mk(Op::CoefRef);
            n->name = trim(s.substr(st, i - st));
            ++i;
            return n;
        }
        if (p.opts_.allow_params && i < s.size() && s[i] == '{') return param_ref();
        if (i < s.size() && cecon::ident::isIdentStartByte(s[i])) {
            size_t st = i;
            while (i < s.size() && cecon::ident::isIdentByte(s[i])) ++i;
            std::string id = s.substr(st, i - st);
            ws();
            if (i < s.size() && s[i] == '(') {
                ++i;
                NodeP x = expr();
                ws();
                if (id == "cond") {
                    if (i >= s.size() || s[i++] != ',') p.die("cond() requires three arguments");
                    NodeP yes = expr();
                    ws();
                    if (i >= s.size() || s[i++] != ',') p.die("cond() requires three arguments");
                    NodeP no = expr();
                    ws();
                    if (i >= s.size() || s[i++] != ')') p.die("missing ')'");
                    NodeP n = mk(Op::Cond, std::move(x), std::move(yes));
                    n->c = std::move(no);
                    return n;
                }
                if (i >= s.size() || s[i] != ')') p.die("missing ')'");
                ++i;
                if (id == "ln" || id == "log") return mk(Op::Ln, std::move(x));
                if (id == "exp") return mk(Op::Exp, std::move(x));
                if (id == "sqrt") return mk(Op::Sqrt, std::move(x));
                if (id == "abs") return mk(Op::Abs, std::move(x));
                if (id == "float") return mk(Op::Float, std::move(x));
                if (id == "invlogit") return mk(Op::Invlogit, std::move(x));
                if (id == "logit") return mk(Op::Logit, std::move(x));
                if (id == "normal") return mk(Op::Normal, std::move(x));
                if (id == "normalden") return mk(Op::Normalden, std::move(x));
                p.die("unknown function '" + id + "'");
            }
            // `_pi` is a NULLARY constant, recognised before the bare-identifier
            // branch: nlcom mode (allow_vars=false) must still accept `_pi` (Stata's
            // nlcom does), and no data column can be named `_pi` in Stata, so no
            // variable is ever shadowed.
            if (id == "_pi") return mk(Op::Pi);
            if (!p.opts_.allow_vars)
                p.die("bare identifier '" + id + "'" + p.opts_.bare_ident_hint);
            NodeP n = mk(Op::VarRef);
            n->name = id;
            n->idx = -1;
            for (size_t v = 0; v < p.vars_.size(); ++v)
                if (p.vars_[v] == id) { n->idx = static_cast<int>(v); break; }
            if (n->idx < 0) {
                n->idx = static_cast<int>(p.vars_.size());
                p.vars_.push_back(id);
            }
            return n;
        }
        return number();
    }

    // '{' NAME [ '=' NUMBER ] '}' — recorded in first-appearance order. A repeat
    // reference to an already-seen name reuses its slot; a repeat that restates a
    // DIFFERENT initial value is rejected rather than silently picking one.
    NodeP param_ref() {
        ++i;  // '{'
        size_t st = i;
        while (i < s.size() && s[i] != '}' && s[i] != '=') ++i;
        if (i >= s.size()) p.die("missing '}'");
        std::string nm = trim(s.substr(st, i - st));
        if (nm.empty()) p.die("empty parameter name in '{}'");
        for (char c : nm)
            if (!cecon::ident::isIdentByte(c))
                p.die("bad parameter name '" + nm + "' (letters, digits, _ and non-ASCII characters only)");
        bool has_init = false;
        double init = 0.0;
        if (s[i] == '=') {
            ++i;
            size_t vs = i;
            while (i < s.size() && s[i] != '}') ++i;
            if (i >= s.size()) p.die("missing '}'");
            std::string lit = trim(s.substr(vs, i - vs));
            if (lit.empty()) p.die("empty initial value for '{" + nm + "=}'");
            size_t used = 0;
            try {
                init = std::stod(lit, &used);
            } catch (const std::exception&) {
                p.die("bad initial value '" + lit + "' for parameter '" + nm + "'");
            }
            if (used != lit.size())
                p.die("bad initial value '" + lit + "' for parameter '" + nm + "'");
            has_init = true;
        }
        if (i >= s.size() || s[i] != '}') p.die("missing '}'");
        ++i;
        int idx = -1;
        for (size_t v = 0; v < p.params_.size(); ++v)
            if (p.params_[v].name == nm) { idx = static_cast<int>(v); break; }
        if (idx < 0) {
            idx = static_cast<int>(p.params_.size());
            Param pr;
            pr.name = nm;
            pr.init = init;
            pr.init_set = has_init;
            p.params_.push_back(pr);
        } else if (has_init) {
            Param& pr = p.params_[static_cast<size_t>(idx)];
            if (pr.init_set && pr.init != init)
                p.die("parameter '" + nm + "' is given two different initial values (" +
                      std::to_string(pr.init) + " and " + std::to_string(init) + ")");
            pr.init = init;
            pr.init_set = true;
        }
        NodeP n = mk(Op::ParamRef);
        n->idx = idx;
        n->name = nm;
        return n;
    }

    NodeP number() {
        size_t st = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' ||
                                ((s[i] == 'e' || s[i] == 'E') && i > st) ||
                                ((s[i] == '+' || s[i] == '-') && i > st &&
                                 (s[i - 1] == 'e' || s[i - 1] == 'E')))) ++i;
        if (i == st) p.die("parse error");
        NodeP n = mk(Op::Num);
        n->num = std::stod(s.substr(st, i - st));
        return n;
    }
};

inline Program Program::compile(const std::string& text, const Options& opts) {
    Program p;
    p.opts_ = opts;
    p.text_ = text;
    Parser parser(p.text_, p);
    p.root_ = parser.parse();
    return p;
}

}  // namespace expr
}  // namespace cecon
