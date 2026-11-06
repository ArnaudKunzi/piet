// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2c lane `datafns` (piet-row-evaluator-string-data-arms) -- the string-valued
// DATA arms of tests/eval/golden/aer_w2/w2c_functions.log: `generate v = f(s)`
// on the do-file's own fixture (`set obs 20`, s = "  Ab  cD " on every row,
// w2c_functions.do:20-27). Every arm names the PREC record it rests on and
// asserts what the record pins: rc, the row-1 value, the storage type where the
// record prints one, and the missing count where the log prints
// "(N missing values generated)" (read here from the data_mutation block's
// n_missing). The macro twins of these functions are selftest_w2c_macrofns.cpp.
//
// Layers: generate goes through PIET's row evaluator (strmutate.cpp evalRows
// over exprfns.cpp's addStataFns + addRowPlaneRegexs); `count if`/`keep if`
// through the same evaluator's condition-only mode (needsStringCount). The
// records print their values through the macro layer (eval/macros.cpp: the
// inline `=VAR[1]' and `: type VAR' forms). The engine evaluates nothing here.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/translate.h"
#include "vendor/nlohmann/json.hpp"

namespace {

struct Reply {
  std::string rc = "0";
  std::string text;      // every display block's text, concatenated
  long long nMissing = -1;  // n_missing of `var` in the last data_mutation block
};

Reply run(piet::SessionState& session, const std::string& cmd, const std::string& var = "") {
  Reply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
      } else if (t == "display") {
        r.text += b.value("text", std::string());
      } else if (t == "data_mutation" && !var.empty() && b.contains("varlist_after") &&
                 b["varlist_after"].is_array()) {
        for (const auto& v : b["varlist_after"]) {
          if (v.is_object() && v.value("name", std::string()) == var && v.contains("n_missing") &&
              v["n_missing"].is_number_integer()) {
            r.nMissing = v["n_missing"].get<long long>();
          }
        }
      }
    }
  } catch (const std::exception& e) {
    r.rc = std::string("EXC ") + e.what();
  }
  return r;
}

std::string typeOf(piet::SessionState& s, const std::string& var) {
  (void)run(s, "local __w2cd_t : type " + var);
  return s.eval.locals.get("__w2cd_t", "<unset>");
}

// One string data arm: `generate var = rhs` at rc 0, row 1 == want; `type` and
// `nMissing` are checked when given (the record or the log pins them).
void strArm(piet::SessionState& s, const std::string& arm, const std::string& var,
            const std::string& rhs, const std::string& want, const std::string& type = "",
            long long nMissing = -1) {
  const Reply g = run(s, "generate " + var + " = " + rhs, var);
  const Reply d = run(s, "display \"[\" " + var + "[1] \"]\"");
  const std::string t = type.empty() ? std::string() : typeOf(s, var);
  const bool ok = g.rc == "0" && d.text == "[" + want + "]" && (type.empty() || t == type) &&
                  (nMissing < 0 || g.nMissing == nMissing);
  w2check(ok, "W2c(datafns) " + arm + ": `generate " + var + " = " + rhs + "` -> rc 0, [" + want +
                  "]" + (type.empty() ? "" : " " + type) +
                  (nMissing < 0 ? "" : ", " + std::to_string(nMissing) + " missing") +
                  " -- got rc " + g.rc + " " + d.text + (type.empty() ? "" : " " + t) +
                  (nMissing < 0 ? "" : ", " + std::to_string(g.nMissing) + " missing"));
}

// One numeric data arm: `generate var = rhs` at rc 0, `display var[1]` == want.
void numArm(piet::SessionState& s, const std::string& arm, const std::string& var,
            const std::string& rhs, const std::string& want) {
  const Reply g = run(s, "generate " + var + " = " + rhs, var);
  const Reply d = run(s, "display " + var + "[1]");
  w2check(g.rc == "0" && d.text == want,
          "W2c(datafns) " + arm + ": `generate " + var + " = " + rhs + "` -> rc 0, " + want +
              " -- got rc " + g.rc + " " + d.text);
}

void fixture(piet::SessionState& s) {
  (void)run(s, "clear");
  (void)run(s, "set obs 20");
  (void)run(s, "generate x = _n - 10");
  (void)run(s, "generate s = \"  Ab  cD \"");
  (void)run(s, "generate s2 = \"q\"");
}

}  // namespace

