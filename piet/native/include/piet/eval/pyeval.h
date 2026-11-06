// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// pyeval — a restricted Python-expression evaluator.
//
// The Python backend runs macro arithmetic through eval() with a
// no-builtins namespace (macros._eval_arith, postfile.eval_macro_expr).
// Whitelist regexes upstream guarantee the expression contains only:
// digits, whitespace, + - * / ( ) . ^(pre-rewritten to **) e E letters _
// , and (for the macros path) quote characters and < > = !.
//
// This module evaluates exactly that Python subset with Python semantics:
//   - int/float/bool/str object model (PyValue), true division, ** right-
//     assoc, // floor division, chained comparisons, and/or returning
//     operands, `not` prefix
//   - Python exception behavior mapped to PyExc: ZeroDivisionError,
//     TypeError (str<num ordering, str+num), ValueError/OverflowError from
//     math fns (sqrt(-1), log(0), exp(1000), round(nan)), NameError for
//     unknown names — the caller mirrors `except Exception: return None`.
//   - complex results (e.g. (-2)**0.5) are reported as PyExc: the Python
//     side returns a complex object which every M1-scope caller then
//     rejects via isinstance checks — except postfile.eval_macro_expr,
//     which would str() it; that path is a documented parity divergence
//     (see the M1 report; corpus excludes complex-producing cases).
//
// Namespace model: names bound to constant PyValues or to functions.
// Function implementations live in exprfns.{h,cpp} (Stata fn library) and
// in pyeval.cpp (math builtins with Python exception semantics).
#ifndef PIET_NATIVE_EVAL_PYEVAL_H
#define PIET_NATIVE_EVAL_PYEVAL_H

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "piet/eval/pyvalue.h"

namespace piet::eval {

using PyFn = std::function<PyValue(const std::vector<PyValue>&)>;

struct PyNamespace {
  std::map<std::string, PyValue> consts;
  std::map<std::string, PyFn> fns;

  // EXPRLAYER-1c (P2-PERF): an optional IMMUTABLE base namespace, consulted
  // only after this namespace's own two maps.
  //
  // WHY. mathNamespace()+addStataFns() insert 39 entries into two std::maps —
  // 39 red-black nodes and 39 std::function constructions — and every `if`,
  // every `while`, and every non-trivial `local x = <expr>` built the whole
  // thing from scratch. The EXPRLAYER zone profile measured 25.9 ms of S01's
  // condition path and 12.8 ms of its evalArith path inside that construction
  // alone (46,748 namespace builds, 911,586 function-map insertions in one S01
  // run). The registry is a compile-time constant; only the per-call additions
  // (_rc, _N/_n, dataset columns, session scalars, the scalar() closure) vary.
  //
  // LOOKUP ORDER IS THE SEMANTICS. The historical code inserted the registry
  // FIRST and the per-call values AFTER into one map, so a per-call value with
  // a registry name overwrote it — a dataset column or scalar named `pi` or
  // `log` won. Own-maps-before-base reproduces that exactly, in the same
  // consts-before-fns order within each level, for every consumer: base holds
  // what was inserted earlier, own holds what was inserted later, and later
  // still wins. A namespace with no base behaves precisely as before.
  //
  // LIFETIME. `base` must outlive this namespace. Every current use points at
  // a function-local `static const PyNamespace` with static storage duration,
  // which outlives everything.
  const PyNamespace* base = nullptr;

