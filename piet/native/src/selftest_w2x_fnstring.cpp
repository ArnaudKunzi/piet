// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2X selftest lane: the COMMON_GE2 string/math/matrix functions
// (goal ge2-fn-uncovered-string-math). Every arm replays a statement shape of
// the captured case w2x_fn_string_math (StataBE 19.5; capture under
// internal capture, landed as
// tests/eval/golden/aer_w2/w2x_fn_string_math.{do,log}) and names the PREC
// record it rests on. Each arm failed on the pre-change binary; the probe
// internal probe BUG-904..906.do replays the same shapes
// through `piet_native --do`.
//
// Layers: generate/replace string rows go through PIET's row evaluator
// (strmutate.cpp over exprfns.cpp's addStataFns), `display`/`local =` through
// the macro/arith layer (macros.cpp evalArith over the same registry), `if {`
// through loops.cpp's evalBoolExpr, matrix joins through handlers_m3.cpp.

#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

struct Reply {
  std::string rc = "0";
  std::string text;  // every display block's text, concatenated
};

Reply run(piet::SessionState& session, const std::string& cmd) {
  Reply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") r.rc = piet::eval::coarseRc(b);
      else if (t == "display") r.text += b.value("text", std::string());
    }
  } catch (const std::exception& e) {
    r.rc = std::string("EXC ") + e.what();
  }
  return r;
}

// One display arm: `cmd` must print exactly `want` at rc 0.
void displayArm(piet::SessionState& s, const std::string& cmd, const std::string& want,
                const std::string& label) {
  const Reply r = run(s, cmd);
  w2check(r.rc == "0" && r.text == want,
          "W2X " + label + ": `" + cmd + "` -> [" + want + "] -- got rc " + r.rc + " [" + r.text +
              "]");
}

void rcArm(piet::SessionState& s, const std::string& cmd, const std::string& rc,
           const std::string& label) {
  const Reply r = run(s, cmd);
  w2check(r.rc == rc, "W2X " + label + ": `" + cmd + "` -> r(" + rc + ") -- got rc " + r.rc);
}

}  // namespace

