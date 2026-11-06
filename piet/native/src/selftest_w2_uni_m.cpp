// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode names, lane M: macro expansion and the expression lexers.
//
// The one shared rule (src/core/ident.h) says every scanner treats a byte >= 0x80
// as an identifier byte. Lane M put that rule into the macro layer (macros.cpp:
// isIdentStart/isIdentChar, the safe-arithmetic whitelist, every name regex),
// the Python-expression lexer (pyeval.cpp), the loop/condition layer (loops.cpp)
// and the tokenizer's unexpanded-global net. Every expected value below is
// StataBE 19.5's: scratch-r150/uni-oracle/ops.golden and the lane's own
// scratch-r150/uni-m/m1.do -> m1.golden (each OUT| line quoted beside its arm).
// The mixed-script arm is the one the rule exists for: with a scalar `_2020`
// defined, `دخل_2020` must stay ONE name -- before, the Arabic bytes were word
// breaks and the ASCII tail `_2020` was bound to the scalar on its own.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// Every `OUT|tag|value` display line in `blocks`, keyed by tag (last wins).
std::map<std::string, std::string> outLines(const ojson& blocks) {
  std::map<std::string, std::string> out;
  if (!blocks.is_array()) return out;
  for (const auto& b : blocks) {
    if (!b.is_object() || b.value("type", std::string()) != "display") continue;
    const std::string text = b.value("text", std::string());
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      if (nl == std::string::npos) nl = text.size();
      const std::string line = text.substr(at, nl - at);
      at = nl + 1;
      if (line.rfind("OUT|", 0) != 0) continue;
      const size_t bar = line.find('|', 4);
      if (bar == std::string::npos) continue;
      out[line.substr(4, bar - 4)] = line.substr(bar + 1);
    }
  }
  return out;
}

std::string uncapturedError(const ojson& blocks) {
  if (!blocks.is_array()) return "dispatch did not return an array";
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error" && !b.value("captured", false))
      return b.value("message", std::string("error"));
  }
  return std::string();
}

}  // namespace

