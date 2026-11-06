// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Generic `ml model lf`: compile an admissible stored program, never execute
// its statements while fitting. Method contract: scope-ge2/ml-getmata/SPEC.md
// 4.2-4.5; captured behavior: w2c_ml_getmata.log (the actual capture permits
// pweights with an explicitly requested vce(oim), unlike the draft spec).
#include "vendor/nlohmann/json.hpp"
#include "piet/handlers.h"
#include "handlers_e3_common.h"
#include "lapack_dsyevr.h"
#include "core/expr_eval.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/inrange_filter.h"
#include "piet/program.h"
#include "piet/strutil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace piet::eval {
// Defined in eval/exprfns.cpp (runiform() bound to the session `set seed` stream).
void addSessionRngFns(PyNamespace& ns, EvalSession& session);
}  // namespace piet::eval

namespace piet::handlers {

// Shared fit-sample filter, also used by regress; preserves original row ids.
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
    const std::optional<std::string>&, const std::optional<std::string>&, std::string*);

namespace {
using ojson = nlohmann::ordered_json;
using Vec = std::vector<double>;
using Mat = std::vector<Vec>;
struct MlError : std::runtime_error {
  int rc;
  MlError(std::string s, int r = 198) : std::runtime_error(std::move(s)), rc(r) {}
};
std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::vector<std::string> words(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> v;
  for (std::string x; in >> x;) v.push_back(x);
  return v;
}
bool ident(const std::string& s) {
  static const std::regex re("[A-Za-z_][A-Za-z_0-9]*");
  return std::regex_match(s, re);
}
double number(const std::string& s) {
  try {
    size_t pos = 0;
    double x = std::stod(s, &pos);
    if (pos == s.size() && std::isfinite(x) && s.find_first_of("xX") == std::string::npos)
      return x;
  } catch (const std::exception&) {}
  throw MlError("ml: invalid numeric value " + s);
}
std::string replaceName(const std::string& text, const std::string& name,
                        const std::string& value) {
  return std::regex_replace(text, std::regex("\\b" + name + "\\b"), "(" + value + ")");
}
std::string expanded(const std::string& text, eval::EvalSession& ev) {
  auto x = eval::expandDefinitionText(text, ev);
  if (x.isError) throw MlError("ml model lf: cannot expand evaluator: " +
                              x.error.value("message", std::string()));
  return trim(x.line);
}
std::string compileBody(const ProgramDef& def, size_t nEq,
                        const std::vector<std::string>& depvars, SessionState& session,
                        std::vector<std::string>* conditions) {
  eval::EvalSession env;
  env.globals = session.eval.globals;
  for (size_t i = 0; i < depvars.size(); ++i)
    env.globals.set("ML_y" + std::to_string(i + 1), depvars[i]);
  std::set<std::string> temps;
  std::map<std::string, std::string> gens;
  std::string expression;
  bool args = false;
  int braces = 0;
  for (auto line : def.body) {
    line = trim(line);
    if (line.empty()) continue;
    if (line == "}") { if (--braces < 0) throw MlError("ml evaluator: unmatched brace"); continue; }
    static const std::regex quiet(R"(^qui(?:etly)?\s*)", std::regex::icase);
    // Only quiet blocks/prefixes are admissible; no dispatch or side effects.
    std::smatch m;
    while (std::regex_search(line, m, quiet) && m.position() == 0) line = trim(line.substr(m.length()));
    if (line == "{") { ++braces; continue; }
    if (line.rfind("args ", 0) == 0) {
      auto a = words(line.substr(5));
      if (args || a.size() != nEq + 1) throw MlError("ml evaluator: args must name lnf and every equation");
      args = true;
      for (size_t j = 0; j < a.size(); ++j) {
        if (!ident(a[j])) throw MlError("ml evaluator: invalid args name");
        env.locals.set(a[j], j ? "__ml_eq" + std::to_string(j - 1) : "__ml_lnf");
      }
      continue;
    }
    if (!args) throw MlError("ml evaluator: args must precede the likelihood expression");
    if (line.rfind("tempvar ", 0) == 0) {
      for (const auto& name : words(line.substr(8))) {
        if (!ident(name)) throw MlError("ml evaluator: invalid tempvar");
        std::string var = "__ml_tv" + std::to_string(temps.size());
        env.locals.set(name, var);
        temps.insert(var);
      }
      continue;
    }
    if (std::regex_match(line, m, std::regex(R"(^local\s+([A-Za-z_]\w*)\s+(.*)$)"))) {
      std::string rhs = trim(m[2]);
      if (!rhs.empty() && rhs.front() == '=') rhs = trim(rhs.substr(1));
      rhs = expanded(rhs, env);
      if (rhs.size() < 2 || rhs.front() != '"' || rhs.back() != '"')
        throw MlError("ml evaluator: only text-valued local definitions are supported");
      env.locals.set(m[1], rhs.substr(1, rhs.size() - 2));
      continue;
    }
    line = expanded(line, env);
    if (!std::regex_match(line, m,
          std::regex(R"(^(gen(?:erate)?|replace)\s+(?:(double|float)\s+)?([A-Za-z_]\w*)\s*=\s*(.+)$)")))
      throw MlError("ml model lf: evaluator " + def.name + " contains an unsupported statement: " + line);
    const std::string target = m[3], store = m[2];
    std::string rhs = trim(m[4]);
    const auto assignment = eval::tokenize("replace " + target + " = " + rhs);
    if (assignment.inRange || assignment.weightVar || !assignment.opts.empty())
      throw MlError("ml evaluator: unsupported assignment qualifier");
    rhs = trim(assignment.argText.substr(assignment.argText.find('=')+1));
    for (const auto& g : gens) rhs = replaceName(rhs,g.first,g.second);
    if (m[1] == "replace") {
      if (target != "__ml_lnf") throw MlError("ml model lf: replace must target lnf");
      if (assignment.ifExpr) {
        std::string condition = *assignment.ifExpr;
        for (const auto& g : gens) condition = replaceName(condition,g.first,g.second);
        if (condition.find("__ml_eq") != std::string::npos || condition.find("__ml_lnf") != std::string::npos ||
            condition.find("__ml_tv") != std::string::npos)
          throw MlError("ml evaluator: if-conditions must depend only on data");
        if (std::regex_search(condition,std::regex(R"(\b(_n|_N|runiform|runiformint|rnormal|rgamma|rbinomial|sum)\b)")))
          throw MlError("ml evaluator: stateful or row-order if-conditions are not supported");
        const std::string guard = "__ml_cond"+std::to_string(conditions->size());
        conditions->push_back(condition);
        expression = "cond("+guard+",("+rhs+"),("+
            (expression.empty()?"sqrt(-1)":expression)+"))";
      } else expression = rhs;
    } else {
      if (assignment.ifExpr) throw MlError("ml evaluator: generated tempvar qualifiers are not supported");
      if (!temps.count(target) || gens.count(target))
        throw MlError("ml evaluator: generate must assign a declared tempvar exactly once");
      // Inline only earlier definitions: cycles/forward references stay unknown.
      const bool floatStore = store == "float" || (store.empty() && session.defaultStorageType == "float");
      gens[target] = floatStore ? "float("+rhs+")" : rhs;
    }
  }
  if (braces || expression.empty()) throw MlError("ml model lf: incomplete single-replace evaluator");
  cecon::expr::Options opts;
  opts.tag = "ml model lf";
  opts.allow_params = false;
  opts.allow_coefs = false;
  // Parsing here rejects if/in qualifiers, subscripts and stateful functions.
  auto check = cecon::expr::Program::compile(expression, opts);
  for (const auto& v : check.variables()) {
    if (v.rfind("__ml_eq", 0) == 0 || v.rfind("__ml_cond",0) == 0) {
      if (session.datastore()->has(v)) throw MlError("ml evaluator: reserved internal name conflicts with data: "+v);
      continue;
    }
    if (v == "_n" || v == "_N" || temps.count(v) || !session.datastore()->has(v))
      throw MlError("ml evaluator: unknown or unsupported expression variable " + v);
    if (session.datastore()->col(v).dtype != cecon::DType::F64)
      throw MlError("ml evaluator: nonnumeric expression variable " + v, 109);
  }
  return expression;
}

// A small SPD inverse also validates concavity. Native PIET does not link the
// engine's LAPACK library; all matrices below are row-major and bounded to 64.
bool inverse(const Mat& A, Mat& out) {
  const size_t k = A.size();
  Mat L(k, Vec(k));
  for (size_t i = 0; i < k; ++i) for (size_t j = 0; j <= i; ++j) {
    long double s = A[i][j];
    for (size_t p = 0; p < j; ++p) s -= static_cast<long double>(L[i][p]) * L[j][p];
    if (i == j) { if (!(s > 0)) return false; L[i][j] = std::sqrt(s); }
    else L[i][j] = static_cast<double>(s) / L[j][j];
  }
  out.assign(k, Vec(k));
  for (size_t c = 0; c < k; ++c) {
    Vec y(k);
    for (size_t i = 0; i < k; ++i) {
      long double z = i == c ? 1 : 0;
      for (size_t j = 0; j < i; ++j) z -= L[i][j] * y[j];
      y[i] = z / L[i][i];
    }
    for (size_t i = k; i-- > 0;) {
      long double z = y[i];
      for (size_t j = i + 1; j < k; ++j) z -= L[j][i] * out[j][c];
      out[i][c] = z / L[i][i];
    }
  }
  return true;
}
Vec multiply(const Mat& A, const Vec& b) {
  Vec x(b.size());
  for (size_t i = 0; i < b.size(); ++i)
    for (size_t j = 0; j < b.size(); ++j) x[i] += A[i][j] * b[j];
  return x;
}
double dot(const Vec& a, const Vec& b) {
  long double s = 0;
  for (size_t i = 0; i < a.size(); ++i) s += static_cast<long double>(a[i]) * b[i];
  return s;
}
struct Data {
  cecon::expr::Program program;
  Mat vars;                         // row x expression variables
  std::vector<int> varEq;            // expression variable -> equation, -1=data
  Mat X;                            // row x coefficient
  std::vector<size_t> eq;            // coefficient -> equation
  std::vector<std::string> names;     // full Stata equation:column stripe
  std::vector<size_t> sourceRows;
  Vec weight;
  bool weighted = false;
  size_t allRows = 0;
  size_t nEq = 0;
  std::vector<char> eqCons;          // equation -> has a trailing _cons column