// BUG-905 round 3: the expression layer's missing values are StataBE's own
// doubles (exprfns.h): `.` is 2^1023, `.a` 2^1023 + 2^1011, and every finite
// value under 2^1023 is a real number -- c(maxdouble) = 8.988465674311579e+307
// included. Until this round `.` WAS c(maxdouble), so -c(maxdouble) printed
// 8.99e+307 and c(maxdouble)/2 printed 8.99e+307. Oracle rows:
// internal capture missing_range_oracle.log (mro:<line>),
// w2e_float_followups.log, pins_oracle_r.log, w2x_fn_string_math.log,
// w2_openbugs.log, and src/data/missing.h's StataBE pins.
static void testW2xMissingRepresentation() {
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2xmissrep").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState s(workdir);
  (void)run(s, "clear");

  displayArm(s, "display (-8.988465674311579e+307)", "-8.99e+307",
             "A01 -c(maxdouble) is a number (mro:73 sentinel-negated)");
  displayArm(s, "display (8.988465674311579e+307)", "8.99e+307",
             "A02 c(maxdouble) is a number (mro:67 sentinel-exact)");
  displayArm(s, "display (8.988465674311578e+307)", "8.99e+307",
             "A03 one ulp under c(maxdouble) (mro:70)");
  displayArm(s, "display (-.)", ".", "A04 -. is . (mro:58 negated-dot)");
  displayArm(s, "display (./2)", ".", "A05 ./2 is . (mro:103 dot-div-two)");
  displayArm(s, "display ((.a+1) == .)", "1", "A06 (.a+1) == . (src/data/missing.h:15)");
  displayArm(s, "display (8.988465674311579e+307/2)", "4.49e+307",
             "A07 c(maxdouble)/2 is a number (mro:67)");
  displayArm(s, "display (8.988465674311579e+307 == .) \"|\" (8.988465674311579e+307 < .)", "0|1",
             "A08 c(maxdouble) is below . (mro:67, src/data/missing.h:6)");
  (void)run(s, "scalar a09 = -8.988465674311579e+307");
  displayArm(s, "display (a09 < 0)", "1", "A09 scalar -c(maxdouble) < 0 (mro:73)");
  {
    const auto r = piet::doexec::executeDoSource(
        "local a09if 0\nif -8.988465674311579e+307 < 0 {\n    local a09if 1\n}\n", s,
        piet::dispatch);
    const std::string got = s.eval.locals.get("a09if", "?");
    w2check(!r.stoppedAtReason.has_value() && got == "1",
            "W2X A09 `if -8.988465674311579e+307 < 0 {` -> 1 (mro:73) -- got " + got +
                (r.stoppedAtReason ? " stop=" + *r.stoppedAtReason : std::string()));
  }
  (void)run(s, "scalar a10 = -1e308");
  displayArm(s, "display scalar(a10) \"|\" missing(scalar(a10)) \"|\" (a10 == .)", ".|1|1",
             "A10 scalar -1e308 is . (w2e_float_followups.log:1770)");
  displayArm(s, "display (real(\"abc\") == .) \"|\" (log(0) == .)", "1|1",
             "A13 real(\"abc\") and log(0) are . itself (w2x_fn_string_math.log:782 f4, :1065 f1; "
             "mro:82)");
  displayArm(s, "display max(8.988465674311579e+307, 1) \"|\" missing(8.988465674311579e+307)",
             "8.99e+307|0", "A16 max/missing of c(maxdouble) (mro:67, :88)");
  displayArm(s,
             "display (log(8.988465674311579e+307) < .) \"|\" (asinh(8.988465674311579e+307) < .)",
             "1|1", "A17 log/asinh of c(maxdouble) are numbers (mro:67, w2x:1065 f7)");
  {
    const auto r = piet::doexec::executeDoSource(
        "local a19 0\nif .a > . {\n    local a19 1\n}\n", s, piet::dispatch);
    const std::string got = s.eval.locals.get("a19", "?");
    w2check(!r.stoppedAtReason.has_value() && got == "1",
            "W2X A19 `if .a > . {` -> 1 (src/data/missing.h:14) -- got " + got +
                (r.stoppedAtReason ? " stop=" + *r.stoppedAtReason : std::string()));
  }
  (void)run(s, "matrix A21 = (.a, 1)");
  displayArm(s, "display A21[1,1] \"|\" (A21[1,1] + 0 == .)", ".a|1",
             "A21 a .a matrix cell reads as .a (w2_openbugs.log:53, mro:76, missing.h:15)");
  displayArm(s, "display min(-8.988465674311579e+307, 1) \"|\" missing(-8.988465674311579e+307)",
             "-8.99e+307|0", "A22 min/missing of -c(maxdouble) (mro:73, :91)");

  // Data arms: k = 1, .a, ., c(maxdouble), -c(maxdouble).
  (void)run(s, "set obs 5");
  (void)run(s, "generate double k = _n");
  (void)run(s, "replace k = .a in 2");
  (void)run(s, "replace k = . in 3");
  (void)run(s, "replace k = 8.988465674311579e+307 in 4");
  (void)run(s, "replace k = -8.988465674311579e+307 in 5");
  (void)run(s, "generate str3 s = \"x\"");
  (void)run(s, "replace s = \"abc\" in 1");
  (void)run(s, "generate double w = 0");
  (void)run(s, "generate double v = _n");
  (void)run(s, "summarize v, meanonly");
  displayArm(s, "display r(sd) \"|\" %21x r(sd)", ".|+1.0000000000000X+3ff",
             "A11 r(sd) after meanonly is . (pins_oracle_r.log:195)");
  (void)run(s, "summarize v, meanonly");
  (void)run(s, "generate double z12 = r(sd)");
  (void)run(s, "count if missing(z12)");
  displayArm(s, "display r(N)", "5", "A12 generate = r(sd) after meanonly is . (rlog:195)");
  (void)run(s, "generate str8 t14 = cond(real(s) == ., \"miss\", \"num\")");
  displayArm(s, "display \"[\" t14[1] \"]\"", "[miss]",
             "A14 cond(real(\"abc\") == ., ...) in a string row (w2x:782 f4)");
  {
    const Reply r = run(s, "generate str8 t15 = cond(log(w) == ., \"miss\", \"num\")");
    const Reply d = run(s, "display \"[\" t15[1] \"]\"");
    w2check(r.rc == "0" && d.text == "[miss]",
            "W2X A15 cond(log(0) == ., ...) in a string row -> rc 0 [miss] (w2x:1065 f1, mro:82) "
            "-- got rc " + r.rc + " " + d.text);
  }
  {
    const Reply r = run(s, "generate str5 t20 = string(cond(k == .a, 1, 2))");
    const Reply d = run(s, "display t20[1] \"|\" t20[2] \"|\" t20[3]");
    w2check(r.rc == "0" && d.text == "2|1|2",
            "W2X A20 a .a literal in a string row -> rc 0 2|1|2 (src/data/missing.h:13) -- got rc " +
                r.rc + " " + d.text);
  }
  (void)run(s, "generate str8 t23 = cond(k >= ., \"miss\", \"real\")");
  displayArm(s,
             "display t23[1] \"|\" t23[2] \"|\" t23[3] \"|\" t23[4] \"|\" t23[5]",
             "real|miss|miss|real|real",
             "A23 k >= . per row; c(maxdouble) rows are real (missing.h:14, mro:67)");
  (void)run(s, "generate double n24 = k + strlen(s) * 0");
  (void)run(s, "count if missing(n24)");
  displayArm(s, "display r(N)", "2",
             "A24 string-row arithmetic keeps +/-c(maxdouble) real (mro:67, missing.h:15)");
}