void testW2cDataFns() {
  std::printf("[selftest] --- W2c string data functions (w2c_functions.log data arms) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2cdatafns").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  {
    piet::SessionState s(workdir);
    fixture(s);

    // itrim_data :440 -- main: r(1), itrim() unregistered.
    strArm(s, "itrim_data :440", "__d_itrim", "itrim(s)", "  Ab cD ");

    // regexs_* :599/:609/:619 -- main: r(1), regexs() had no row-plane entry.
    // The regexm() state is process-wide and earlier selftests leave a match in
    // it; StataBE ran the do-file in a fresh session, where nothing had matched.
    // A match of the empty string restores that reading of regexs(0) ("").
    (void)run(s, "local __w2cd_prime = regexm(\"b\", \"a*\")");
    strArm(s, "regexs_whole :599", "__d_regexs0", "cond(regexm(s,\"A\"), regexs(0), \"NOMATCH\")",
           "", "str1", 1);
    strArm(s, "regexs_group_matched :609", "__d_regexs1",
           "cond(regexm(s,\"(A)(Z)?\"), regexs(1), \"NOMATCH\")", "", "", 1);
    strArm(s, "regexs_group_unmatched :619", "__d_regexs2",
           "cond(regexm(s,\"(A)(Z)?\"), regexs(2), \"EMPTY\")", "", "", 20);
    {
      // regexs_macro_standalone :628 -- the state the row loop left behind.
      const Reply r = run(s, "local __m_regexs = regexs(0)");
      const std::string got = s.eval.locals.get("__m_regexs", "<unset>");
      w2check(r.rc == "0" && got == "A",
              "W2c(datafns) regexs_macro_standalone :628 after the data arms: `local __m_regexs "
              "= regexs(0)` -> A -- got rc " + r.rc + " [" + got + "]");
    }

    // reverse_data :639 / strofreal_data :948 / strproper_data :968 -- main
    // stored the right values; these pin them on the row path.
    strArm(s, "reverse_data :639", "__d_rev", "reverse(s)", " Dc  bA  ");
    numArm(s, "strmatch_star :919", "__d_sm_star", "strmatch(s,\"*b*\")", "1");
    numArm(s, "strmatch_q :928", "__d_sm_q", "strmatch(s2,\"?\")", "1");
    strArm(s, "strofreal_data :948", "__d_sor", "strofreal(x,\"%5.2f\")", "-9.00", "str5");
    strArm(s, "strproper_data :968", "__d_sp", "strproper(s)", "  Ab  Cd ");
    numArm(s, "strrpos_data :988", "__d_srp", "strrpos(s,\"c\")", "7");

    // ustr* :1090-:1174 -- code points, not bytes. main: ustrleft/ustrlen/
    // ustrregexm/ustrtrim r(1) (unregistered).
    strArm(s, "ustrunescape_data :1090", "__d_uni", "ustrunescape(\"caf\\u00e9 na\\u00efve\")",
           "caf\xc3\xa9 na\xc3\xafve");
    strArm(s, "ustrleft_data :1110", "__d_ustrleft", "ustrleft(__d_uni,4)", "caf\xc3\xa9");
    numArm(s, "ustrlen_data :1121", "__d_ustrlen", "ustrlen(__d_uni)", "10");
    numArm(s, "ustrregexm_data :1132", "__d_ustrregexm", "ustrregexm(__d_uni,\"caf\")", "1");
    strArm(s, "ustrregexra_data :1143", "__d_ustrregexra", "ustrregexra(__d_uni,\"caf\",\"tea\")",
           "tea\xc3\xa9 na\xc3\xafve");
    strArm(s, "ustrtrim_data :1163", "__d_ustrtrim", "ustrtrim(\" \" + __d_uni + \" \")",
           "caf\xc3\xa9 na\xc3\xafve");
    strArm(s, "usubinstr_data :1174", "__d_usi", "usubinstr(s,\"b\",\"z\",.)", "  Az  cD ");
    numArm(s, "wordcount_data :1194", "__d_wc", "wordcount(s)", "2");

    // The records' own display lines, verbatim from w2c_functions.do, against
    // the oracle's PREC text. They print the value through an inline
    // `` `=VAR[1]' `` (BUG-718) and the type through an inline `` `: type VAR' ``
    // (BUG-719). main: the first never converged (rc 1, PRECFAIL display_rc=1),
    // the second was refused as unsupported_macro (display_rc=198).
    {
      (void)run(s, "local __rc = 0");
      struct Rec {
        const char* arm;
        const char* line;
        const char* want;
      };
      const Rec recs[] = {
          {"itrim_data :440", "display \"PREC|itrim_data|\" `__rc' \"|`=__d_itrim[1]'|\"",
           "PREC|itrim_data|0|  Ab cD |"},
          {"regexs_whole :599",
           "display \"PREC|regexs_whole|\" `__rc' \"|`=__d_regexs0[1]'|`: type __d_regexs0'|\"",
           "PREC|regexs_whole|0||str1|"},
          {"regexs_group_matched :609",
           "display \"PREC|regexs_group_matched|\" `__rc' \"|`=__d_regexs1[1]'|\"",
           "PREC|regexs_group_matched|0||"},
          {"regexs_group_unmatched :619",
           "display \"PREC|regexs_group_unmatched|\" `__rc' \"|`=__d_regexs2[1]'|\"",
           "PREC|regexs_group_unmatched|0||"},
          {"reverse_data :639", "display \"PREC|reverse_data|\" `__rc' \"|`=__d_rev[1]'|\"",
           "PREC|reverse_data|0| Dc  bA  |"},
          {"strofreal_data :948",
           "display \"PREC|strofreal_data|\" `__rc' \"|`=__d_sor[1]'|`: type __d_sor'|\"",
           "PREC|strofreal_data|0|-9.00|str5|"},
          {"strproper_data :968", "display \"PREC|strproper_data|\" `__rc' \"|`=__d_sp[1]'|\"",
           "PREC|strproper_data|0|  Ab  Cd |"},
          {"ustrunescape_data :1090",
           "display \"PREC|ustrunescape_data|\" `__rc' \"|`=__d_uni[1]'|\"",
           "PREC|ustrunescape_data|0|caf\xc3\xa9 na\xc3\xafve|"},
          {"ustrleft_data :1110",
           "display \"PREC|ustrleft_data|\" `__rc' \"|`=__d_ustrleft[1]'|\"",
           "PREC|ustrleft_data|0|caf\xc3\xa9|"},
          {"ustrregexra_data :1143",
           "display \"PREC|ustrregexra_data|\" `__rc' \"|`=__d_ustrregexra[1]'|\"",
           "PREC|ustrregexra_data|0|tea\xc3\xa9 na\xc3\xafve|"},
          {"ustrtrim_data :1163",
           "display \"PREC|ustrtrim_data|\" `__rc' \"|`=__d_ustrtrim[1]'|\"",
           "PREC|ustrtrim_data|0|caf\xc3\xa9 na\xc3\xafve|"},
          {"usubinstr_data :1174", "display \"PREC|usubinstr_data|\" `__rc' \"|`=__d_usi[1]'|\"",
           "PREC|usubinstr_data|0|  Az  cD |"},
      };
      for (const Rec& rec : recs) {
        const Reply r = run(s, rec.line);
        w2check(r.rc == "0" && r.text == rec.want,
                std::string("W2c(datafns) ") + rec.arm + " record line -> " + rec.want +
                    " -- got rc " + r.rc + " [" + r.text + "]");
      }
    }

    // BUG-718 beyond the records (no golden arm): a numeric subscript inside a
    // larger expression evaluates (x = _n - 10); an index past _N is missing
    // ([U] 13.7); `_N` is substituted first. A string subscript inside a larger
    // expression evaluates too since name-binding-display-literals: an inline
    // `=exp' is the expression evaluator's, string concatenation included
    // (tests/eval/golden/aer_w2/w2av_name_binding.log:2208
    // `PREC|bd_inl_strvarcat|0|[aab1]|`, `="a" + s'`); it was refused at rc 198.
    {
      const Reply r = run(s, "display \"`=x[1]+x[2]'|`=x[_N]'|`=x[0]'|`=__d_rev[25]'|\"");
      w2check(r.rc == "0" && r.text == "-17|10|.||",
              "W2c(datafns) BUG-718 `=x[1]+x[2]'|`=x[_N]'|`=x[0]'|`=__d_rev[25]' -> -17|10|.|| "
              "-- got rc " + r.rc + " [" + r.text + "]");
      const Reply q = run(s, "display \"`=__d_rev[1]+\"z\"'\"");
      w2check(q.rc == "0" && q.text == " Dc  bA  z",
              "W2c(datafns) BUG-718 a string subscript inside a larger inline expression "
              "evaluates (the cell ` Dc  bA  `, w2c_functions.log:639, + \"z\") -- got rc " + q.rc + " [" + q.text + "]");
      // BUG-719: an inline `: type` of a variable that does not exist is Stata's
      // own failure, not the unsupported_macro r(198) the form used to get.
      // BUG-1338: that failure prints `variable NAME not found` and expands to
      // nothing at rc 0 (StataBE 19.5: `display "P|" 7 "|`: type nosuch'|"`
      // prints the message, then `P|7||`); this leg pinned r(111) before.
      const Reply t = run(s, "display \"<`: type __w2cd_nosuch'>\"");
      w2check(t.rc == "0" && t.text == "variable __w2cd_nosuch not found<>",
              "W2c(datafns) BUG-719/BUG-1338 `: type nosuch' inline -> its message, then <> at "
              "rc 0 -- got rc " + t.rc + " [" + t.text + "]");
      // BUG-719: the plane is refreshed for the inline form. A variable generated
      // since the last `local`/`global` line was r(111) "not found" without it.
      (void)run(s, "generate str3 __w2cd_t3 = \"abc\"");
      const Reply t3 = run(s, "display \"`: type __w2cd_t3'|`: type x'\"");
      w2check(t3.rc == "0" && t3.text == "str3|float",
              "W2c(datafns) BUG-719 inline `: type' of a just-generated variable -> str3|float "
              "-- got rc " + t3.rc + " [" + t3.text + "]");
    }

    // A construct ICU and ECMAScript read differently is refused by name with
    // rc 199 (the ustrregexra rule), never answered: \w on non-ASCII text.
    {
      const Reply r = run(s, "generate __d_urw = ustrregexm(__d_uni,\"\\w+\")");
      w2check(r.rc == "199",
              "W2c(datafns) ustrregexm refuses \\w on non-ASCII text with r(199) -- got rc " +
                  r.rc);
    }

    // Outside the goal's acceptance (no golden arm): the if-condition forms of
    // the spec, through needsStringCount. s matches on all 20 rows.
    {
      const Reply c = run(s, "count if strmatch(s,\"*Ab*\")");
      const Reply n = run(s, "display r(N)");
      w2check(c.rc == "0" && n.text == "20",
              "W2c(datafns) `count if strmatch(s,\"*Ab*\")` -> 20 -- got rc " + c.rc + " " +
                  n.text);
      const Reply k = run(s, "keep if ustrregexm(s,\"Ab\")");
      const Reply m = run(s, "display _N");
      w2check(k.rc == "0" && m.text == "20",
              "W2c(datafns) `keep if ustrregexm(s,\"Ab\")` keeps 20 -- got rc " + k.rc + " " +
                  m.text);
    }
  }

  // FU-849: a string-function right-hand side replacing into an integer type
  // widens exactly as the numeric path does. The numeric twins are the oracle-
  // pinned path (w2e_float_default.log:810 replace_byte_promote: a fraction into
  // a byte makes it float); the string-RHS form has no captured line, so the
  // pin is same-binary equality. main: b stayed byte holding the double 0.1.
  {
    piet::SessionState s(workdir);
    (void)run(s, "clear");
    (void)run(s, "set obs 2");
    for (const char* v : {"b", "n", "b3", "n3"}) (void)run(s, std::string("generate byte ") + v + " = 1");
    const Reply rb = run(s, "replace b = real(\"0.1\")");
    const Reply rn = run(s, "replace n = 0.1");
    const Reply rb3 = run(s, "replace b3 = real(\"300\")");
    const Reply rn3 = run(s, "replace n3 = 300");
    const std::string tb = typeOf(s, "b"), tn = typeOf(s, "n");
    const std::string tb3 = typeOf(s, "b3"), tn3 = typeOf(s, "n3");
    const std::string xb = run(s, "display %21x b[1]").text, xn = run(s, "display %21x n[1]").text;
    const std::string xb3 = run(s, "display %21x b3[1]").text,
                      xn3 = run(s, "display %21x n3[1]").text;
    w2check(rb.rc == "0" && rn.rc == "0" && tb == tn && tn == "float" && xb == xn,
            "W2c(datafns) FU-849 `replace b = real(\"0.1\")` on a byte equals `replace n = 0.1` "
            "(float, float(0.1)) -- got " + tb + " " + xb + " vs " + tn + " " + xn + " (rc " +
                rb.rc + "/" + rn.rc + ")");
    w2check(rb3.rc == "0" && rn3.rc == "0" && tb3 == tn3 && tn3 == "int" && xb3 == xn3,
            "W2c(datafns) FU-849 `replace b3 = real(\"300\")` on a byte equals `replace n3 = 300` "
            "(int, 300) -- got " + tb3 + " " + xb3 + " vs " + tn3 + " " + xn3 + " (rc " + rb3.rc +
                "/" + rn3.rc + ")");
    // The format the widened variable shows is the numeric twin's too.
    const std::string fb = run(s, "local __w2cd_f : format b").rc == "0"
                               ? s.eval.locals.get("__w2cd_f", "<unset>")
                               : "<rc>";
    const std::string fn = run(s, "local __w2cd_f : format n").rc == "0"
                               ? s.eval.locals.get("__w2cd_f", "<unset>")
                               : "<rc>";
    w2check(fb == fn, "W2c(datafns) FU-849 widened b keeps the numeric twin's format -- got " +
                          fb + " vs " + fn);
  }
  // FU-849 under `nopromote`: the type stays and the value truncates, as the
  // numeric twin does (w2e_float_default.log:827 replace_byte_nopromote:
  // `replace rb2 = 2.7, nopromote` -> +1.0000000000000X+001|byte). The first
  // FU-849 candidate widened here regardless (b became float holding 0.1).
  {
    piet::SessionState s(workdir);
    (void)run(s, "clear");
    (void)run(s, "set obs 2");
    for (const char* v : {"pb", "pn", "qb"}) (void)run(s, std::string("generate byte ") + v + " = 1");
    const Reply rb = run(s, "replace pb = real(\"2.7\"), nopromote");
    const Reply rn = run(s, "replace pn = 2.7, nopromote");
    const Reply rq = run(s, "replace qb = real(\"0.1\"), nopromote");
    const std::string tb = typeOf(s, "pb"), tn = typeOf(s, "pn"), tq = typeOf(s, "qb");
    const std::string xb = run(s, "display %21x pb[1]").text, xn = run(s, "display %21x pn[1]").text;
    const std::string xq = run(s, "display %21x qb[1]").text;
    w2check(rb.rc == "0" && rn.rc == "0" && tb == "byte" && tn == "byte" &&
                xn == "+1.0000000000000X+001" && xb == xn,
            "W2c(datafns) FU-849 `replace pb = real(\"2.7\"), nopromote` on a byte stays byte "
            "holding 2 (:827) -- got " + tb + " " + xb + " vs " + tn + " " + xn + " (rc " + rb.rc +
                "/" + rn.rc + ")");
    w2check(rq.rc == "0" && tq == "byte" && xq == "+0.0000000000000X-3ff",
            "W2c(datafns) FU-849 `replace qb = real(\"0.1\"), nopromote` stays byte holding 0 -- "
            "got rc " + rq.rc + " " + tq + " " + xq);
  }
  // rng_rnormal / rng_uniform_alias: coordinates must reach every supported
  // row draw. These are local-stream invariants, not Stata draw-identity claims.
  for (const std::string fn : {"rnormal", "uniform"}) {
    piet::SessionState s(workdir);
    (void)run(s, "clear");
    (void)run(s, "set obs 20");
    (void)run(s, "set seed 37");
    const Reply first = run(s, "generate double ra = " + fn + "()");
    const Reply second = run(s, "generate double rb = " + fn + "()");
    const Reply different = run(s, "count if ra != rb");
    const double nDifferent = s.eval.rValues.find("N") ? *s.eval.rValues.find("N") : -1;
    (void)run(s, "set seed 37");
    const Reply repeated = run(s, "generate double rc = " + fn + "()");
    const Reply equal = run(s, "count if ra != rc");
    w2check(first.rc == "0" && second.rc == "0" && different.rc == "0" && nDifferent > 0 &&
                repeated.rc == "0" && equal.rc == "0" && s.eval.rValues.find("N") && *s.eval.rValues.find("N") == 0,
            "W2c(datafns) " + fn + ": successive calls differ and reseeding reproduces every row");
  }
  {
    piet::SessionState s(workdir);
    const std::vector<std::string> argv = {"eval"};
    for (const std::string expr : {"myrnormal()", "uniformity()", "x.uniform()", "\"uniform()\""}) {
      const auto before = s.rngStream;
      const auto injected = piet::translate::injectRngContextForExpr(argv, expr, s);
      w2check(injected == argv && s.rngStream == before,
              "W2c RNG detector: a quoted or identifier-contained draw token does not consume a stream");
    }
  }
  fs::remove_all(workdir, ec);
}