  Vec values(const Vec& b) const {
    Vec out(X.size());
    Vec v(program.variables().size());
    const size_t nEq = eq.empty() ? 0 : *std::max_element(eq.begin(), eq.end()) + 1;
    Vec xb(nEq);
    for (size_t r = 0; r < X.size(); ++r) {
      std::fill(xb.begin(), xb.end(), 0);
      for (size_t j = 0; j < b.size(); ++j) xb[eq[j]] += X[r][j] * b[j];
      v = vars[r];
      for (size_t j = 0; j < v.size(); ++j) if (varEq[j] >= 0) v[j] = xb[varEq[j]];
      out[r] = program.eval(nullptr, cecon::expr::VarSource{v.data(), nullptr}, nullptr);
    }
    return out;
  }
  double ll(const Vec& b) const {
    auto f = values(b);
    long double sum = 0;
    for (size_t r = 0; r < f.size(); ++r) {
      if (!std::isfinite(f[r])) return -std::numeric_limits<double>::infinity();
      sum += static_cast<long double>(weight[r]) * f[r];
    }
    return sum;
  }
};

std::vector<std::pair<std::string, std::string>> atoms(const std::string& term) {
  if (term.find("##") != std::string::npos)
    throw MlError("ml: ## factorial expansion is not implemented; specify its main effects and # terms");
  std::vector<std::pair<std::string, std::string>> out;
  std::istringstream s(term);
  for (std::string a; std::getline(s, a, '#');) {
    std::string kind = "c", name = a;
    if (a.rfind("i.", 0) == 0 || a.rfind("c.", 0) == 0) { kind = a.substr(0,1); name = a.substr(2); }
    if (!ident(name)) throw MlError("ml: unsupported equation term " + term);
    out.emplace_back(kind, name);
  }
  return out;
}

Data prepare(const ojson& state, SessionState& session) {
  if (!session.hasData()) throw MlError("ml: no observations", 2000);
  Datastore* head = session.datastore();
  const size_t nAll = static_cast<size_t>(head->nRows());
  std::string index;
  const std::optional<std::string> ifExpr = state["if"].is_null() ? std::nullopt :
      std::optional<std::string>(state["if"].get<std::string>());
  const std::optional<std::string> inRange = state["in"].is_null() ? std::nullopt :
      std::optional<std::string>(state["in"].get<std::string>());
  auto filter = applyIfAndInFilterKeepingIndex(session, *session.currentHbin(), ifExpr, inRange, &index);
  if (!filter.ok) throw MlError("ml: " + filter.refusal);
  Datastore ds(filter.dataPath);
  cecon::expr::Options opt;
  opt.tag = "ml lf"; opt.allow_params = false; opt.allow_coefs = false;
  Data d{cecon::expr::Program::compile(state["expr"].get<std::string>(), opt)};
  d.allRows = nAll;
  const size_t n = ds.nRows();
  std::set<std::string> needed;
  for (const auto& v : d.program.variables())
    if (v.rfind("__ml_eq", 0) != 0 && v.rfind("__ml_cond",0) != 0) needed.insert(v);
  for (const auto& e : state["equations"]) {
    for (const auto& y : e["depvars"]) needed.insert(y.get<std::string>());
    for (const auto& t : e["terms"])
      for (const auto& a : atoms(t.get<std::string>())) needed.insert(a.second);
  }
  const std::string w = state.value("weight", std::string());
  if (!w.empty()) needed.insert(w);
  std::map<std::string, const double*> cols;
  for (const auto& v : needed) {
    if (!ds.has(v)) throw MlError("ml: variable " + v + " not found", 111);
    if (ds.col(v).dtype != cecon::DType::F64) throw MlError("ml: numeric variable required: " + v, 109);
    cols[v] = ds.col(v).f64();
  }
  std::vector<size_t> rows;
  for (size_t r = 0; r < n; ++r) {
    bool good = true;
    for (const auto& c : cols) if (!std::isfinite(c.second[r])) { good = false; break; }
    if (!w.empty() && cols[w][r] < 0) throw MlError("ml: weights must be nonnegative", 402);
    if (good && (w.empty() || cols[w][r] > 0)) rows.push_back(r);
  }
  if (rows.empty()) throw MlError("ml: no observations", 2000);
  d.X.assign(rows.size(), Vec());
  size_t eidx = 0;
  for (const auto& e : state["equations"]) {
    const std::string eqname = e["name"];
    auto append = [&](const std::string& name, const Vec& v) {
      d.names.push_back(eqname + ":" + name); d.eq.push_back(eidx);
      for (size_t r = 0; r < rows.size(); ++r) d.X[r].push_back(v[r]);
    };
    for (const auto& term : e["terms"]) {
      const auto aa = atoms(term.get<std::string>());
      std::vector<std::pair<std::string, Vec>> products{{"", Vec(rows.size(), 1)}};
      for (const auto& a : aa) {
        std::vector<std::pair<std::string, Vec>> next;
        std::set<double> levels;
        if (a.first == "i") for (size_t r : rows) levels.insert(cols[a.second][r]);
        else levels.insert(0);
        // Stata retains the sole factor level in a no-constant interaction.
        const bool dropBase = a.first == "i" && levels.size() > 1 &&
                              (!e["noconstant"].get<bool>() || aa.size() == 1);
        for (const auto& p : products) for (double lev : levels) {
          if (dropBase && lev == *levels.begin()) continue;
          std::ostringstream label;
          if (!p.first.empty()) label << p.first << '#';
          if (a.first == "i") label << lev << '.' << a.second;
          else label << (aa.size() > 1 ? "c." : "") << a.second;
          Vec v = p.second;
          for (size_t r = 0; r < rows.size(); ++r)
            v[r] *= a.first == "i" ? static_cast<double>(cols[a.second][rows[r]] == lev) : cols[a.second][rows[r]];
          next.emplace_back(label.str(), std::move(v));
        }
        products = std::move(next);
      }
      for (const auto& p : products) append(p.first, p.second);
    }
    if (!e["noconstant"].get<bool>()) append("_cons", Vec(rows.size(), 1));
    d.eqCons.push_back(!e["noconstant"].get<bool>());
    ++eidx;
  }
  d.nEq = eidx;
  d.weighted = !w.empty();
  if (d.names.empty() || d.names.size() > 64)
    throw MlError("ml: this version supports 1 through 64 likelihood parameters");
  // Refuse nonidentified equation designs rather than publish invented SEs.
  for (size_t e = 0; e < eidx; ++e) {
    std::vector<size_t> js;
    for (size_t j = 0; j < d.eq.size(); ++j) if (d.eq[j] == e) js.push_back(j);
    if (js.empty()) throw MlError("ml: an equation must contain at least one parameter");
    Mat cross(js.size(), Vec(js.size()));
    for (size_t j = 0; j < js.size(); ++j) for (size_t k = 0; k < js.size(); ++k)
      for (size_t r = 0; r < rows.size(); ++r) cross[j][k] += d.X[r][js[j]] * d.X[r][js[k]];
    Mat inv;
    if (!inverse(cross, inv)) throw MlError("ml: collinear equation design is not implemented");
  }
  for (const auto& v : d.program.variables()) {
    int e = -1;
    for (size_t j = 0; j < eidx; ++j) if (v == "__ml_eq" + std::to_string(j)) e = j;
    d.varEq.push_back(e);
  }
  const double* rowIndex = index.empty() ? nullptr : ds.col(index).f64();
  std::map<std::string, std::vector<char>> guards;
  if (state.contains("conditions")) for (size_t c = 0; c < state["conditions"].size(); ++c) {
    const std::string condition = state["conditions"][c].get<std::string>();
    const std::string combined = ifExpr ? "("+*ifExpr+") & ("+condition+")" : condition;
    std::string conditionIndex;
    auto selected = applyIfAndInFilterKeepingIndex(session,*session.currentHbin(),combined,inRange,&conditionIndex);
    if (!selected.ok) throw MlError("ml evaluator condition: "+selected.refusal);
    Datastore chosen(selected.dataPath);
    std::vector<char> mask(nAll,0);
    if (conditionIndex.empty()) std::fill(mask.begin(),mask.end(),1);
    else {
      const double* ids = chosen.col(conditionIndex).f64();
      for (int64_t r = 0; r < chosen.nRows(); ++r) {
        const size_t i = static_cast<size_t>(ids[r]-1);
        if (i < mask.size()) mask[i] = 1;
      }
    }
    guards["__ml_cond"+std::to_string(c)] = std::move(mask);
  }
  for (size_t r : rows) {
    Vec v;
    const size_t original = rowIndex ? static_cast<size_t>(rowIndex[r]-1) : r;
    for (size_t j = 0; j < d.varEq.size(); ++j) {
      const auto& name = d.program.variables()[j];
      auto g = guards.find(name);
      v.push_back(d.varEq[j] >= 0 ? 0 : g != guards.end() ? g->second.at(original) : cols.at(name)[r]);
    }
    d.vars.push_back(std::move(v));
    d.weight.push_back(w.empty() ? 1 : cols[w][r]);
    d.sourceRows.push_back(rowIndex ? static_cast<size_t>(rowIndex[r] - 1) : r);
  }
  return d;
}

// ---------------------------------------------------------------------------
// Fitting an `ml model lf` problem: the initial-value search and the
// maximization.
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
// Compensated (double-double) accumulation, for sums Stata keeps in quad
// precision.
struct Accum {
  double s = 0, c = 0;
  void add(double x) {
    const double t = s + x;
    c += std::fabs(s) >= std::fabs(x) ? (s - t) + x : (x - t) + s;
    s = t;
  }
  double get() const { return s + c; }
};
// Mata invsym() on a symmetric matrix (upper triangle), with the drop rule
// measured on StataBE (estimation_glm.cpp glmVceRank, BUGLOG BUG-939): sweep the
// largest remaining pivot first; a column whose pivot is <= 1e-9 x its
// original diagonal, below 1e-19, or whose original diagonal is not positive
// is omitted (row and column 0). probe6 (impl_A/stata_probe) checks the
// omitted columns on indefinite, singular and negative inputs.
Mat invsymStata(const Mat& in) {
  const size_t k = in.size();
  Mat A(k, Vec(k));
  for (size_t i = 0; i < k; ++i) for (size_t j = i; j < k; ++j) A[i][j] = A[j][i] = in[i][j];
  for (size_t i = 0; i < k; ++i) for (size_t j = 0; j < k; ++j)
    if (!std::isfinite(A[i][j])) return Mat(k, Vec(k, kMissing));
  Vec d0(k);
  std::vector<char> left(k), swept(k, 0);
  for (size_t j = 0; j < k; ++j) { d0[j] = A[j][j]; left[j] = d0[j] > 0; }
  for (;;) {
    size_t best = k;
    double pivot = 0;
    for (size_t j = 0; j < k; ++j) {
      if (!left[j]) continue;
      const double p = A[j][j];
      if (p <= 1e-9 * d0[j] || p < 1e-19) continue;
      if (best == k || p > pivot) { best = j; pivot = p; }
    }
    if (best == k) break;
    left[best] = 0; swept[best] = 1;
    // Stata's sweep arithmetic, the same as Mata invsym() in mata_subset.cpp
    // (StataBE 19.5 bits, 51/51 cells, WS-A round 2): symmetric storage; for
    // i <= j, a_ij -= (a_ip/D)*a_jp; a_ip = a_ip/D; a_pp = -1/D; negated at
    // the end. Unfused (this TU is built with -ffp-contract=off).
    const size_t p = best;
    const double D = A[p][p];
    for (size_t i = 0; i < k; ++i) {
      if (i == p) continue;
      const double t = A[i][p] / D;
      for (size_t j = i; j < k; ++j) {
        if (j == p) continue;
        const double u = t * A[j][p];
        A[i][j] = A[j][i] = A[i][j] - u;
      }
    }
    for (size_t i = 0; i < k; ++i)
      if (i != p) A[i][p] = A[p][i] = A[i][p] / D;
    A[p][p] = -1.0 / D;
  }
  for (auto& r : A) for (double& x : r) x = -x;
  for (size_t i = 0; i < k; ++i)
    if (!swept[i]) for (size_t j = 0; j < k; ++j) A[i][j] = A[j][i] = 0;
  return A;
}
Mat negate(Mat A) { for (auto& r : A) for (double& x : r) x = -x; return A; }

// ---------------------------------------------------------------------------
// Small numeric helpers.  Everything in this part of the file is plain double
// arithmetic in index order unless a comment says otherwise; the TU is built
// without contraction, so `a*b + c` is two roundings (needed to agree with
// StataBE's moptimize results to the last bit).
bool isMissing(double x) { return !std::isfinite(x); }
bool anyMissing(const Vec& v) {
  return std::any_of(v.begin(), v.end(), [](double x) { return isMissing(x); });
}
// [M-5] reldif(): |x - y| / (|y| + 1); the vector form takes the largest.
double relChange(double now, double before) {
  return std::fabs(now - before) / (std::fabs(before) + 1);
}
double largestRelChange(const Vec& now, const Vec& before) {
  double worst = 0;
  for (size_t i = 0; i < now.size(); ++i) worst = std::max(worst, relChange(now[i], before[i]));
  return worst;
}
double sumOfProducts(const Vec& a, const Vec& b) {
  double s = 0;
  for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}
// Row vector times matrix: out_j = sum_i v_i M_ij.
Vec rowVecTimes(const Vec& v, const Mat& M) {
  const size_t m = M.empty() ? 0 : M[0].size();
  Vec out(m, 0.0);
  for (size_t j = 0; j < m; ++j) {
    double s = 0;
    for (size_t i = 0; i < v.size(); ++i) s += v[i] * M[i][j];
    out[j] = s;
  }
  return out;
}
double diagonalSum(const Mat& M) {
  double s = 0;
  for (size_t i = 0; i < M.size(); ++i) s += M[i][i];
  return s;
}
// A Hessian H with V = invsym(-H) is treated as concave unless invsym had to
// drop a column whose own diagonal entry in H is non-zero.
bool passesConcavity(const Mat& H, const Mat& V) {
  for (size_t i = 0; i < H.size(); ++i)
    if (V[i][i] == 0 && H[i][i] != 0) return false;
  return true;
}
MlError notNegativeSemidefinite() { return MlError("Hessian is not negative semidefinite", 430); }
MlError missingRegion() {
  return MlError("could not calculate numerical derivatives -- discontinuous region with missing values encountered", 430);
}
MlError flatRegion() {
  return MlError("could not calculate numerical derivatives -- flat or discontinuous region encountered", 430);
}

// Eigen-decomposition of a symmetric matrix the way Mata's symeigensystem()
// reports it: values largest first, vectors as matching columns, signs as
// LAPACK's DSYEVR leaves them; any non-finite entry gives all-missing output.
void eigenLargestFirst(const Mat& A, Vec& values, Mat& vectors) {
  const size_t n = A.size();
  bool finite = true;
  for (const auto& row : A) if (anyMissing(row)) finite = false;
  if (!finite) { values.assign(n, kMissing); vectors.assign(n, Vec(n, kMissing)); return; }
  std::vector<double> packed(n * n), w, z;
  for (size_t j = 0; j < n; ++j) for (size_t i = 0; i < n; ++i) packed[i + j * n] = A[i][j];
  if (lapack::dsyevrVAL(static_cast<int>(n), packed, 0.0, w, z) != 0)
    throw MlError("ml: could not decompose the Hessian", 430);
  values.assign(n, 0.0);
  vectors.assign(n, Vec(n, 0.0));
  for (size_t c = 0; c < n; ++c) {
    const size_t src = n - 1 - c;
    values[c] = w[src];
    for (size_t r = 0; r < n; ++r) vectors[r][c] = z[r + src * n];
  }
}

// ---------------------------------------------------------------------------
// The lf log likelihood.  lnL(b) = sum_r w_r l_r, where l_r is the evaluator's
// expression at the observation's linear indices theta_re = x_re b_e.  For lf
// models every numerical derivative is taken with respect to whole index
// columns ([M-5] moptimize(), "type lf" evaluators; the linear-form restriction).
struct IndexShift { int eq = -1; double by = 0; };

class LfObjective {
 public:
  explicit LfObjective(const Data& d) : d_(d), slots_(d.program.variables().size()) {
    blocks_.assign(d.nEq, {});
    for (size_t j = 0; j < d.eq.size(); ++j) blocks_[d.eq[j]].push_back(j);
  }
  const Data& data() const { return d_; }
  size_t nObs() const { return d_.X.size(); }
  size_t nEq() const { return d_.nEq; }
  size_t nParams() const { return d_.names.size(); }
  // Coefficient positions of one equation (contiguous, `_cons` last if any).
  const std::vector<size_t>& block(size_t e) const { return blocks_[e]; }

