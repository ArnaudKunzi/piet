// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Extended evalcase kinds: exprfn / pyexpr (expression engine) and the
// macro-engine kinds (eval_arith / eval_macro_expr / expand_macros /
// capture_definition / macro_script / has_macro_refs / capture_r).
#include <cmath>
#include <string>
#include <vector>

#include "evalcase_internal.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"

namespace piet::eval {

using ojson = nlohmann::ordered_json;

namespace {

// Tagged-value codec shared with py_twin.py (see its enc_value/dec_value):
//   {"t":"none"} | {"t":"bool","v":..} | {"t":"int","v":..} |
//   {"t":"float","v": number | "__nan__" | "__inf__" | "__-inf__"} |
//   {"t":"str","v":..}
PyValue decValue(const ojson& j) {
  const std::string t = j.at("t").get<std::string>();
  if (t == "bool") return PyValue::boolean(j.at("v").get<bool>());
  if (t == "int") return PyValue::integer(j.at("v").get<long long>());
  if (t == "float") {
    const auto& v = j.at("v");
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      if (s == "__nan__") return PyValue::floating(std::nan(""));
      if (s == "__inf__") return PyValue::floating(HUGE_VAL);
      if (s == "__-inf__") return PyValue::floating(-HUGE_VAL);
      return PyValue::floating(std::stod(s));
    }
    return PyValue::floating(v.get<double>());
  }
  if (t == "str") return PyValue::str(j.at("v").get<std::string>());
  throw std::runtime_error("unsupported value tag: " + t);
}

ojson encFloat(double f) {
  if (std::isnan(f)) return ojson("__nan__");
  if (std::isinf(f)) return ojson(f > 0 ? "__inf__" : "__-inf__");
  return ojson(f);
}

ojson encValue(const PyValue& v) {
  ojson out;
  switch (v.kind()) {
    case PyValue::Kind::Bool:
      out["t"] = "bool";
      out["v"] = v.boolVal();
      break;
    case PyValue::Kind::Int:
      out["t"] = "int";
      out["v"] = v.intVal();
      break;
    case PyValue::Kind::Float:
      out["t"] = "float";
      out["v"] = encFloat(v.floatVal());
      break;
    case PyValue::Kind::Str:
      out["t"] = "str";
      out["v"] = v.strVal();
      break;
  }
  return out;
}

ojson handleExprfn(const ojson& c) {
  const std::string fnName = c.at("fn").get<std::string>();

  PyNamespace ns;
  addStataFns(ns);
  // Session-bound scalar(): the case may carry a {"scalars": {...}} map.
  std::vector<std::pair<std::string, std::string>> scalars;
  if (c.contains("scalars")) {
    for (const auto& [k, v] : c.at("scalars").items()) {
      scalars.emplace_back(k, v.get<std::string>());
    }
  }
  addScalarFn(ns, [&scalars](const std::string& name) -> std::optional<std::string> {
    for (const auto& [k, v] : scalars) {
      if (k == name) return v;
    }
    return std::nullopt;
  });

  auto it = ns.fns.find(fnName);
  if (it == ns.fns.end()) {
    ojson out;
    out["ok"] = false;
    out["driver_error"] = "unknown exprfn: " + fnName;
    return out;
  }

  std::vector<PyValue> args;
  if (c.contains("args")) {
    for (const auto& a : c.at("args")) args.push_back(decValue(a));
  }

  ojson out;
  try {
    PyValue result = it->second(args);
    out["ok"] = true;
    out["result"] = encValue(result);
  } catch (const PyExc&) {
    // Exception TEXT is Python-internal (all oracle callers catch broadly);
    // parity compares the raised/not-raised outcome only.
    out["ok"] = false;
    out["raised"] = true;
  }
  return out;
}

// --- session codec (mirrors py_twin make_session / dump_session) -----------

double decFloatValue(const ojson& v) {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "__nan__") return std::nan("");
    if (s == "__inf__") return HUGE_VAL;
    if (s == "__-inf__") return -HUGE_VAL;
    return std::stod(s);
  }
  return v.get<double>();
}