void testW2UniM() {
  std::printf("[selftest] --- Unicode names (M): macro expansion, expression lexers ---\n");

  // ── pyeval lexer: a UTF-8 name is ONE Name token ─────────────────────────
  {
    piet::eval::PyNamespace ns;
    ns.consts["قيمة"] = piet::eval::PyValue::integer(2);
    ns.consts["دخل_2020"] = piet::eval::PyValue::integer(5);
    ns.consts["_2020"] = piet::eval::PyValue::integer(99);
    auto v = piet::eval::evalPyExpr("قيمة + دخل_2020", ns);
    w2check(v && v->isInt() && v->intVal() == 7,
            "UNI/M pyeval: `قيمة + دخل_2020` lexes two names (2 + 5 = 7); the ASCII tail "
            "`_2020` of the mixed-script name is not a token of its own (main: the first Arabic "
            "byte was `unexpected character`)");
    auto s = piet::eval::evalPyExpr("ملاحظات + \"x\"",
                                    [] {
                                      piet::eval::PyNamespace n;
                                      n.consts["ملاحظات"] = piet::eval::PyValue::str("hello");
                                      return n;
                                    }());
    w2check(s && s->isStr() && s->strVal() == "hellox",
            "UNI/M pyeval: a string name `ملاحظات + \"x\"` -> hellox (ops.golden gen_str_expr)");
  }

  // ── macro layer, no dataset ──────────────────────────────────────────────
  {
    piet::eval::EvalSession s;
    piet::eval::MacroFnError err;
    w2check(piet::eval::tryCaptureDefinition("local م \"abc\"", s, &err) &&
                s.locals.get("م", "<unset>") == "abc",
            "UNI/M local NAME: `local م \"abc\"` stores the local `م` (m1 local_name)");
    w2check(piet::eval::tryCaptureDefinition("global ج \"gval\"", s, &err) &&
                s.globals.get("ج", "<unset>") == "gval",
            "UNI/M global NAME: `global ج \"gval\"` stores the global `ج` (m1 global_name)");
    auto e1 = piet::eval::expandMacros("display \"`م'|$ج|${ج}\"", s);
    w2check(!e1.isError && e1.line == "display \"abc|gval|gval\"",
            "UNI/M expansion: `` `م' ``, `$ج` and `${ج}` expand (m1 local_name/global_name/"
            "global_brace); got [" + e1.line + "]");
    // A global reference must START with a letter; later characters are any
    // non-ASCII (StataBE m2.do: euro, aidigit, nbsp print literally;
    // arabic_glued `$ق€` is the undefined global `ق€`; tick_euro `` `€' `` is
    // an (undefined) local reference and expands to "").
    auto e2 = piet::eval::expandMacros("display \"Price $€5|x$٥٠y|x$ y\"", s);
    w2check(!e2.isError && e2.line == "display \"Price $€5|x$٥٠y|x$ y\"",
            "UNI/M `$` + a non-letter is literal text (m2 euro/aidigit/nbsp); got [" + e2.line +
                "]");
    auto e3 = piet::eval::expandMacros("display \"[$ج€][`€']\"", s);
    w2check(!e3.isError && e3.line == "display \"[][]\"",
            "UNI/M `$ج€` reads the global `ج€` and `` `€' `` the local `€` (both undefined -> \"\"; "
            "m2 arabic_glued/tick_euro); got [" + e3.line + "]");
    w2check(!piet::eval::tryCaptureDefinition("global ٥ 3", s, &err) &&
                s.globals.find("٥") == nullptr,
            "UNI/M `global ٥ 3` defines nothing (StataBE r(198), m2 def_digit)");
    w2check(piet::eval::tryCaptureDefinition("local €x 3", s, &err) &&
                s.locals.get("€x", "<unset>") == "3",
            "UNI/M `local €x 3` is a local definition (StataBE rc 0, m2 def_euro)");
    w2check(piet::eval::tryCaptureDefinition("scalar سعر = 40", s, &err) &&
                s.scalars.find("سعر") != nullptr,
            "UNI/M scalar NAME: `scalar سعر = 40` defines the scalar `سعر`");
    auto a1 = piet::eval::evalArith("سعر + 1", &s);
    w2check(a1 && *a1 == "41", "UNI/M evalArith: a bare Unicode scalar name binds (`سعر + 1` -> 41)");
    auto a2 = piet::eval::evalArith("scalar(سعر)", &s);
    w2check(a2 && *a2 == "40", "UNI/M evalArith: `scalar(سعر)` -> 40 (ops.golden scalar_fn)");
    // The mixed-script case at the substitution layer: scalar `_2020` must not
    // be spliced into the tail of `دخل_2020`, nor `x` into `xملاحظات`.
    w2check(piet::eval::substituteBareName("دخل_2020 + _2020", "_2020", "99") == "دخل_2020 + 99",
            "UNI/M bare-name substitution: scalar `_2020` binds only the standalone name, never "
            "the tail of `دخل_2020`");
    w2check(piet::eval::substituteBareName("xملاحظات + x", "x", "3") == "xملاحظات + 3",
            "UNI/M bare-name substitution: scalar `x` is not substituted inside `xملاحظات`");
    w2check(piet::eval::hasMacroRefs("display `ملاحظات'"),
            "UNI/M hasMacroRefs: `` `ملاحظات' `` is a local reference");
  }

  // ── subscripted Unicode variables through a variable reader ─────────────
  // EvalSession::varObs stands in for the dataset (SessionState installs the
  // real one), so these arms hold the macro layer alone to the oracle values:
  // id = 1..4, قيمة = id*2, ملاحظات = "hello", دخل_2020 = 5 (ops.do / m1.do).
  {
    piet::eval::EvalSession s;
    s.varObs = [](const std::string& name,
                  long long idx) -> std::optional<piet::eval::EvalSession::VarObsReading> {
      piet::eval::EvalSession::VarObsReading r;
      r.inRange = idx >= 1 && idx <= 4;
      if (name == "قيمة") {
        r.text = std::to_string(2 * idx);
      } else if (name == "ملاحظات") {
        r.text = "hello";
        r.isString = true;
      } else if (name == "دخل_2020") {
        r.text = "5";
      } else {
        return std::nullopt;
      }
      return r;
    };
    s.scalars.set("_2020", "99");
    auto m1 = piet::eval::expandMacros("display \"`=ملاحظات[1]'\"", s);
    w2check(!m1.isError && m1.line == "display \"hello\"",
            "UNI/M `=ملاحظات[1]': hello (ops.golden macro_eq; main: the safe-arith whitelist "
            "refused every byte >= 0x80); got [" + m1.line + "]");
    auto v3 = piet::eval::evalArith("قيمة[3]", &s);
    w2check(v3 && *v3 == "6", "UNI/M evalArith `قيمة[3]` -> 6 (ops.golden local_eq_sub)");
    auto v10 = piet::eval::evalArith("قيمة[2]*10", &s);
    w2check(v10 && *v10 == "40", "UNI/M evalArith `قيمة[2]*10` -> 40 (ops.golden scalar)");
    auto mx = piet::eval::evalArith("دخل_2020[1] + 1", &s);
    w2check(mx && *mx == "6",
            "UNI/M mixed-script: `دخل_2020[1] + 1` -> 6 with scalar _2020 = 99 defined (m1 "
            "mixed_local); got [" + (mx ? *mx : std::string("nullopt")) + "]");
    auto mb = piet::eval::evalArith("دخل_2020", &s);
    w2check(mb && *mb == "5",
            "UNI/M mixed-script bare name: `دخل_2020` reads the variable (5), never `دخل` + "
            "scalar `_2020`; got [" + (mb ? *mb : std::string("nullopt")) + "]");
  }

  // ── the macro/scan cross-checks over the widened corpora ─────────────────
  {
    std::string why;
    w2check(piet::eval::selftestSafeArithScanMatchesRegex(&why),
            "UNI/M safe-arith scan == regex twin over the Unicode corpus " + why);
    why.clear();
    w2check(piet::eval::selftestLocalRefScanMatchesRegex(&why),
            "UNI/M local-ref scan == regex twin over the Unicode corpus " + why);
    why.clear();
    w2check(piet::eval::selftestScalarScansMatchRegex(&why),
            "UNI/M scalar scans == regex twins over the Unicode corpus " + why);
    why.clear();
    w2check(piet::eval::selftestMacroDefScanMatchesRegex(&why),
            "UNI/M local/global definition scan == regex twin over the Unicode corpus " + why);
    why.clear();
    w2check(piet::loops::selftestBoolIdentScanMatchesRegex(&why),
            "UNI/M condition identifier scan == regex twin over the Unicode corpus " + why);
  }

  // ── a do-file through the real session (m1.do's lane-M forms) ────────────
  {
    namespace fs = std::filesystem;
    const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2unim").u8string();
    std::error_code ec;
    fs::remove_all(workdir, ec);
    fs::create_directories(workdir, ec);
    piet::SessionState session(workdir);
    const std::string src =
        "clear\n"
        "set obs 4\n"
        "generate id = _n\n"
        "generate double qv = id*2\n"
        "rename qv قيمة\n"
        "generate str10 sv = \"hello\"\n"
        "rename sv ملاحظات\n"
        "display \"OUT|macro_eq|`=ملاحظات[1]'\"\n"
        "local x = قيمة[3]\n"
        "display \"OUT|local_eq_sub|`x'\"\n"
        "scalar سعر = قيمة[2]*10\n"
        "display \"OUT|scalar_fn|\" scalar(سعر)\n"
        "local م \"abc\"\n"
        "display \"OUT|local_name|`م'\"\n"
        "if قيمة[1] > 0 {\n"
        "    display \"OUT|if_block|taken\"\n"
        "}\n"
        "foreach ع in a b {\n"
        "    display \"OUT|foreach_uvar|`ع'\"\n"
        "}\n"
        "scalar _2020 = 99\n"
        "generate dv = 5\n"
        "rename dv دخل_2020\n"
        "local z = دخل_2020[1] + 1\n"
        "display \"OUT|mixed_local|`z'\"\n"
        "display \"OUT|mixed_inline|`=دخل_2020[1]'\"\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    const auto out = outLines(r.blocks);
    const std::string err = uncapturedError(r.blocks);
    auto got = [&out](const std::string& tag) {
      auto it = out.find(tag);
      return it == out.end() ? std::string("<missing>") : it->second;
    };
    w2check(err.empty(), "UNI/M do-file: no uncaptured error (got [" + err + "])");
    // m1.golden / ops.golden values, verbatim.
    const std::vector<std::pair<std::string, std::string>> want = {
        {"macro_eq", "hello"},      // `=ملاحظات[1]' (main: the safe-arith gate refused it)
        {"local_eq_sub", "6"},      // local x = قيمة[3]
        {"scalar_fn", "40"},        // scalar سعر = قيمة[2]*10 ; scalar(سعر)
        {"local_name", "abc"},      // local م
        {"if_block", "taken"},      // if قيمة[1] > 0 { }
        {"foreach_uvar", "b"},      // foreach ع in a b (last iteration)
        {"mixed_local", "6"},       // local z = دخل_2020[1] + 1, scalar _2020 = 99 defined
        {"mixed_inline", "5"},      // `=دخل_2020[1]'
    };
    for (const auto& [tag, value] : want) {
      w2check(got(tag) == value, "UNI/M do-file " + tag + ": want [" + value + "] got [" + got(tag) +
                                     "]");
    }
  }
}