  // theta[r][e], each accumulated from 0 over the equation's columns in order.
  Mat indices(const Vec& b) const {
    Mat theta(nObs(), Vec(nEq(), 0.0));
    for (size_t r = 0; r < nObs(); ++r)
      for (size_t j = 0; j < b.size(); ++j) theta[r][d_.eq[j]] += d_.X[r][j] * b[j];
    return theta;
  }
  // Per-observation l_r at theta, optionally with one or two index columns
  // moved by a constant (one double addition each).
  Vec perObs(const Mat& theta, IndexShift first = {}, IndexShift second = {}) const {
    Vec out(nObs());
    Vec at;
    for (size_t r = 0; r < nObs(); ++r) {
      at = theta[r];
      if (first.eq >= 0) at[first.eq] = at[first.eq] + first.by;
      if (second.eq >= 0) at[second.eq] = at[second.eq] + second.by;
      slots_ = d_.vars[r];
      for (size_t v = 0; v < slots_.size(); ++v)
        if (d_.varEq[v] >= 0) slots_[v] = at[d_.varEq[v]];
      out[r] = d_.program.eval(nullptr, cecon::expr::VarSource{slots_.data(), nullptr}, nullptr);
    }
    return out;
  }
  // Weighted total, kept in double-double like Mata's quad sums; one missing
  // contribution makes the point infeasible ([R] ml).
  double total(const Vec& contributions) const {
    Accum acc;
    for (size_t r = 0; r < contributions.size(); ++r) {
      if (isMissing(contributions[r])) return kMissing;
      acc.add(d_.weight[r] * contributions[r]);
    }
    return acc.get();
  }
  double at(const Vec& b) const { return total(perObs(indices(b))); }