EvalMatrix decMatrix(const ojson& j) {
  EvalMatrix m;
  if (j.contains("data")) {
    for (const auto& row : j.at("data")) {
      std::vector<double> r;
      for (const auto& cell : row) {
        r.push_back(cell.is_null() ? std::nan("") : decFloatValue(cell));
      }
      m.data.push_back(std::move(r));
    }
  }
  if (j.contains("rownames")) {
    for (const auto& n : j.at("rownames")) m.rownames.push_back(n.get<std::string>());
  }
  if (j.contains("colnames")) {
    for (const auto& n : j.at("colnames")) m.colnames.push_back(n.get<std::string>());
  }
  return m;
}

EvalSession decodeSession(const ojson& spec) {
  EvalSession s;
  if (spec.is_null()) return s;
  if (spec.contains("locals")) {
    for (const auto& [k, v] : spec.at("locals").items()) s.locals.set(k, v.get<std::string>());
  }
  if (spec.contains("globals")) {
    for (const auto& [k, v] : spec.at("globals").items()) s.globals.set(k, v.get<std::string>());
  }
  if (spec.contains("scalars")) {
    for (const auto& [k, v] : spec.at("scalars").items()) s.scalars.set(k, v.get<std::string>());
  }
  if (spec.contains("r_values")) {
    for (const auto& [k, v] : spec.at("r_values").items()) s.rValues.set(k, decFloatValue(v));
  }
  if (spec.contains("r_strings")) {
    for (const auto& [k, v] : spec.at("r_strings").items()) s.rStrings.set(k, v.get<std::string>());
  }
  if (spec.contains("e_scalars")) {
    for (const auto& [k, v] : spec.at("e_scalars").items()) s.eScalars.set(k, decFloatValue(v));
  }
  if (spec.contains("matrices")) {
    for (const auto& [k, v] : spec.at("matrices").items()) s.matrices.emplace_back(k, decMatrix(v));
  }
  if (spec.contains("e_matrices")) {
    for (const auto& [k, v] : spec.at("e_matrices").items()) s.eMatrices.emplace_back(k, decMatrix(v));
  }
  if (spec.contains("var_meta")) {
    for (const auto& [k, v] : spec.at("var_meta").items()) {
      if (v.contains("label") && !v.at("label").is_null()) {
        s.varLabels.set(k, v.at("label").get<std::string>());
      } else {
        s.varLabels.set(k, "");
      }
    }
  }
  if (spec.contains("cwd") && spec.at("cwd").is_string()) s.cwd = spec.at("cwd").get<std::string>();
  if (spec.contains("last_model_result") && !spec.at("last_model_result").is_null()) {
    s.lastModelResult = spec.at("last_model_result");
  }
  return s;
}

ojson encFloatMap(const OrderedFloatDict& d) {
  ojson out = ojson::object();
  for (const auto& [k, v] : d.items()) out[k] = encFloat(v);
  return out;
}

ojson encStrMap(const OrderedDict& d) {
  ojson out = ojson::object();
  for (const auto& [k, v] : d.items()) out[k] = v;
  return out;
}

// py_twin.dump_session shape.
ojson dumpSession(const EvalSession& s) {
  ojson out;
  out["locals"] = encStrMap(s.locals);
  out["globals"] = encStrMap(s.globals);
  out["scalars"] = encStrMap(s.scalars);
  out["r_values"] = encFloatMap(s.rValues);
  out["r_strings"] = encStrMap(s.rStrings);
  return out;
}

EvalSession sessionFromCase(const ojson& c) {
  if (c.contains("session") && !c.at("session").is_null()) return decodeSession(c.at("session"));
  return EvalSession{};
}

// --- macro-engine kinds ------------------------------------------------------

ojson handleEvalArith(const ojson& c) {
  ojson out;
  out["ok"] = true;
  std::string expr = c.at("expr").get<std::string>();
  if (c.contains("session") && !c.at("session").is_null()) {
    EvalSession s = decodeSession(c.at("session"));
    auto r = evalArith(expr, &s);
    out["result"] = r ? ojson(*r) : ojson(nullptr);
  } else {
    auto r = evalArith(expr, nullptr);
    out["result"] = r ? ojson(*r) : ojson(nullptr);
  }
  return out;
}