  bool hasName(const std::string& n) const {
    if (consts.count(n) != 0 || fns.count(n) != 0) return true;
    return base != nullptr && base->hasName(n);
  }
};

// The two immutable registries, built once per process. `stataMathNamespace()`
// is mathNamespace(false)+addStataFns() — the expression registry evalArith and
// the if/while condition path share; `postfileMathNamespace()` is
// mathNamespace(true), which evalMacroExpr uses. Callers point a per-call
// PyNamespace's `base` at one of these instead of rebuilding it.
const PyNamespace& stataMathNamespace();
const PyNamespace& postfileMathNamespace();

// Evaluate `expr` in `ns`. nullopt == "Python raised" (any exception).
// Mirrors: try: eval(expr, {"__builtins__":{}}, ns) except Exception: None.
// A bare-function-object result also maps to nullopt (isinstance checks
// reject it in every consumer this API serves).
std::optional<PyValue> evalPyExpr(const std::string& expr, const PyNamespace& ns);

// Detailed outcome for the one consumer (postfile.eval_macro_expr) whose
// Python original str()s a non-numeric result: a bare builtin name
// evaluates to the function object, and str(fn) is deterministic
// ("<built-in function sqrt>").
struct PyEvalOutcome {
  enum class Status { Value, FnObject, Raised } status;
  PyValue value;       // Status::Value
  std::string fnName;  // Status::FnObject
  std::string error;   // Status::Raised — diagnostic message (e.g. "NameError: ...")
};
PyEvalOutcome evalPyExprDetailed(const std::string& expr, const PyNamespace& ns);

// name-binding-display-literals: Stata's x/0 is missing, not an error
// (tests/eval/golden/aer_w2/w2av_name_binding.log bd_dy_div0 `[.]`, bd_loc_div0,
// bd_sca_div0, bd_inl_div0 at rc 0). While a scope is alive on this thread, `/`
// by zero evaluates to the missing sentinel instead of raising
// ZeroDivisionError. The Stata expression surfaces (eval::evalArith and
// eval::evalMacroExpr, strmutate.cpp's string row evaluator, loops.cpp's
// `if`/`while` condition) open one; a caller that wants Python's semantics (the
// pyeval contract selftest, postfile) simply does not. Scopes nest.
//
// The same scope carries Stata's other "no error, just missing" rules, each of
// which a NaN would otherwise break (it prints `.` but compares false):
//   * a function result that is NaN is the missing sentinel
//     (`invttail(5,2) == .` is 1; BUG-1358);
//   * an arithmetic result that overflows to +/-inf stays one, and the operator
//     that reads it -- arithmetic, comparison, negation -- reads `.`
//     (`(1e300*1e10 == .)` is 1; BUG-1363), while a function argument gets the
//     infinity itself, as in Stata (`inlist(1e300*1e10, .)` is 0);
//   * `^` never raises: 0^-1, (-8)^(1/3) and 1e200^2 are the sentinel (BUG-1065).
class StataDivisionScope {
 public:
  StataDivisionScope();
  ~StataDivisionScope();
  StataDivisionScope(const StataDivisionScope&) = delete;
  StataDivisionScope& operator=(const StataDivisionScope&) = delete;

 private:
  bool prev_;
};

// The math-builtin namespace shared by macros._eval_arith and
// postfile.eval_macro_expr: sqrt log log2 log10 exp abs min max round floor
// ceil pi e. `withInfNan` additionally binds inf/nan (postfile._MATH_NS has
// them; macros._MATH_NS does not).
PyNamespace mathNamespace(bool withInfNan);

// --- Prebound (compile-once, evaluate-per-row) evaluation -------------------
//
// evalPyExpr() re-parses the expression string AND resolves every identifier
// through PyNamespace's std::map on EVERY call. For a row-wise evaluator that
// runs the SAME expression across millions of rows (strmutate::evalRows), both
// costs are per-row waste: the parse is invariant, and re-binding every column
// by name into a std::map per row is O(rows × cols) ordered-map writes plus a
// map lookup per identifier.
//
// PreboundExpr parses ONCE and resolves each identifier ONCE to a slot: an
// index into a small per-row value vector the caller refills, a captured
// constant, or a captured function. Per row the caller writes only the few
// referenced slots (a flat vector, no map) and evaluates the cached AST.
//
// Resolution precedence, fixed at compile time to mirror evalPyExpr's
// name-resolution contract: a name in `rowVars` (the per-row-varying set —
// data columns plus the row specials the caller lists) is a SLOT and wins over
// any same-named entry in `constNs`; otherwise `constNs.consts` (captured by
// value) then `constNs.fns` (captured by copy); otherwise unresolved →
// NameError at eval time, exactly as evalPyExpr's Name case raises.
class PreboundExpr {
 public:
  PreboundExpr();
  ~PreboundExpr();
  PreboundExpr(PreboundExpr&&) noexcept;
  PreboundExpr& operator=(PreboundExpr&&) noexcept;
  PreboundExpr(const PreboundExpr&) = delete;
  PreboundExpr& operator=(const PreboundExpr&) = delete;

  // False if the expression did not parse. On false the caller should fall
  // back to evalPyExpr per row, which reproduces the identical failure.
  bool ok() const;
  const std::string& error() const;  // parse-error diagnostic when !ok()

  // Slot i is filled from the name rowSlotNames()[i]. Only the row-varying
  // names actually REFERENCED by the expression appear, in first-reference
  // order. Empty when the expression references no rowVar.
  const std::vector<std::string>& rowSlotNames() const;

  // Evaluate for one row. `slots[i]` is the value of rowSlotNames()[i] this
  // row; slots.size() must equal rowSlotNames().size(). nullopt == "Python
  // raised" or a bare-function result — identical to evalPyExpr's contract.
  std::optional<PyValue> eval(const std::vector<PyValue>& slots) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend PreboundExpr compilePreboundExpr(const std::string& expr, const PyNamespace& constNs,
                                          const std::set<std::string>& rowVars);
};

// Compile `expr` for repeated per-row evaluation (see PreboundExpr). `rowVars`
// = names that vary per row (columns + row specials such as "_n"/"_N"), each a
// slot that wins over constNs; `constNs` supplies the constant consts/fns,
// captured at compile time. A parse failure yields a PreboundExpr with
// ok()==false.
PreboundExpr compilePreboundExpr(const std::string& expr, const PyNamespace& constNs,
                                 const std::set<std::string>& rowVars);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_PYEVAL_H