 private:
  const Data& d_;
  std::vector<std::vector<size_t>> blocks_;
  mutable Vec slots_;
};

// ---------------------------------------------------------------------------
// Choosing the perturbation of one index column.  The goal (deriv()'s) is a
// change in lnL between v0 = (|f|+1e-8)1e-8 and v1 = (|f|+1e-7)1e-7; the
// multiplier on the base step is searched by interpolation, first with the
// minus side alone, then with both sides.  The applied shift is the product
// base*multiplier rounded through single precision.
class ColumnStep {
 public:
  ColumnStep(const LfObjective& obj, const Mat& theta, size_t eq, double base, double f0)
      : obj_(obj), theta_(theta), eq_(static_cast<int>(eq)), base_(base), f0_(f0),
        low_(tolBand(1e-8)), high_(tolBand(1e-7)), aim_((low_ + high_) / 2) {}

  // Returns the multiplier settled on; shift() and the row accessors then
  // describe it.  The minus side alone usually decides.  Otherwise, or when a
  // side turns out infeasible, both sides are searched, first shrinking the
  // multiplier if a side was infeasible.
  double settle(double start) {
    Reading r{start, kMissing};
    const Outcome minusOnly = searchMinusSide(r);
    bool bothFeasible = minusOnly != Outcome::Infeasible;
    if (bothFeasible) {
      plus_ = probe(r.mult, +1);
      bothFeasible = plus_.feasible();
    }
    if (bothFeasible && minusOnly == Outcome::InGoal) return chosen_ = r.mult;
    return searchBothSides(bothFeasible ? r.mult : shrinkUntilFeasible(r.mult));
  }
  double shift() const { return shiftFor(chosen_); }
  const Vec& plusRows() const { return plus_.rows; }
  const Vec& minusRows() const { return minus_.rows; }

 private:
  struct Side {
    double value = kMissing;
    Vec rows;
    bool feasible() const { return !isMissing(value); }
  };
  // One measurement: a multiplier and the change in lnL it produced.
  struct Reading { double mult, gap; };
  enum class Outcome { InGoal, Missed, Infeasible };
  // The three latest readings, newest first; unused slots are 0.
  struct Trail {
    double mult[3] = {0, 0, 0}, gap[3] = {0, 0, 0};
    void record(const Reading& r) {
      mult[2] = mult[1]; gap[2] = gap[1];
      mult[1] = mult[0]; gap[1] = gap[0];
      mult[0] = r.mult; gap[0] = r.gap;
    }
  };
  // The most useful reading so far: much larger change, or a comparable one
  // from a smaller multiplier.
  struct Keeper {
    Reading kept;
    void offer(const Reading& r) {
      if (r.gap > 1.1 * kept.gap || (r.gap >= 0.9 * kept.gap && r.mult < kept.mult)) kept = r;
    }
  };

  double tolBand(double eps) const { return (std::fabs(f0_) + eps) * eps; }
  bool inGoal(double gap) const { return gap >= low_ && gap <= high_; }
  double shiftFor(double m) const { return static_cast<double>(static_cast<float>(base_ * m)); }
  Side probe(double m, double sign) const {
    Side s;
    s.rows = obj_.perObs(theta_, {eq_, sign * shiftFor(m)});
    s.value = obj_.total(s.rows);
    return s;
  }
  // Next multiplier: double when the change is too small to interpolate on,
  // else interpolate (linear on the first move, then quadratic in the three
  // latest pairs) for the multiplier giving the mid-goal change; a proposal
  // that is unusable or heads the wrong way becomes a plain doubling/halving.
  double propose(const Trail& t, bool firstMove, double flat) const {
    const double m = t.mult[0], gap = t.gap[0];
    double next;
    if (gap < flat) {
      next = 2 * m;
    } else if (firstMove) {
      next = t.mult[0] + ((t.mult[1] - t.mult[0]) * (aim_ - t.gap[0])) / (t.gap[1] - t.gap[0]);
    } else {
      auto term = [&](int k, int a, int b) {
        return ((t.mult[k] * (aim_ - t.gap[a])) * (aim_ - t.gap[b])) /
               ((t.gap[k] - t.gap[a]) * (t.gap[k] - t.gap[b]));
      };
      next = (term(0, 1, 2) + term(1, 0, 2)) + term(2, 0, 1);
    }
    const bool wrongWay = (gap > high_ && next > m) || (gap < low_ && next < m);
    if (isMissing(next) || next <= 0 || wrongWay) next = gap < low_ ? 2 * m : m / 2;
    return next;
  }
  double meanGap() const {
    return (std::fabs(f0_ - plus_.value) + std::fabs(f0_ - minus_.value)) / 2;
  }
  // Fills r.gap at r.mult, probing the minus side first and the plus side only
  // when asked and the minus side evaluated; false if a probed side is infeasible.
  bool measure(Reading& r, bool twoSided) {
    minus_ = probe(r.mult, -1);
    if (!minus_.feasible()) return false;
    if (!twoSided) {
      r.gap = std::fabs(f0_ - minus_.value);
      return true;
    }
    plus_ = probe(r.mult, +1);
    if (!plus_.feasible()) return false;
    r.gap = meanGap();
    return true;
  }
  // The one search both stages share: from the measured reading `r`, move the
  // multiplier until the change is in the goal band or `rounds` moves are spent.
  Outcome seek(Reading& r, bool twoSided, int rounds, double flat, Keeper* keeper) {
    Trail trail;
    trail.record(r);
    for (int used = 0; !inGoal(r.gap); ++used) {
      if (used == rounds) return Outcome::Missed;
      r.mult = propose(trail, used == 0, flat);
      if (!measure(r, twoSided)) return Outcome::Infeasible;
      trail.record(r);
      if (keeper) keeper->offer(r);
    }
    return Outcome::InGoal;
  }
  // First stage.  A missed search leaves r and the minus side where it began.
  Outcome searchMinusSide(Reading& r) {
    if (!measure(r, false)) return Outcome::Infeasible;
    const Reading began = r;
    const Side beganMinus = minus_;
    const Outcome how = seek(r, false, 20, tolBand(1e-10), nullptr);
    if (how == Outcome::Missed) { r = began; minus_ = beganMinus; }
    return how;
  }
  double shrinkUntilFeasible(double m) {
    for (int round = 0; round < 50; ++round) {
      Reading r{m /= 2, kMissing};
      if (measure(r, true)) return m;
    }
    throw missingRegion();
  }
  // Second stage, entered with both sides evaluated at m.  A search that fails
  // falls back on the kept reading if that one moved lnL at all.
  double searchBothSides(double m) {
    const double flat = tolBand(1e-12);
    Reading r{m, meanGap()};
    Keeper keeper{r};
    const Outcome how = seek(r, true, 40, flat, &keeper);
    if (how == Outcome::InGoal) return chosen_ = r.mult;
    if (keeper.kept.gap < flat) {
      if (how == Outcome::Infeasible) throw missingRegion();
      throw flatRegion();
    }
    minus_ = probe(keeper.kept.mult, -1);
    plus_ = probe(keeper.kept.mult, +1);
    return chosen_ = keeper.kept.mult;
  }