void testW2xFnString() {
  std::printf("[selftest] --- W2X string/math/matrix functions (w2x_fn_string_math.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2xfnstring").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState s(workdir);
  (void)run(s, "clear");
  (void)run(s, "set obs 2");
  (void)run(s, "generate double k = _n");
  (void)run(s, "replace k = .a in 2");
  (void)run(s, "generate str20 s = \"  Ab  cE \"");
  (void)run(s, "replace s = \"hello world foo\" in 2");

  // FU-639/FU-652: blanks inside a string literal survive generate/replace
  // (fx_fix_s [  Ab  cE ]; fx_length_gen 9).
  displayArm(s, "display \"[\" s[1] \"]\"", "[  Ab  cE ]", "fx_fix_s generate keeps a blank run");
  (void)run(s, "generate double lc = length(s)");
  displayArm(s, "display lc[1]", "9", "fx_length_gen length() of the stored blank run");

  // C5 substr (fx_substr_edges / fx_substr_neg).
  displayArm(s, "display \"[\" substr(\"abcdef\", -2, 2) \"]\"", "[ef]",
             "fx_substr_edges negative start counts from the end");
  displayArm(s, "display \"[\" substr(\"abcdef\", 0, 2) \"][\" substr(\"abcdef\", 2, -1) \"][\" "
                "substr(\"abcdef\", -9, 3) \"]\"",
             "[][][]", "fx_substr_edges start 0, negative length, start before the first");
  // C8, C7, C9, C10, C16.
  displayArm(s, "display strpos(\"\", \"\")", "0", "fx_strpos_edges strpos(\"\",\"\")");
  displayArm(s, "display \"[\" subinstr(\"abcb\", \"b\", \"x\", -1) \"]\"", "[abcb]",
             "fx_subinstr_negn a negative count replaces nothing");
  displayArm(s, "display length(\"\xc3\xa9\")", "2", "fx_length_edges length() counts bytes");
  displayArm(s, "display length(strtrim(char(9) + \" a \" + char(9)))", "5",
             "fx_trim_edges strtrim strips blanks only");
  displayArm(s, "display \"[\" word(\"a b c d\", -1) \"]\"", "[d]",
             "fx_word_edges a negative n counts from the end");
  // C4 real() keeps the extended kind.
  displayArm(s, "display real(\".a\")", ".a", "fx_real_edges real(\".a\")");

  // C11 math (fx_log_edges, fx_log10_edges, fx_exp_edges, fx_asinh_edges).
  displayArm(s, "display log(.) \"|\" asinh(.) \"|\" exp(.) \"|\" exp(710)", ".|.|.|.",
             "fx_log/asinh/exp_edges a missing argument or an overflow is missing");
  displayArm(s, "display %21x log10(1000)", "+1.7ffffffffffffX+001",
             "fx_log10_edges log10 is ln(x)/ln(10)");
  displayArm(s, "display %21x asinh(1e-10)", "+1.b7cdffffa18d8X-022",
             "fx_asinh_edges asinh is ln(x+sqrt(x^2+1))");
  displayArm(s, "display exp(-745)", "0", "fx_exp_edges exp(-745) underflows to 0");

  // C12/C13 missing() with n mixed arguments; inlist() with one argument.
  displayArm(s, "display missing(1, \"a\", 2, 3) \"|\" missing(., \"a\", 2, 3) \"|\" missing(1, \"\")",
             "0|1|1",
             "fx_missing_edges n arguments, string and numeric");
  (void)run(s, "count if missing(k, s)");
  displayArm(s, "display r(N)", "1", "fx_missing_count4 count if missing(k, s)");
  rcArm(s, "count if inlist(k)", "198", "fx_inlist_one inlist() with one argument");

  // C14/C15 string() default %9.0g and the f-format overflow.
  displayArm(s, "display string(.00001) \"|\" string(1e-6) \"|\" string(123456789)",
             ".00001|1.00e-06|1.23e+08", "fx_string1_edges string(x) is %9.0g (FU-616)");
  displayArm(s, "display string(0.5, \"%03.2f\") \"|\" string(1234.5, \"%5.2fc\")", "0.50|1234.50",
             "fx_string2_fmts an f overflow widens when e is no shorter");
  // C4 string(k) of a `.a` cell in generate.
  (void)run(s, "generate str4 sk = string(k)");
  displayArm(s, "display \"[\" sk[2] \"]\"", "[.a]", "fx_string1_cond string() of a .a cell");
  // BUG-907: arithmetic on a missing is plain `.` -- the kind is lost, and
  // `./2`, `.*0`, `-.` are `.` (StataBE 19.5 pin in src/data/missing.h:
  // `(.a + 1) -> .`, `((.a+1) == .) -> 1`; tests/unit/test_missing.cpp arm 4).
  // The row evaluator binds the `.a` cell with its kind (the arm above), so its
  // arithmetic must collapse it (pyeval.cpp evalBinNumeric / unary minus).
  (void)run(s, "generate str8 ska = string(k + 1) + \"|\" + string(-k) + \"|\" + string(k / 2) + "
               "\"|\" + string(k * 0)");
  displayArm(s, "display \"[\" ska[2] \"]\"", "[.|.|.|.]",
             "BUG-907 string(k+1), string(-k), string(k/2), string(k*0) of a .a cell");
  (void)run(s, "generate str5 skc = cond(k + 1 == . & -k == ., \"sys\", \"other\")");
  displayArm(s, "display \"[\" skc[2] \"]\"", "[sys]",
             "BUG-907 cond(k + 1 == . & -k == ., ...) of a .a cell");
  (void)run(s, "scalar sc907 = real(\".a\") - 0");
  displayArm(s, "display (sc907 == .) \"|\" (.a + 1 == .) \"|\" (-.a == .) \"|\" ./2",
             "1|1|1|.", "BUG-907 scalar/display arithmetic on .a is .");

  // C6 str# truncation and replace promotion.
  (void)run(s, "generate str3 up3 = upper(s)");
  displayArm(s, "display \"[\" up3[2] \"]\"", "[HEL]", "fx_upper_str3 str3 truncates");
  (void)run(s, "generate str2 sip = \"ab\"");
  (void)run(s, "replace sip = subinstr(s, \" \", \"__\", .)");
  (void)run(s, "local sipt : type sip");
  displayArm(s, "display \"`sipt'\"", "str17", "fx_subinstr_rep replace promotes str2 to str17");

  // C17 inline extended macro functions.
  (void)run(s, "local m_slt = strltrim(\"`: display %9.3f 0.25'\")");
  displayArm(s, "display \"[`m_slt']\"", "[0.250]", "fx_strltrim_macro inline `: display'");

  // C18 colnumb().
  (void)run(s, "matrix CB = (1, 2, 3, 4)");
  (void)run(s, "matrix colnames CB = frac_bound2 b c.equity#c.x _cons");
  displayArm(s, "display colnumb(CB, \"b\") \"|\" colnumb(CB, \"B\") \"|\" colnumb(CB, \"\")",
             "2|.|1", "fx_colnumb_disp/_empty colnumb() reads its literal");
  (void)run(s, "local m_cnf = colnumb(CB, \"c.equity#c.x\")");
  displayArm(s, "display \"[`m_cnf']\"", "[3]", "fx_colnumb_fv local = colnumb()");
  rcArm(s, "display colnumb(NOSUCHM, \"a\")", "111", "fx_colnumb_nomat absent matrix");
  {
    // fx_colnumb_if: the `if` block reads colnumb() (loops.cpp evalBoolExpr).
    const auto r = piet::doexec::executeDoSource(
        "local m_h1 0\nlocal m_h2 0\n"
        "if !missing(colnumb(CB, \"zz\")) {\n    local m_h1 1\n}\n"
        "if !missing(colnumb(CB, \"_cons\")) {\n    local m_h2 1\n}\n",
        s, piet::dispatch);
    const std::string got = s.eval.locals.get("m_h1", "?") + "|" + s.eval.locals.get("m_h2", "?");
    w2check(!r.stoppedAtReason.has_value() && got == "0|1",
            "W2X fx_colnumb_if `if !missing(colnumb(CB, name)) {` -> 0|1 -- got " + got +
                (r.stoppedAtReason ? " stop=" + *r.stoppedAtReason : std::string()));
  }

  // C19 nullmat() joins.
  for (int i = 1; i <= 3; ++i) {
    (void)run(s, "matrix NB = nullmat(NB) \\ (" + std::to_string(i) + ", " +
                     std::to_string(10 * i) + ")");
  }
  (void)run(s, "local nbr : rownames NB");
  // fx_nullmat_colloop rides along: a stored matrix keeps its own names (c1 c1 c1).
  (void)run(s, "matrix tmp = (1.5)");
  for (int i = 1; i <= 3; ++i) (void)run(s, "matrix NC = nullmat(NC), tmp");
  (void)run(s, "local ncc : colnames NC");
  displayArm(s, "display \"[`nbr'][`ncc']\"", "[r1 r2 r3][c1 c1 c1]",
             "fx_nullmat_rowloop/_colloop inline rows take positional names, stored ones keep theirs");
  rcArm(s, "matrix NX = nullmat(NOSUCH2)", "111", "fx_nullmat_bare nullmat() outside a join");
  (void)run(s, "local plist \"10 50 90\"");
  for (int i = 1; i <= 3; ++i) {
    (void)run(s, "local i " + std::to_string(i));
    (void)run(s, "matrix NP = nullmat(NP) \\ `: word `i' of `plist''");
  }
  displayArm(s, "display rowsof(NP) \"|\" NP[3,1]", "3|90",
             "fx_nullmat_macro inline `: word i of list'");

  testW2xMissingRepresentation();
}