ojson handleEvalMacroExpr(const ojson& c) {
  EvalSession s = sessionFromCase(c);
  auto r = evalMacroExpr(c.at("expr").get<std::string>(), s);
  ojson out;
  out["ok"] = true;
  out["result"] = r ? ojson(*r) : ojson(nullptr);
  return out;
}

ojson handleExpandMacros(const ojson& c) {
  EvalSession s = sessionFromCase(c);
  ExpandResult r = expandMacros(c.at("line").get<std::string>(), s);
  ojson out;
  if (r.isError) {
    out["ok"] = false;
    out["error"] = r.error;
  } else {
    out["ok"] = true;
    out["result"] = r.line;
  }
  return out;
}

ojson handleCaptureDefinition(const ojson& c) {
  EvalSession s = sessionFromCase(c);
  bool captured = tryCaptureDefinition(c.at("line").get<std::string>(), s);
  ojson out;
  out["ok"] = true;
  out["captured"] = captured;
  out["session_after"] = dumpSession(s);
  return out;
}

ojson handleMacroScript(const ojson& c) {
  // Mirrors the dispatcher step-0a/0b pipeline for macro-layer lines (and
  // py_twin.h_macro_script): capture first; else expand when refs present.
  EvalSession s = sessionFromCase(c);
  ojson results = ojson::array();
  for (const auto& lineJ : c.at("lines")) {
    std::string line = lineJ.get<std::string>();
    if (tryCaptureDefinition(line, s)) {
      results.push_back(ojson{{"captured", true}});
      continue;
    }
    if (hasMacroRefs(line)) {
      ExpandResult r = expandMacros(line, s);
      if (r.isError) {
        results.push_back(ojson{{"error", r.error}});
      } else {
        results.push_back(ojson{{"expanded", r.line}});
      }
    } else {
      results.push_back(ojson{{"noop", true}});
    }
  }
  ojson out;
  out["ok"] = true;
  out["results"] = results;
  out["session_after"] = dumpSession(s);
  return out;
}

ojson handleHasMacroRefs(const ojson& c) {
  ojson out;
  out["ok"] = true;
  out["result"] = hasMacroRefs(c.at("line").get<std::string>());
  return out;
}

ojson handleCaptureR(const ojson& c) {
  EvalSession s = sessionFromCase(c);
  captureRFromBlock(c.at("block"), s);
  ojson out;
  out["ok"] = true;
  out["r_values"] = encFloatMap(s.rValues);
  return out;
}

ojson handlePyexpr(const ojson& c) {
  // Raw engine diff: mathNamespace mirrors macros._MATH_NS (no inf/nan) or
  // postfile._MATH_NS (with inf/nan, without STATA_FNS).
  bool postfile = c.value("ns", std::string("arith")) == "postfile";
  PyNamespace ns = mathNamespace(/*withInfNan=*/postfile);
  if (!postfile) addStataFns(ns);

  ojson out;
  auto result = evalPyExpr(c.at("expr").get<std::string>(), ns);
  if (!result) {
    out["ok"] = false;
    out["raised"] = true;
    return out;
  }
  out["ok"] = true;
  out["result"] = encValue(*result);
  return out;
}

}  // namespace

nlohmann::ordered_json handleExtendedCase(const std::string& kind, const nlohmann::ordered_json& c) {
  if (kind == "exprfn") return handleExprfn(c);
  if (kind == "pyexpr") return handlePyexpr(c);
  if (kind == "eval_arith") return handleEvalArith(c);
  if (kind == "eval_macro_expr") return handleEvalMacroExpr(c);
  if (kind == "expand_macros") return handleExpandMacros(c);
  if (kind == "capture_definition") return handleCaptureDefinition(c);
  if (kind == "macro_script") return handleMacroScript(c);
  if (kind == "has_macro_refs") return handleHasMacroRefs(c);
  if (kind == "capture_r") return handleCaptureR(c);
  return nlohmann::ordered_json(nullptr);
}

}  // namespace piet::eval