  const LfObjective& obj_;
  const Mat& theta_;
  int eq_;
  double base_, f0_, low_, high_, aim_;
  double chosen_ = 1;
  Side minus_, plus_;
};

// ---------------------------------------------------------------------------
// Fitting an lf model: start-value search plus modified Newton-Raphson/BHHH
// with step halving ([R] Maximize; [M-5] optimize(), moptimize()).
class LfOptimizer {
 public:
  LfOptimizer(const Data& d, std::function<double()> uniform)
      : obj_(d), uniform_(std::move(uniform)) {}

  struct Options {
    double ptol = 1e-6, vtol = 1e-7, nrtol = 1e-5;
    int maxiter = 300;
    bool difficult = false;
    std::vector<std::pair<std::string, int>> techniques{{"nr", 5}};
  };
  struct Result {
    Vec b; double ll = kMissing; int iterations = 0; bool converged = false;
    Mat H; Mat scores;
  };

  // `ml search`: returns the improved start vector, its lnL in *outValue.
  Vec search(Vec b, int repeat, double* outValue) {
    double f = obj_.at(b);
    if (isMissing(f)) {
      Found pick = levelTrials(b);
      if (!pick.ok()) pick = randomTrials(b, kMissing, 1000, true);
      if (!pick.ok()) throw MlError("could not find feasible values", 491);
      b = pick.b; f = pick.f;
    }
    if (repeat > 0 && repeat != 10) {
      const Found pick = randomTrials(b, f, repeat, false);
      b = pick.b; f = pick.f;
    }
    rescaleAll(b, f);
    if (obj_.nEq() > 1) {
      bool moved = false;
      for (size_t e = 0; e < obj_.nEq(); ++e) moved = rescaleOne(b, f, e) || moved;
      if (moved) for (size_t e = 0; e < obj_.nEq(); ++e) rescaleOne(b, f, e);
    }
    *outValue = f;
    return b;
  }

  Result maximize(Vec b, const Options& o, const std::function<bool()>& cancelled) {
    plan_ = o.techniques;
    multipliers_.assign(obj_.nEq(), 1.0);
    Point here = differentiate(b, newtonFor(0));
    if (isMissing(here.f)) throw MlError("initial values not feasible", 1400);
    Vec history{here.f};
    int k = 0;
    bool done = false;
    while (!done && k < o.maxiter) {
      if (cancelled()) throw MlError("ml: cancelled", 1);
      ++k;
      const Vec next = step(b, here, o.difficult, history);
      Point there = differentiate(next, newtonFor(static_cast<size_t>(k)));
      if (isMissing(there.f)) throw missingRegion();
      done = meetsCriteria(b, here.f, next, there, o);
      b = next;
      here = std::move(there);
      history.push_back(here.f);
    }
    // The posted V always comes from second differences.
    if (!here.newton) here = differentiate(b, true);
    Result out;
    out.b = b; out.ll = here.f; out.iterations = k; out.converged = done;
    out.H = std::move(here.H); out.scores = std::move(here.scores);
    return out;
  }

 private:
  struct Found {
    Vec b; double f = kMissing;
    bool ok() const { return !isMissing(f); }
  };
  // Value, gradient, Hessian (second differences or -OPG) and equation scores.
  struct Point {
    double f = kMissing;
    Vec g;
    Mat H, scores;
    bool newton = true;
  };

  // ----- start-value search --------------------------------------------
  // Put equation e's level at c: its _cons when it has one, otherwise the
  // unweighted least-squares coefficients of the constant c on its columns.
  void setLevel(Vec& b, size_t e, double c) const {
    const auto& cols = obj_.block(e);
    const Data& d = obj_.data();
    if (d.eqCons[e]) { b[cols.back()] = c; return; }
    const size_t m = cols.size();
    Vec xy(m, 0.0);
    Mat xx(m, Vec(m, 0.0));
    for (size_t r = 0; r < obj_.nObs(); ++r)
      for (size_t i = 0; i < m; ++i) {
        xy[i] += c * d.X[r][cols[i]];
        for (size_t j = 0; j < m; ++j) xx[i][j] += d.X[r][cols[i]] * d.X[r][cols[j]];
      }
    const Vec fit = rowVecTimes(xy, invsymStata(xx));
    for (size_t i = 0; i < m; ++i) b[cols[i]] = fit[i];
  }
  void setAllLevels(Vec& b, double c) const {
    for (size_t e = 0; e < obj_.nEq(); ++e) setLevel(b, e, c);
  }
  // Fixed levels +-0.5, 1.25, 2.5, 10: the -g try always replaces the pick,
  // the +g try only when better; a feasible +g ends the scan.
  Found levelTrials(const Vec& from) const {
    Found pick;
    Vec work = from;
    for (double g : {0.5, 1.25, 2.5, 10.0}) {
      setAllLevels(work, -g);
      const double down = obj_.at(work);
      if (!isMissing(down)) pick = {work, down};
      setAllLevels(work, g);
      const double up = obj_.at(work);
      if (isMissing(up)) continue;
      if (!pick.ok() || up > pick.f) pick = {work, up};
      break;
    }
    return pick;
  }
  // Random levels (2u+1)*range, range cycling 1, 5, 10, 25, 100, 1000; one
  // draw per equation per attempt from the session's runiform() stream.
  Found randomTrials(const Vec& from, double fromValue, int attempts, bool firstFeasible) {
    static constexpr double kRanges[] = {1, 5, 10, 25, 100, 1000};
    Vec work = from;
    Found best{from, fromValue};
    for (int a = 0; a < attempts; ++a) {
      const double range = kRanges[a % 6];
      for (size_t e = 0; e < obj_.nEq(); ++e) setLevel(work, e, (2 * uniform_() + 1) * range);
      const double f = obj_.at(work);
      if (isMissing(f)) continue;
      if (firstFeasible) return {work, f};
      if (!best.ok() || f > best.f) best = {work, f};
    }
    return firstFeasible ? Found{} : best;
  }
  static Vec times(Vec v, double c) { for (double& x : v) x *= c; return v; }
  // Whole-vector halving/doubling; a zero vector is first replaced by the
  // level trials' pick even when that is worse.
  void rescaleAll(Vec& b, double& f) const {
    if (sumOfProducts(b, b) == 0) {
      const Found alt = levelTrials(b);
      if (!alt.ok()) return;
      b = alt.b; f = alt.f;
    }
    for (double factor : {0.5, 2.0}) {
      Vec trial = times(b, factor);
      double ft = obj_.at(trial);
      if (isMissing(ft) || !(ft > f)) continue;
      do {
        b = trial; f = ft;
        trial = times(b, factor);
        ft = obj_.at(trial);
      } while (!isMissing(ft) && ft > f);
      return;
    }
  }
  void scaleBlock(Vec& b, size_t e, double c) const {
    for (size_t j : obj_.block(e)) b[j] *= c;
  }
  double meanIndex(const Vec& b, size_t e) const {
    const Mat theta = obj_.indices(b);
    double s = 0;
    for (const auto& row : theta) s += row[e];
    return s / static_cast<double>(obj_.nObs());
  }
  // One equation's block: halving (with a sign flip when its mean index is
  // ~0) or doubling.  Returns whether the equation counts as rescaled.
  bool rescaleOne(Vec& b, double& f, size_t e) const {
    auto better = [&](double ft) {
      return !isMissing(ft) && ft > f && relChange(ft, f) > 1e-12;
    };
    // Keep moving the block by `factor` while `ok` accepts the new value.
    auto walk = [&](Vec trial, double factor, const std::function<bool(double)>& ok) {
      bool any = false;
      for (double ft = obj_.at(trial); ok(ft); ft = obj_.at(trial)) {
        b = trial; f = ft; any = true;
        scaleBlock(trial, e, factor);
      }
      return any;
    };
    Vec trial = b;
    scaleBlock(trial, e, 0.5);
    if (walk(trial, 0.5, better)) {
      if (std::fabs(meanIndex(b, e)) < 1e-8) {
        trial = b;
        scaleBlock(trial, e, -4);
        walk(trial, 2, better);
      }
      return true;
    }
    trial = b;
    scaleBlock(trial, e, 2);
    return walk(trial, 2, [&](double ft) { return !isMissing(ft) && ft > f; });
  }

  // ----- derivatives ---------------------------------------------------
  bool newtonFor(size_t evaluation) const {
    size_t cycle = 0;
    for (const auto& t : plan_) cycle += static_cast<size_t>(t.second);
    size_t pos = evaluation % cycle;
    for (const auto& t : plan_) {
      if (pos < static_cast<size_t>(t.second)) return t.first == "nr";
      pos -= static_cast<size_t>(t.second);
    }
    return true;
  }
  // Base step of column e: (|weighted mean index| + 1e-4) * 1e-4.
  double baseStep(const Mat& theta, size_t e) const {
    const Data& d = obj_.data();
    Accum num, den;
    for (size_t r = 0; r < theta.size(); ++r) {
      num.add(d.weight[r] * theta[r][e]);
      den.add(d.weight[r]);
    }
    return (std::fabs(num.get() / den.get()) + 1e-4) * 1e-4;
  }

  Point differentiate(const Vec& b, bool newton) {
    const Data& d = obj_.data();
    const size_t N = obj_.nObs(), E = obj_.nEq(), K = obj_.nParams();
    Point p;
    p.newton = newton;
    const Mat theta = obj_.indices(b);
    const Vec base = obj_.perObs(theta);
    p.f = obj_.total(base);
    if (isMissing(p.f)) return p;

    Vec h(E);
    std::vector<Vec> up(E), down(E);
    for (size_t e = 0; e < E; ++e) {
      ColumnStep sel(obj_, theta, e, baseStep(theta, e), p.f);
      const double start = multipliers_[e] == 0 ? 1.0 : multipliers_[e];
      multipliers_[e] = sel.settle(start);
      h[e] = sel.shift();
      up[e] = sel.plusRows();
      down[e] = sel.minusRows();
    }
    p.scores.assign(N, Vec(E));
    for (size_t r = 0; r < N; ++r)
      for (size_t e = 0; e < E; ++e) p.scores[r][e] = (up[e][r] - down[e][r]) / (2 * h[e]);
    p.g.assign(K, 0.0);
    for (size_t c = 0; c < K; ++c) {
      const size_t e = d.eq[c];
      double s = 0;
      for (size_t r = 0; r < N; ++r) s += (d.weight[r] * p.scores[r][e]) * d.X[r][c];
      p.g[c] = s;
    }
    if (anyMissing(p.g)) throw MlError("ml: numerical derivatives are not feasible", 430);
    p.H = newton ? secondDifferences(theta, base, h, up, down) : opgHessian(p.scores);
    return p;
  }

  // Sum over observations of (u_r x_ra) x_rb, plain and in order.
  double weightedCross(const Vec& u, size_t a, size_t b) const {
    const Data& d = obj_.data();
    double s = 0;
    for (size_t r = 0; r < u.size(); ++r) s += (u[r] * d.X[r][a]) * d.X[r][b];
    return s;
  }
  Mat secondDifferences(const Mat& theta, const Vec& base, const Vec& h,
                        const std::vector<Vec>& up, const std::vector<Vec>& down) const {
    const Data& d = obj_.data();
    const size_t N = obj_.nObs(), E = obj_.nEq(), K = obj_.nParams();
    Mat H(K, Vec(K, 0.0));
    Vec u(N);
    for (size_t e = 0; e < E; ++e) {
      const auto& own = obj_.block(e);
      for (size_t r = 0; r < N; ++r)
        u[r] = d.weight[r] * (((up[e][r] - 2 * base[r]) + down[e][r]) / (h[e] * h[e]));
      for (size_t i = 0; i < own.size(); ++i)
        for (size_t j = 0; j <= i; ++j)
          H[own[i]][own[j]] = H[own[j]][own[i]] = weightedCross(u, own[j], own[i]);
      // Mixed differences against every earlier equation, reusing the
      // one-sided values: (l++ + l--) + ((((2 l0 - l+e) - l-e) - l+f) - l-f).
      for (size_t f = 0; f < e; ++f) {
        const Vec both = obj_.perObs(theta, {static_cast<int>(e), h[e]}, {static_cast<int>(f), h[f]});
        const Vec neither = obj_.perObs(theta, {static_cast<int>(e), -h[e]}, {static_cast<int>(f), -h[f]});
        const double denom = (2 * h[e]) * h[f];
        for (size_t r = 0; r < N; ++r) {
          const double rest = (((2 * base[r] - up[e][r]) - down[e][r]) - up[f][r]) - down[f][r];
          u[r] = d.weight[r] * (((both[r] + neither[r]) + rest) / denom);
        }
        for (size_t a : own) for (size_t c : obj_.block(f))
          H[a][c] = H[c][a] = weightedCross(u, a, c);
      }
    }
    return H;
  }
  // BHHH: minus the weighted outer product of the observation gradients.
  Mat opgHessian(const Mat& scores) const {
    const Data& d = obj_.data();
    const size_t K = obj_.nParams();
    Mat H(K, Vec(K, 0.0));
    for (size_t a = 0; a < K; ++a)
      for (size_t c = 0; c <= a; ++c) {
        Accum acc;
        for (size_t r = 0; r < scores.size(); ++r) {
          const double ga = scores[r][d.eq[a]] * d.X[r][a], gc = scores[r][d.eq[c]] * d.X[r][c];
          acc.add(((d.weight[r] * d.weight[r]) * ga) * gc);
        }
        H[a][c] = H[c][a] = -acc.get();
      }
    return H;
  }

  // ----- steps ---------------------------------------------------------
  struct Move {
    Vec b;
    double value;  // b's value, except after spent halvings: the last value tried
  };
  // Try from + dir.  A worse or infeasible trial is pulled halfway back to
  // `from`; spending all `halvings` means staying at `from`, even when the
  // final one happened to succeed.  An acceptable first trial is pushed
  // further along dir by t = forward, 2*forward, ... while the value rises.
  Move lineSearch(const Vec& from, double f0, const Vec& dir, double forward, int halvings) const {
    Vec trial(from.size());
    for (size_t j = 0; j < trial.size(); ++j) trial[j] = from[j] + dir[j];
    double ft = obj_.at(trial);
    if (isMissing(ft) || ft < f0) {
      for (int n = 1; n <= halvings; ++n) {
        for (size_t j = 0; j < trial.size(); ++j) trial[j] = (trial[j] + from[j]) / 2;
        ft = obj_.at(trial);
        if (n < halvings && !isMissing(ft) && ft >= f0) return {trial, ft};
      }
      return {from, ft};
    }
    for (double t = forward;; t *= 2) {
      Vec further(trial.size());
      for (size_t j = 0; j < trial.size(); ++j) further[j] = trial[j] + t * dir[j];
      const double ff = obj_.at(further);
      if (isMissing(ff) || !(ff > ft)) break;
      trial = std::move(further);
      ft = ff;
    }
    return {trial, ft};
  }
  // Step-size factor for a fall-back direction: aim for a gain of
  // eps*|f0|, eps = 1e-2 shrunk to the recent relative progress (>= 1e-8).
  static double stepFactor(double f0, const Vec& g, const Vec& dir, const Vec& history) {
    double eps = 1e-2;
    const size_t n = history.size();
    if (n >= 3) {
      const double recent = history[n - 1], before = history[n - 2];
      eps = std::min(1e-2, std::max(1e-8, (0.1 * (recent - before)) / std::fabs(before)));
    }
    const double c = (eps * std::fabs(f0)) / sumOfProducts(g, dir);
    return isMissing(c) ? 2.0 : c;
  }

  // g'V; a missing element means V was not usable.
  static Vec usableDirection(const Vec& g, const Mat& V) {
    Vec dir = rowVecTimes(g, V);
    if (anyMissing(dir)) throw notNegativeSemidefinite();
    return dir;
  }
  static void scaleBy(Vec& v, double c) { for (double& x : v) x *= c; }

  Vec step(const Vec& b, const Point& at, bool difficult, const Vec& history) const {
    const Mat V = invsymStata(negate(at.H));
    if (passesConcavity(at.H, V)) return lineSearch(b, at.f, usableDirection(at.g, V), 0.125, 60).b;
    if (difficult) return mixedStep(b, at, history);
    // Default for a non-concave H (m-marquardt, [M-5] optimize()): a loaded
    // inverse with the original trace, sized like a gradient step.
    Mat loaded = inverseWithLoadedDiagonal(at.H);
    matchTrace(loaded, diagonalSum(V));
    Vec dir = usableDirection(at.g, loaded);
    scaleBy(dir, stepFactor(at.f, at.g, dir, history));
    return lineSearch(b, at.f, dir, 1.0, 60).b;
  }
  // invsym(-H) once H's diagonal has been lowered enough that every column
  // survives; the load doubles on each attempt.
  static Mat inverseWithLoadedDiagonal(Mat H) {
    Mat V;
    double load = 1.1;
    int attempts = 0;
    do {
      for (size_t i = 0; i < H.size(); ++i) H[i][i] = H[i][i] - load * std::fabs(H[i][i]);
      V = invsymStata(negate(H));
      load *= 2;
    } while (!passesConcavity(H, V) && ++attempts < 2000);
    return V;
  }
  static void matchTrace(Mat& V, double trace) {
    const double ratio = trace / diagonalSum(V);
    if (!(ratio > 0)) return;
    for (auto& row : V) scaleBy(row, ratio);
  }

  // `difficult` on a non-concave H ("hybrid", [M-5] optimize()): the eigen-
  // directions of -H split into those with clearly positive curvature, which
  // take a Newton step, and the rest, which take a gradient step from there.
  struct CurvatureSplit {
    Vec lambda;      // eigenvalues of -H, largest first
    Mat Q;           // matching eigenvectors as columns
    Vec projected;   // gradient in the eigenbasis
    size_t newton;   // leading directions that get the Newton step
  };
  static CurvatureSplit splitCurvature(const Point& at) {
    CurvatureSplit s;
    eigenLargestFirst(negate(at.H), s.lambda, s.Q);
    s.projected = rowVecTimes(at.g, s.Q);
    const size_t n = s.lambda.size();
    s.newton = 0;
    if (s.lambda[0] > 0 && n >= 2) {
      const double cut = 1e-7 * s.lambda[0];
      const auto strong = std::count_if(s.lambda.begin(), s.lambda.end(), [&](double l) { return l >= cut; });
      s.newton = std::min(static_cast<size_t>(strong), n - 1);
    }
    return s;
  }
  // Q's columns from `first` on, weighted by `weights`.
  static Vec onColumns(const Mat& Q, const Vec& weights, size_t first) {
    Vec dir(Q.size(), 0.0);
    for (size_t r = 0; r < Q.size(); ++r)
      for (size_t c = 0; c < weights.size(); ++c) dir[r] += weights[c] * Q[r][first + c];
    if (anyMissing(dir)) throw notNegativeSemidefinite();
    return dir;
  }
  // No Newton directions leaves the point where it is.
  Move newtonPart(const Vec& b, const Point& at, const CurvatureSplit& s) const {
    if (s.newton == 0) return {b, at.f};
    Vec weights(s.newton);
    for (size_t c = 0; c < s.newton; ++c) weights[c] = s.projected[c] * (1 / s.lambda[c]);
    return lineSearch(b, at.f, onColumns(s.Q, weights, 0), 0.125, 60);
  }
  Vec gradientPart(const Move& from, const Point& at, const CurvatureSplit& s, const Vec& history) const {
    Vec weights(s.projected.begin() + static_cast<long>(s.newton), s.projected.end());
    scaleBy(weights, stepFactor(at.f, weights, weights, history));
    return lineSearch(from.b, from.value, onColumns(s.Q, weights, s.newton), 1.0, 10).b;
  }
  Vec mixedStep(const Vec& b, const Point& at, const Vec& history) const {
    const CurvatureSplit s = splitCurvature(at);
    return gradientPart(newtonPart(b, at, s), at, s, history);
  }

  // ----- convergence ([R] Maximize) --------------------------------------
  bool smallNewtonStep(const Point& at, double nrtol) const {
    const Mat V = invsymStata(negate(at.H));
    if (!passesConcavity(at.H, V)) return false;
    return std::fabs(sumOfProducts(at.g, usableDirection(at.g, V))) < nrtol;
  }
  bool meetsCriteria(const Vec& prevB, double prevF, const Vec& b, const Point& at, const Options& o) {
    const bool params = largestRelChange(b, prevB) <= o.ptol;
    const bool value = relChange(at.f, prevF) <= o.vtol;
    if (!params && !value) return false;
    if (!smallNewtonStep(at, o.nrtol)) return false;
    if (at.newton) return true;
    // A BHHH point must also pass with second differences.
    return smallNewtonStep(differentiate(b, true), o.nrtol);
  }

  LfObjective obj_;
  std::function<double()> uniform_;
  std::vector<std::pair<std::string, int>> plan_{{"nr", 5}};
  Vec multipliers_;
};
// The robust meat sum_r w_r^2 (s_r x_r)(s_r x_r)' from the equation-level
// scores (row x equation) of the final derivative evaluation.
Mat meat(const Data& d, const Mat& score) {
  const size_t k = d.names.size();
  Mat A(k, Vec(k));
  for (size_t j = 0; j < k; ++j) for (size_t l = 0; l <= j; ++l) {
    Accum x;
    for (size_t r = 0; r < score.size(); ++r)
      x.add((score[r][d.eq[j]] * d.X[r][j]) * (score[r][d.eq[l]] * d.X[r][l]) * d.weight[r] * d.weight[r]);
    A[j][l] = A[l][j] = x.get();
  }
  return A;
}
std::vector<std::pair<std::string,int>> schedule(const std::string& spec) {
  const auto w = words(spec);
  std::vector<std::pair<std::string,int>> out;
  for (size_t i = 0; i < w.size(); ++i) {
    if (w[i] != "nr" && w[i] != "bhhh") throw MlError("ml: technique " + w[i] + " is not implemented");
    const std::string method = w[i];
    int count = 5;
    if (i + 1 < w.size() && w[i+1] != "nr" && w[i+1] != "bhhh") {
      const double x = number(w[++i]);
      if (x < 1 || x != std::floor(x) || x > 100000) throw MlError("ml: invalid technique step count");
      count = static_cast<int>(x);
    }
    out.emplace_back(method, count);
  }
  if (out.empty()) throw MlError("ml: empty technique()");
  return out;
}
}  // namespace

ojson runMl(const eval::ParsedCommand& parsed, SessionState& session) {
  try {
    const auto args = words(parsed.argText);
    if (args.empty()) throw MlError("ml: subcommand required");
    const std::string sub = args[0];
    if (sub == "model") session.eval.mlState.reset();
    if (sub == "clear") {
      if (args.size() != 1 || !parsed.opts.empty()) throw MlError("ml clear: invalid syntax");
      session.eval.mlState.reset(); return e3::displayText("");
    }
    std::vector<e3::OptSpec> options;
    if (sub == "model") options = {{"technique",4,true},{"vce",3,true},{"nolog",3,false}};
    else if (sub == "init") options = {{"copy",4,false}};
    else if (sub == "search") options = {{"repeat",3,true}};
    else if (sub == "maximize" || sub == "max") options = {{"difficult",4,false}, {"iterate",4,true},
      {"tolerance",3,true},{"ltolerance",3,true},{"nrtolerance",3,true},{"nolog",3,false}};
    else throw MlError("ml: subcommand " + sub + " is not implemented", 199);
    std::map<std::string,eval::OptValue> opts;
    for (const auto& kv : parsed.opts) {
      const int i = e3::matchOpt(options, kv.first);
      if (i < 0 || kv.second.isBool == options[i].takesArg)
        throw MlError("ml " + sub + ": option " + kv.first + " not allowed");
      opts[options[i].full] = kv.second;
    }
    if (sub != "model" && (parsed.ifExpr || parsed.inRange || parsed.weightVar))
      throw MlError("ml " + sub + ": qualifiers and weights are not allowed");
    if (sub == "model") {
      // A failed redefinition leaves no pending model; stale e() remains live.
      if (!session.hasData()) throw MlError("ml: no observations", 2000);
      std::smatch m;
      if (!std::regex_match(parsed.argText, m, std::regex(R"(^model\s+lf\s+([A-Za-z_]\w*)\s*(.*)$)")))
        throw MlError("ml model: only the lf method is implemented");
      const std::string evaluator = m[1];
      auto found = program::findProgram(session, evaluator);
      if (!found) throw MlError("ml: evaluator program " + evaluator + " not found", 199);
      const std::string rest = trim(m[2]);
      ojson equations = ojson::array();
      std::vector<std::string> depvars;
      std::set<std::string> eqnames;
      size_t p = 0;
      while (p < rest.size()) {
        while (p < rest.size() && std::isspace(static_cast<unsigned char>(rest[p]))) ++p;
        if (p == rest.size()) break;
        if (rest[p] != '(') throw MlError("ml model lf: expected an equation in parentheses");
        size_t end = p + 1;
        for (; end < rest.size() && rest[end] != ')'; ++end)
          if (rest[end] == '(') throw MlError("ml model lf: nested equation syntax is not supported");
        if (end == rest.size()) throw MlError("ml model lf: unterminated equation");
        std::string eqtext = trim(rest.substr(p+1, end-p-1)); p = end+1;
        const auto comma = eqtext.find(',');
        bool noConstant = false;
        if (comma != std::string::npos) {
          if (trim(eqtext.substr(comma+1)) != "noconstant") throw MlError("ml equation: unsupported option");
          noConstant = true; eqtext = trim(eqtext.substr(0,comma));
        }
        std::string eqname = "eq" + std::to_string(equations.size()+1);
        const auto colon = eqtext.find(':');
        if (colon != std::string::npos) { eqname = trim(eqtext.substr(0,colon)); eqtext = trim(eqtext.substr(colon+1)); }
        if (!ident(eqname) || !eqnames.insert(eqname).second) throw MlError("ml: duplicate or invalid equation name");
        std::vector<std::string> ys, terms;
        const auto equal = eqtext.find('=');
        if (equal != std::string::npos) {
          ys = words(eqtext.substr(0,equal)); terms = words(eqtext.substr(equal+1));
        } else if (!eqtext.empty()) throw MlError("ml equation: expected dependent variable = regressors");
        for (const auto& y : ys) { if (!ident(y)) throw MlError("ml: invalid dependent variable"); depvars.push_back(y); }
        for (const auto& term : terms) (void)atoms(term);
        equations.push_back({{"name",eqname},{"depvars",ys},{"terms",terms},{"noconstant",noConstant}});
      }
      if (equations.empty()) throw MlError("ml model lf: at least one equation is required");
      std::vector<std::string> conditions;
      const std::string expr = compileBody(session.programs.at(*found), equations.size(), depvars, session,&conditions);
      const std::string tech = opts.count("technique") ? trim(opts["technique"].strValue) : "nr";
      (void)schedule(tech);
      const std::string vce = opts.count("vce") ? trim(opts["vce"].strValue) : parsed.weightVar ? "robust" : "oim";
      if (vce != "oim" && vce != "robust") throw MlError("ml: only vce(oim) and vce(robust) are implemented");
      ojson state{{"expr",expr},{"conditions",conditions},{"equations",equations},{"user",evaluator},{"depvars",depvars},
        {"technique",tech},{"vce",vce},{"weight",parsed.weightVar.value_or("")},
        {"wexp",parsed.weightExp.value_or(parsed.weightVar.value_or(""))},
        {"if",parsed.ifExpr ? ojson(*parsed.ifExpr) : ojson(nullptr)},
        {"in",parsed.inRange ? ojson(*parsed.inRange) : ojson(nullptr)}};
      auto data = prepare(state, session);
      state["init"] = Vec(data.names.size(),0);
      state["names"] = data.names;
      session.eval.mlState = std::move(state);
      return e3::displayText("");
    }
    if (!session.eval.mlState) throw MlError("ml: model not defined");
    auto& state = *session.eval.mlState;
    if (sub == "init") {
      if (!opts.count("copy")) throw MlError("ml init: only the copy form is implemented");
      Vec b;
      if (args.size() == 2 && session.eval.findMatrix(args[1])) {
        const auto* matrix = session.eval.findMatrix(args[1]);
        for (const auto& row : matrix->data) b.insert(b.end(),row.begin(),row.end());
      } else for (size_t i = 1; i < args.size(); ++i) b.push_back(number(args[i]));
      if (b.size() != state["init"].size()) throw MlError("ml init: conformability error",503);
      for (double x : b) if (!std::isfinite(x)) throw MlError("ml init: missing initial value");
      state["init"] = b; return e3::displayText("");
    }
    if (args.size() != 1) throw MlError("ml " + sub + ": invalid syntax");
    auto d = prepare(state,session);
    if (ojson(d.names) != state["names"]) throw MlError("ml: equation columns changed since ml model; redefine the model");
    Vec b = state["init"].get<Vec>();
    const size_t k = b.size();
    // The search's random starts draw from the session's runiform() stream
    // (`set seed`), the same draws a do-file's runiform() would see next.
    eval::PyNamespace rngNs;
    eval::addSessionRngFns(rngNs, session.eval);
    auto uniform = [&rngNs]() { return rngNs.fns.at("runiform")({}).numAsDouble(); };
    LfOptimizer fitter(d, uniform);
    if (sub == "search") {
      // ml search, repeat(#) is remembered for this model and reused by later
      // searches; the default (10) makes no random attempts.
      if (opts.count("repeat")) {
        const double repeats = number(opts["repeat"].strValue);
        if (repeats < 0 || repeats != std::floor(repeats) || repeats > 100000)
          throw MlError("ml search: invalid repeat count");
        state["search_repeat"] = static_cast<int>(repeats);
      }
      double value = kMissing;
      b = fitter.search(b, state.value("search_repeat", 10), &value);
      state["init"] = b; return e3::displayText("");
    }
    const double maxit = opts.count("iterate") ? number(opts["iterate"].strValue) : 300;
    if (maxit < 0 || maxit != std::floor(maxit) || maxit > 100000) throw MlError("ml maximize: invalid iterate()");
    auto tolerance = [&](const std::string& name, double fallback) {
      const double v = opts.count(name) ? number(opts[name].strValue) : fallback;
      if (!(v >= 0)) throw MlError("ml maximize: tolerance must be nonnegative");
      return v;
    };
    LfOptimizer::Options mo;
    mo.ptol = tolerance("tolerance",1e-6); mo.vtol = tolerance("ltolerance",1e-7);
    mo.nrtol = tolerance("nrtolerance",1e-5);
    mo.maxiter = static_cast<int>(maxit);
    mo.difficult = opts.count("difficult") > 0;
    mo.techniques = schedule(state["technique"]);
    // ml maximize runs the search first, except under iterate(0).
    if (mo.maxiter > 0) {
      double value = kMissing;
      b = fitter.search(b, state.value("search_repeat", 10), &value);
    }
    auto fit = fitter.maximize(b, mo, [&]() { return session.progressCancelRequested(); });
    b = fit.b;
    const double ll = fit.ll;
    const bool converged = fit.converged;
    const int iterations = fit.iterations;
    Mat V = invsymStata(negate(fit.H));
    const bool robust = state["vce"] == "robust";
    if (robust) {
      const Mat M = meat(d,fit.scores), bread = V;
      V.assign(k,Vec(k));
      const double finiteSample = d.X.size() > 1 ? static_cast<double>(d.X.size())/(d.X.size()-1) : 1;
      for (size_t i = 0; i < k; ++i) for (size_t j = 0; j < k; ++j)
        for (size_t a = 0; a < k; ++a) for (size_t c = 0; c < k; ++c)
          V[i][j] += finiteSample*bread[i][a]*M[a][c]*bread[c][j];
    }
    ojson block{{"type","estimation"},{"estimator","ml"},{"depvar",state["depvars"].empty()?"":state["depvars"][0].get<std::string>()},
      {"n_obs",d.X.size()},{"ll",ll},{"converged",converged},{"ic",iterations}};
    block["coeftable"] = e3::coeftableFromPosting(d.names,b,V,std::nan(""));
    block["e_V"] = {{"names",d.names},{"matrix",V}};
    LastModel lm;
    lm.cobraCmd = "ml"; lm.stataCmd = "ml";
    lm.depvar = block["depvar"]; lm.y = lm.depvar; lm.vcov = robust ? "robust" : "iid";
    lm.eVce = state["vce"].get<std::string>(); lm.fitWeight = state["weight"];
    lm.fitWtype = lm.fitWeight.empty()?"":"pw"; lm.fitWexp = state["wexp"];
    lm.result = block;
    std::vector<char> mask(d.allRows,0);
    for (size_t r : d.sourceRows) if (r < mask.size()) mask[r] = 1;
    if (!e3::installEsampleMask(session,lm,mask)) throw MlError("ml: could not install estimation sample",498);
    session.setLastModel(lm); session.lastEstBlock = block;
    e3::postBV(session,d.names,b,V);
    eval::replaceRClass(session.eval);
    auto scalar = [&](const std::string& name, double v) { e3::setEScalarOnFit(session,name,v); };
    auto local = [&](const std::string& name, const std::string& v) { e3::setEStringOnFit(session,name,v); };
    scalar("N",d.X.size()); scalar("ll",ll); scalar("converged",converged); scalar("ic",iterations);
    scalar("rc",0); scalar("k",k); scalar("rank",k); scalar("k_eq",state["equations"].size());
    scalar("k_dv",state["depvars"].size());
    local("cmd","ml"); local("opt","moptimize"); local("crittype","log likelihood");
    local("user",state["user"]); local("ml_method","lf"); local("technique",state["technique"]);
    local("vce",state["vce"]); local("vcetype",robust?"Robust":"");
    local("depvar",lm.depvar); local("wtype",lm.fitWeight.empty()?"":"pweight");
    local("wexp",lm.fitWeight.empty()?"":"= "+lm.fitWexp);
    // Wald test of equation regressors (intercepts/ancillary constants excluded).
    std::vector<size_t> js;
    for (size_t j = 0; j < k; ++j) if (d.names[j].substr(d.names[j].find(':')+1) != "_cons") js.push_back(j);
    if (!js.empty()) {
      Mat vc(js.size(),Vec(js.size())); Vec coef;
      for (size_t j = 0; j < js.size(); ++j) {
        coef.push_back(b[js[j]]);
        for (size_t l = 0; l < js.size(); ++l) vc[j][l] = V[js[j]][js[l]];
      }
      Mat vi;
      if (inverse(vc,vi)) { scalar("chi2",dot(coef,multiply(vi,coef))); scalar("df_m",js.size()); local("chi2type","Wald"); }
    }
    state["init"] = b;
    return block;
  } catch (const MlError& e) { return e3::rcError(e.what(),e.rc,"engine_error"); }
    catch (const std::exception& e) { return e3::rcError(std::string("ml: ")+e.what(),198,"engine_error"); }
}
}  // namespace piet::handlers
