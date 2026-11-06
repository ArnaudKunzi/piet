// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2D-T03 (2026-10-12) -- one display format path, pinned arm by arm.
//
// Commit 1 (BUG-444, FU-57, FU-53): the shared renderer piet/stata_format.h
// learned Stata's `%21x` hexadecimal float, both format grammars admit it
// (display_hardening.cpp applyStataFmtLocal, eval/exprfns.cpp applyStataFmt),
// and `%w.0g` renders small magnitudes and three-digit exponents the way Stata
// does. Every arm cites the golden record it rests on and fails on frozen main
// 3f4c4fd0 (which prints `%21x` as a decimal and 1e-5/3 at %18.0g as
// .0000033333333333) unless it is marked CONTROL.
//
// The hex arms walk every display ITEM KIND the spec names -- a literal
// expression, a scalar, a local's text, a matrix element and a row-evaluator
// cell -- because the display layer hands the renderer TEXT and only an exact
// round trip of that text gives the stored double back. Each kind's hex is
// compared with hexFloat() of the double the fixture stored, so a lossy text
// on any one path fails its own arm.
//
// Commit 2 (BUG-554, FU-45, FU-88): display's `_b[]`/`_se[]`/`e()` resolvers
// returned %.9g TEXT, so `display %18.0g _b[a]` printed `.484848485` and
// `%21x _b[a]` a decimal. A bare formatted item now renders the stored double;
// `e(NAME)[i,j]` reads the posted e-matrix instead of echoing its source text;
// the no-estimates and unknown-coefficient refusals carry r(111) instead of
// rc 1. The BUG-554 fixture's regress lands 1 ulp from StataBE's
// `_b[a]` (PIET 0x1.f07c1f07c1f06p-2, Stata ...f07): a stored-VALUE difference
// owned by the estimation lane, so the %21x arms pin the hex against the
// scalar route and the golden's first twelve hex digits, never the last one.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gDisplayFormatChecks = 0;
void dfCheck(bool cond, const std::string& label) {
  ++gDisplayFormatChecks;
  w2check(cond, label);
}

// rc of one dispatched line (the last error block's coarse rc; "0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC:") + e.what();
  }
  return rc;
}

// The message of the last error block of one line ("" if none).
std::string errOf(piet::SessionState& session, const std::string& cmd) {
  std::string msg;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (b.is_object() && b.value("type", std::string()) == "error")
        msg = b.value("message", std::string());
    }
  } catch (const std::exception& e) {
    msg = std::string("EXC:") + e.what();
  }
  return msg;
}

// Text of one `display ...` line (empty when it errored or displayed nothing).
std::string shown(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
  } catch (const std::exception&) {
  }
  return text;
}

void testHexRenderer() {
  using piet::fmt::hexFloat;
  // Renderer-level pins, every one a golden field.
  dfCheck(hexFloat(3.0) == "+1.8000000000000X+001",
          "W2D-T03 hex: 3 is +1.8000000000000X+001 (w2f_residuals.log:1339 round_local)");
  dfCheck(hexFloat(-2.0) == "-1.0000000000000X+001" && hexFloat(2.68) == "+1.570a3d70a3d71X+001" &&
              hexFloat(0.33) == "+1.51eb851eb851fX-002",
          "W2D-T03 hex: -2, 2.68, .33 (w2f_residuals.log:1303 round_matrix_row)");
  dfCheck(hexFloat(1.0 / 3.0) == "+1.5555555555555X-002",
          "W2D-T03 hex: 1/3 (w2e_float_default.log:124 gen_double_13)");
  dfCheck(hexFloat(0.0) == "+0.0000000000000X-3ff",
          "W2D-T03 hex: zero is +0.0000000000000X-3ff (w2g_ebmat.log:481 e2_el_eb_12)");
  dfCheck(hexFloat(piet::fmt::stataMissingDouble(0)) == "+1.0000000000000X+3ff",
          "W2D-T03 hex: `.` is +1.0000000000000X+3ff (w2_openbugs.log:45 lit_dot_cells)");
  dfCheck(hexFloat(piet::fmt::stataMissingDouble(1)) == "+1.0010000000000X+3ff" &&
              hexFloat(piet::fmt::stataMissingDouble(26)) == "+1.01a0000000000X+3ff",
          "W2D-T03 hex: `.a`/`.z` at the dta stride (w2i_varsub.log:161, "
          "aer_b4/esttab-beta.log:100)");

  // string()/tostring grammar: the one changed regex line in exprfns.cpp.
  dfCheck(piet::eval::applyStataFmt(722.0, "%21x") == "+1.6900000000000X+009",
          "W2D-T03 hex: string(722, \"%21x\") is +1.6900000000000X+009 (BUG-444.do, "
          "b3_funcs_oracle) -- main returned 722");
  // CONTROLS: the unpinned spellings keep main's fall-through (gFormat).
  dfCheck(piet::eval::applyStataFmt(3.0, "%-21x") == "3" &&
              piet::eval::applyStataFmt(3.0, "%021x") == "3" &&
              piet::eval::applyStataFmt(3.0, "%20x") == "3" &&
              piet::eval::applyStataFmt(3.0, "%9.2x") == "3",
          "W2D-T03 hex CONTROL: %-21x/%021x/%20x/%9.2x stay unclaimed in string() (no golden)");
  dfCheck(piet::eval::applyStataFmt(42.5, "%9.2f") == "42.50" &&
              piet::eval::applyStataFmt(1234567.89, "%9.2f") == "1.23e+06" &&
              piet::eval::applyStataFmt(42.0, "%9.0e") == "4.20e+01" &&
              piet::eval::applyStataFmt(1234567.89, "%12,0fc") == "1.234.568",
          "W2D-T03 CONTROL: the regex change leaves f/e/separator specs byte-identical "
          "(fmt_g_oracle, wsr_oracle rows)");
}

void testGeneralSmallMagnitude() {
  using piet::eval::applyStataFmt;
  const double third5 = 1e-5 / 3.0;
  dfCheck(applyStataFmt(third5, "%18.0g") == "3.33333333333e-06" &&
              applyStataFmt(third5, "%16.0g") == "3.333333333e-06" &&
              applyStataFmt(third5, "%17.0g") == "3.3333333333e-06",
          "W2D-T03 %w.0g: 1e-5/3 is scientific at %16/%17/%18.0g (w2f_residuals.log:428 lt_17) "
          "-- main printed .0000033333333333");
  dfCheck(applyStataFmt(-third5, "%18.0g") == "-3.33333333333e-06",
          "W2D-T03 %w.0g: -1e-5/3 (w2f_residuals.log:538 lt_22)");
  dfCheck(applyStataFmt(1e-300, "%16.0g") == "1.00000000e-300" &&
              applyStataFmt(1e-300, "%17.0g") == "1.000000000e-300" &&
              applyStataFmt(1e-300, "%18.0g") == "1.0000000000e-300",
          "W2D-T03 %w.0g: a 3-digit exponent spends one mantissa digit (w2f_residuals.log:802 "
          "lt_34) -- main printed 1.000000000e-300 at %16.0g");
  dfCheck(applyStataFmt(5e-324, "%16.0g") == "4.94065646e-324" &&
              applyStataFmt(5e-324, "%18.0g") == "4.9406564584e-324",
          "W2D-T03 %w.0g: the subnormal 5e-324 (w2f_residuals.log:824 lt_35)");
  // CONTROLS: the tie case stays fixed, as do the fmt_g_oracle rows.
  dfCheck(applyStataFmt(1e-4 / 3.0, "%18.0g") == ".0000333333333333" &&
              applyStataFmt(1e-4 / 3.0, "%16.0g") == ".00003333333333",
          "W2D-T03 %w.0g CONTROL: 1e-4/3 stays fixed (w2f_residuals.log:406 lt_16)");
  dfCheck(applyStataFmt(1e16, "%18.0g") == "1.00000000000e+16",
          "W2D-T03 %w.0g CONTROL: 1e16 (w2f_residuals.log:626 lt_26, :1015 nw_01; already "
          "right on main)");
  dfCheck(applyStataFmt(0.000012345, "%9.0g") == ".0000123" &&
              applyStataFmt(0.000012345, "%8.0g") == ".000012" &&
              applyStataFmt(0.000012345, "%20.0g") == ".000012345" &&
              applyStataFmt(-0.000012345, "%9.0g") == "-.0000123",
          "W2D-T03 %w.0g CONTROL: .000012345 stays fixed (fmt_g_oracle.log:150/188/283, "
          "fmt_g_oracle_neg.log:104)");
}

void testDisplayHexItemKinds(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  const std::string third = piet::fmt::hexFloat(1.0 / 3.0);

  // A literal expression (evalArith returns repr).
  dfCheck(shown(session, "display %21x 3") == "+1.8000000000000X+001",
          "W2D-T03 display: `display %21x 3` (w2f_residuals.log:1339) -- main printed 3");
  dfCheck(shown(session, "display %21x 1/3") == third,
          "W2D-T03 display: a literal expression keeps every bit (1/3)");
  // A local's text. The golden builds it as `local r = round(2.5)`; PIET's
  // round() of a .5 tie is a value defect of its own (it gives 2), so the
  // local holds the value Stata stored and the arm measures display alone.
  (void)piet::dispatch("local r = 3", session);
  dfCheck(shown(session, "display %21x `r'") == "+1.8000000000000X+001",
          "W2D-T03 display: a local's text (w2f_residuals.log:1339 round_local)");
  (void)piet::dispatch("local t3 = 1/3", session);
  dfCheck(shown(session, "display %21x `t3'") == third,
          "W2D-T03 display: a local holding 1/3 round-trips bit-exact");
  // A scalar.
  (void)piet::dispatch("scalar s3 = 1/3", session);
  dfCheck(shown(session, "display %21x s3") == third,
          "W2D-T03 display: a scalar round-trips bit-exact");
  // A matrix element, the golden's row of values (round() itself is not this
  // task's: the literals are the values Stata stored, w2f_residuals.log:1303).
  (void)piet::dispatch("matrix XR = (3, -2, 1, 2, 2.68, .33)", session);
  dfCheck(shown(session, "display %21x XR[1,1] \"|\" %21x XR[1,2] \"|\" %21x XR[1,5] \"|\" %21x XR[1,6]") ==
              "+1.8000000000000X+001|-1.0000000000000X+001|+1.570a3d70a3d71X+001|+1.51eb851eb851fX-002",
          "W2D-T03 display: matrix elements (w2f_residuals.log:1303 round_matrix_row)");
  // A matrix holding a missing value.
  (void)piet::dispatch("matrix MD = (1.5, 2.5 \\ ., . \\ 4.5, 5.5)", session);
  dfCheck(shown(session, "display %21x MD[1,1] \"|\" %21x MD[2,1] \"|\" %21x MD[3,2]") ==
              "+1.8000000000000X+000|+1.0000000000000X+3ff|+1.6000000000000X+002",
          "W2D-T03 display: matrix cells incl. `.` (w2_openbugs.log:45 lit_dot_cells)");
  dfCheck(shown(session, "display %21x 0") == "+0.0000000000000X-3ff",
          "W2D-T03 display: zero (w2g_ebmat.log:481 e2_el_eb_12)");
  dfCheck(shown(session, "display %21x .a") == "+1.0010000000000X+3ff",
          "W2D-T03 display: `.a` (w2i_varsub.log:161 a_03)");
  // A row-evaluator cell (w2e_float_default.do:71).
  (void)piet::dispatch("set obs 3", session);
  (void)piet::dispatch("generate double g_d13 = 1/3", session);
  dfCheck(shown(session, "display %21x g_d13[1]") == "+1.5555555555555X-002",
          "W2D-T03 display: a variable cell (w2e_float_default.log:124 gen_double_13)");
  // string() through display.
  dfCheck(shown(session, "display string(722, \"%21x\")") == "+1.6900000000000X+009",
          "W2D-T03 display: string(722, \"%21x\") (BUG-444.do)");
  // %w.0g through display: right-justified into the field.
  dfCheck(shown(session, "display %18.0g 1e-5/3") == " 3.33333333333e-06",
          "W2D-T03 display: %18.0g 1e-5/3 (w2f_residuals.log:881 gt_03) -- main printed "
          ".0000033333333333");
  // CONTROLS.
  // display's own %21x renderer (display_hardening.cpp renderHex21, landed on
  // main as 24532f48) claims the left-justified spelling too; a 21-wide field
  // has nothing to justify, so it is the same text. No golden pins `%-21x`.
  dfCheck(shown(session, "display %-21x 3") == "+1.8000000000000X+001",
          "W2D-T03 display CONTROL: %-21x prints the same 21 characters as %21x (no golden)");
  // BUG-1345: a width other than 21 is no format; it fell through and printed `3`.
  dfCheck(rcOf(session, "display %20x 3") == "120",
          "W2D-T03 display CONTROL: a width other than 21 is r(120) invalid %format (StataBE "
          "19.5, 2026-11-03)");
  // BUG-1016: the unformatted default keeps eight significant digits once the
  // leading zero is dropped (`.45833333`, w2i_varsub.log:85 j_01; `.02559116`,
  // w2g_ebmat.log:1728). Main printed `.3333333`.
  dfCheck(shown(session, "display 1/3") == ".33333333" &&
              shown(session, "display %9.2f 1/3") == "     0.33" &&
              shown(session, "display %10.3e 1/3") == " 3.333e-01",
          "W2D-T03 display: default is `.33333333` (BUG-1016, w2i_varsub.log:85); f and e "
          "renderings unchanged (CONTROL). Got `" + shown(session, "display 1/3") + "`");
}

// The BUG-554 probe's fixture == w2_cmdtriage.do's data, after_restore model.
void bug554Fixture(piet::SessionState& session) {
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 12", session);
  (void)piet::dispatch("generate double a = _n*1.5", session);
  (void)piet::dispatch("generate double b = mod(_n*5, 7)", session);
  (void)piet::dispatch("generate double y = 1 + 0.5*a - 0.25*b + mod(_n, 4)", session);
  (void)piet::dispatch("quietly regress y a", session);
}

void testEstimateItemsHonourFormat(const std::string& workdir) {
  piet::SessionState session(workdir);
  bug554Fixture(session);
  // Oracle w2_cmdtriage.log:376 after_restore: %18.0g _b[a] -> " .4848484848484848".
  dfCheck(shown(session, "display %18.0g _b[a]") == " .4848484848484848",
          "W2D-T03 BUG-554: `display %18.0g _b[a]` is \" .4848484848484848\" (w2_cmdtriage.log:376 "
          "after_restore) -- main printed `.484848485`. Got `" +
              shown(session, "display %18.0g _b[a]") + "`");
  // %21x: Stata +1.f07c1f07c1f07X-002 (same line). The display half is the
  // hex FORM at full precision; the last digit is the stored value's.
  {
    const std::string hx = shown(session, "display %21x _b[a]");
    dfCheck(hx.rfind("+1.f07c1f07c1f0", 0) == 0 && hx.size() == 21,
            "W2D-T03 BUG-554: `display %21x _b[a]` is a 21-char hex float agreeing with "
            "+1.f07c1f07c1f07X-002 (w2_cmdtriage.log:376) to 12 hex digits -- main printed "
            "0.484848485. Got `" + hx + "`");
  }
  // FU-45: each bare item prints the same text as the same value in a scalar.
  struct Item {
    const char* expr;
    const char* name;
  };
  static const Item kItems[] = {{"_b[a]", "fq_b"},   {"_se[a]", "fq_se"},  {"e(r2)", "fq_r2"},
                                {"e(rmse)", "fq_rm"}, {"_b[_cons]", "fq_c"}, {"e(N)", "fq_n"}};
  for (const auto& it : kItems) {
    (void)piet::dispatch(std::string("scalar ") + it.name + " = " + it.expr, session);
    for (const char* f : {"%18.0g", "%21x"}) {
      const std::string direct = shown(session, std::string("display ") + f + " " + it.expr);
      const std::string viaScalar = shown(session, std::string("display ") + f + " " + it.name);
      dfCheck(!direct.empty() && direct == viaScalar,
              std::string("W2D-T03 FU-45: `display ") + f + " " + it.expr +
                  "` prints what the same value held in a scalar prints. Got `" + direct +
                  "` vs `" + viaScalar + "`");
    }
  }
  // FU-45, review round 1: a string format on the item prints the number as an
  // unformatted display does, and the same text as the scalar -- an
  // integer-valued e(N) is `12`, never the repr `12.0` the first cut of the
  // stored-double route printed. BUG-1345: `%20x` and `%21X` are no formats
  // (StataBE 19.5, 2026-11-03: r(120) `invalid %format` for _b[a], e(N) and a
  // scalar alike; `%9s _b[a]` .48484848, `%9s e(N)` 12). These arms held the
  // item's text for them too, which was PIET's own fall-through.
  for (const auto& it : kItems) {
    const std::string direct = shown(session, std::string("display %9s ") + it.expr);
    const std::string viaScalar = shown(session, std::string("display %9s ") + it.name);
    const bool reprTail = direct.size() >= 2 && direct.compare(direct.size() - 2, 2, ".0") == 0;
    dfCheck(!direct.empty() && direct == viaScalar && !reprTail,
            std::string("W2D-T03 FU-45: `display %9s ") + it.expr +
                "` prints the scalar's text, no repr `.0`. Got `" + direct + "` vs `" + viaScalar +
                "`");
    for (const char* f : {"%20x", "%21X"}) {
      const std::string rcDirect = rcOf(session, std::string("display ") + f + " " + it.expr);
      const std::string rcScalar = rcOf(session, std::string("display ") + f + " " + it.name);
      dfCheck(rcDirect == "120" && rcScalar == "120",
              std::string("W2D-T03 FU-45: `display ") + f + " " + it.expr +
                  "` is r(120), as through the scalar. Got " + rcDirect + " and " + rcScalar);
    }
  }
  dfCheck(shown(session, "display _b[a] \"|\" %9s _b[a]") == ".48484848|.48484848",
          "W2D-T03 FU-45: `%9s _b[a]` is the unformatted .48484848 (StataBE 19.5). Got `" +
              shown(session, "display _b[a] \"|\" %9s _b[a]") + "`");
  dfCheck(shown(session, "display %21x e(N) \"|\" %20x e(N)") ==
                  "+1.8000000000000X+003|invalid %format" &&
              rcOf(session, "display %21x e(N) \"|\" %20x e(N)") == "120",
          "W2D-T03 FU-45: `display %21x e(N) \"|\" %20x e(N)` prints +1.8000000000000X+003|"
          "invalid %format and is r(120) (StataBE 19.5). Got `" +
              shown(session, "display %21x e(N) \"|\" %20x e(N)") + "`");
  // An unformatted item and an expression on one print StataBE's default:
  // eight significant digits after the leading zero is dropped (BUG-1016;
  // `display _b[ATE:r1vs0.t]` -> `.02559116`, w2g_ebmat.log:1728, and
  // `-.22631858`, :1807). Frozen main 3f4c4fd0 printed seven (`.4848485`).
  // CONTROLS: e(N) and the f/e formats are byte-identical to main.
  dfCheck(shown(session, "display _b[a]") == ".48484848" &&
              shown(session, "display _se[a]") == ".08853079" &&
              shown(session, "display e(r2)") == ".74995775" &&
              shown(session, "display e(N)") == "12" &&
              shown(session, "display _b[a]*2") == ".96969697" &&
              shown(session, "display \"[\" _b[a] \"]\"") == "[.48484848]" &&
              shown(session, "display %9.2f _b[a]") == "     0.48" &&
              shown(session, "display %10.3e _se[a]") == " 8.853e-02",
          "W2D-T03 BUG-554/BUG-1016: unformatted _b[]/_se[]/e() display and an expression on "
          "_b[] print eight digits (w2g_ebmat.log:1728); e(N) and f/e formats unchanged. Got `" +
              shown(session, "display _b[a]") + "`");

  // Sibling of FU-88 in the same resolver: an unknown coefficient is r(111)
  // "[nosuch] not found" (w2g_ebmat.log:1689-1694 e15a_disp_bx1 111; also
  // e15a_oaxaca_disp_bx1, e15c_disp_bx1, e15d_disp_bx1). Main: rc 1.
  dfCheck(rcOf(session, "display _b[nosuch]") == "111" &&
              errOf(session, "display _b[nosuch]") == "[nosuch] not found" &&
              rcOf(session, "display %21x _se[nosuch]") == "111",
          "W2D-T03: `display _b[nosuch]` is r(111) \"[nosuch] not found\" (w2g_ebmat.log:1690 "
          "e15a_disp_bx1) -- main rc 1");

  // e(b)[i,j] after a fit that never `ereturn post`ed its e(b): the fit's own
  // e(b)/e(V), as Stata answers (BUG-1350, StataBE `display e(b)[1,2]` after
  // regress), never the literal source text main printed at rc 0.
  {
    const std::string rc = rcOf(session, "display %18.0g e(b)[1,1]");
    const std::string b = shown(session, "display %18.0g e(b)[1,1]");
    const std::string txt = shown(session, "display e(V)[1,1]");
    dfCheck(rc == "0" && b == shown(session, "display %18.0g _b[a]") &&
                rcOf(session, "display e(V)[1,1]") == "0" &&
                txt.find("e(V)[1,1]") == std::string::npos,
            "W2D-T03 FU-45 / BUG-1350: e(b)[1,1] of a fit is its first coefficient at rc 0 and "
            "e(V)[1,1] answers, never echoed as source text. Got rc " + rc + " `" + b + "` `" +
                txt + "`");
  }
}

void testPostedEMatrixElement(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  piet::eval::EvalMatrix b;
  b.rownames = {"y1"};
  b.colnames = {"x", "_cons"};
  b.data = {{0.1, 1.0 / 3.0}};
  session.eval.eMatrices.emplace_back("b", b);
  piet::eval::EvalMatrix v;
  v.rownames = v.colnames = {"x", "_cons"};
  v.data = {{2.0 / 3.0, -0.25}, {-0.25, 0.5}};
  session.eval.eMatrices.emplace_back("V", v);
  dfCheck(shown(session, "display %18.0g e(b)[1,2]") == " .3333333333333333" &&
              shown(session, "display %21x e(b)[1,2]") == piet::fmt::hexFloat(1.0 / 3.0) &&
              shown(session, "display %21x e(V)[1,1]") == piet::fmt::hexFloat(2.0 / 3.0) &&
              shown(session, "display %18.0g e(V)[2,1]") == "              -.25",
          "W2D-T03 FU-45: `display %18.0g e(b)[1,2]` / `%21x e(V)[1,1]` render the posted "
          "element's stored value (main: the source text e(b)[1,2])");
  dfCheck(shown(session, "display e(b)[1,1]") == ".1",
          "W2D-T03 FU-45 CONTROL: unformatted `display e(b)[1,1]` of a posted matrix still takes "
          "the default %9.0g (the macro layer already answered it on main)");
  dfCheck(rcOf(session, "display %18.0g e(b)[1,3]") == "503" &&
              rcOf(session, "display e(V)[3,1]") == "503",
          "W2D-T03 FU-45: an out-of-range e-matrix subscript is r(503) conformability error "
          "(unpinned: no golden displays one)");
  // Posted e-results with no fitted model: r(111) "_b not allowed when e(b) is
  // not present" (w2g_ebmat.log:2489-2495 e15d_disp_bx1_2) -- reading a posted
  // e(b) through _b[] is w2d-t01's, not claimed here.
  dfCheck(rcOf(session, "display _b[x]") == "111" &&
              errOf(session, "display _b[x]") == "_b not allowed when e(b) is not present",
          "W2D-T03: _b[] with e-results but no fitted model is r(111) (w2g_ebmat.log:2489 "
          "e15d_disp_bx1_2) -- main rc 1");
}

void testNoEstimatesIs111(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  // FU-88 (corrected toward the oracle 2026-10-12): with no e(b), the display
  // read is r(111) "_b not allowed when e(b) is not present" (w2g_ebmat.log:
  // 2099-2105 e15c_disp_bx1, :2181-2187 e15c_disp_bx1_times2). r(301) belongs
  // to `matrix b = e(b)` / `matlist e(b)` (:2223-2238). Frozen main: rc 1.
  // Not claimed here: `display _b[x]*2` (an expression, :2181-2187 is r(111))
  // still answers `.` at rc 0 through the macro layer's resolveBRef, a named
  // divergence in eval/macros.cpp outside this task's files (follow-up).
  dfCheck(rcOf(session, "display _b[x]") == "111" &&
              errOf(session, "display _b[x]") == "_b not allowed when e(b) is not present" &&
              rcOf(session, "display %18.0g _se[x]") == "111",
          "W2D-T03 FU-88: `display _b[x]` with no estimates is r(111) \"_b not allowed when "
          "e(b) is not present\" (w2g_ebmat.log:2099 e15c_disp_bx1) -- frozen main rc 1");
  (void)piet::dispatch("capture noisily display _b[x]", session);
  dfCheck(shown(session, "display _rc") == "111",
          "W2D-T03 FU-88: after `capture noisily display _b[x]`, `display _rc` is 111, never 1");
}

// ---------------------------------------------------------------------------
// display-adjacent-expr-echo (FU-142, FU-141, FU-54, FU-855, FU-201, FU-349,
// FU-853; BUG-816..818). Adjacent parenthesised groups in one display segment
// are evaluated one by one, and an item that is an expression and cannot be
// evaluated is a named refusal, never its source text at rc 0. Display layer
// only (display_hardening.cpp). Unless marked CONTROL, every arm fails on
// frozen main e5b059a4, which printed the source text at rc 0 (or the rc named).

// The w2f_residuals lt_* record: three adjacent string comparisons.
std::string ltRecord(const std::string& arm, const std::string& src, const std::string& l,
                     const std::string& s16, const std::string& s17, const std::string& s18) {
  const auto eq = [&](const std::string& b) { return "(\"" + l + "\"==\"" + b + "\")"; };
  return "display \"PREC|" + arm + "|\" 0 \"|" + src + "|" + l + "|" + s16 + "|" + s17 + "|" + s18 +
         "|\" " + eq(s16) + " " + eq(s17) + " " + eq(s18) + " \"|\"";
}

void testAdjacentGroups(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  // BUG-816 / FU-142: the golden lines, byte for byte.
  dfCheck(shown(session, ltRecord("lt_01", "1/3", ".3333333333333333", ".33333333333333",
                                  ".333333333333333", ".3333333333333333")) ==
              "PREC|lt_01|0|1/3|.3333333333333333|.33333333333333|.333333333333333|"
              ".3333333333333333|001|",
          "BUG-816: lt_01 ends |001| (w2f_residuals.log:76) -- main echoed the three comparisons");
  dfCheck(shown(session, ltRecord("lt_24", "0.1+0.2", ".3", ".3", ".3", ".3")) ==
              "PREC|lt_24|0|0.1+0.2|.3|.3|.3|.3|111|",
          "BUG-816: lt_24 ends |111| (w2f_residuals.log:582)");
  dfCheck(shown(session, ltRecord("lt_35", "5e-324", "4.9406564584e-324", "4.94065646e-324",
                                  "4.940656458e-324", "4.9406564584e-324")) ==
              "PREC|lt_35|0|5e-324|4.9406564584e-324|4.94065646e-324|4.940656458e-324|"
              "4.9406564584e-324|001|",
          "BUG-816: lt_35 ends |001| (w2f_residuals.log:824)");
  dfCheck(shown(session,
                "display \"PREC|gt_01|0|1/3|.3333333333333333|.3333333333333333|\" "
                "(\".3333333333333333\"==\".3333333333333333\") \"|\"") ==
              "PREC|gt_01|0|1/3|.3333333333333333|.3333333333333333|1|",
          "BUG-816 CONTROL: a lone comparison already printed 1 (w2f_residuals.log:837 gt_01)");
  dfCheck(shown(session, "display (\"a\"==\"b\") (\"a\"==\"a\")") == "01",
          "BUG-816: `display (\"a\"==\"b\") (\"a\"==\"a\")` prints 01 (the lt_* shape)");
  {
    const std::string rc = rcOf(session, "display (\"a\"==\"b\")(\"a\"==\"a\")");
    dfCheck(rc == "0" && shown(session, "display (\"a\"==\"b\")(\"a\"==\"a\")") == "01",
            "BUG-816: groups with no space between them split too, 01 -- main r(198). Got rc " + rc);
  }
  dfCheck(shown(session, "display (\"(\" == \")\") (\"a)\" == \"a)\")") == "01" &&
              shown(session, "display ((1+2)*(3)) (4)") == "94",
          "BUG-816: the split respects a parenthesis inside quotes and nested groups (01, 94)");
  dfCheck(shown(session, "display %5.2f (1/3) (2/3)") ==
              shown(session, "display %5.2f (1/3)") + shown(session, "display (2/3)"),
          "BUG-816: a pending %fmt formats the first group, the next takes the default -- "
          "the same text as the two items displayed apart");
  {
    const std::string rc = rcOf(session, "display (1) (1+)");
    const std::string txt = shown(session, "display (1) (1+)");
    dfCheck(rc == "198" && txt.rfind("1", 0) == 0,
            "BUG-816: the first failing group stops the line with the items before it printed, "
            "r(198) -- main echoed `(1) (1+)` at rc 0. Got rc " + rc + " `" + txt + "`");
  }
  {
    // CONTROL: the group splitter leaves a segment that is not all groups alone.
    // Main echoed `(1) foo` at rc 0; StataBE prints `1` and then `foo not found`
    // on the same line, r(111) (measured 19.5: `display (1) foo` -> `1foo not
    // found`). This arm admitted either until BUG-1333 split the segment into
    // its two expressions; it now pins StataBE's.
    const std::string txt = shown(session, "display (1) foo");
    const std::string rc = rcOf(session, "display (1) foo");
    dfCheck(rc == "111" && txt == "1foo not found",
            "BUG-816 CONTROL: `display (1) foo` prints `1foo not found` at r(111). Got rc " + rc +
                " `" + txt + "`");
  }
}

void testUnevaluableItemRefused(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  // BUG-817 / FU-855: a parenthesised item after a quoted part. StataBE prints
  // the parts before it and then `invalid syntax`, r(198)
  // (internal capture w2ac_expr_relational.log:2602-2606,
  // disp_not_gt; genuine output of a capture that later timed out, not a golden).
  for (const std::string& line : {std::string("display \"a|\" (1+) \"|\""),
                                  std::string("display \"b|\" (2*) \"|\"")}) {
    const std::string rc = rcOf(session, line);
    const std::string txt = shown(session, line);
    dfCheck(rc != "0" && rc != "1" && txt.find("(1+)") == std::string::npos &&
                txt.find("(2*)") == std::string::npos,
            "BUG-817 FU-855: `" + line + "` is a named refusal, never the source text -- main "
            "printed it at rc 0. Got rc " + rc + " `" + txt + "`");
  }
  dfCheck(rcOf(session, "display \"R|\" %21x (!-1>0) \"|\"") == "198",
          "BUG-817 CONTROL: `display \"R|\" %21x (!-1>0)` stays r(198) (w2ac_expr_relational "
          "disp_not_gt)");
  // FU-141: an unevaluable item under a pending format.
  {
    const std::string rc = rcOf(session, "display %21x (1+)");
    const std::string txt = shown(session, "display %21x (1+)");
    dfCheck(rc == "198" && txt.find("(1+)") == std::string::npos,
            "BUG-817 FU-141: `display %21x (1+)` is r(198), never the source under the format. "
            "Got rc " + rc + " `" + txt + "`");
  }
  dfCheck(shown(session, "display %21x round(1/3, .01)") == "+1.51eb851eb851fX-002",
          "BUG-817 CONTROL: %21x round(1/3, .01) is the value (w2e_float_default.log:1899 "
          "fn_round_lit_double)");
  // FU-201: an arithmetic failure after a quoted part. Stata prints `.` for 1/0
  // (unpinned); the arith layer's division-by-zero is not this task's, so the
  // arm pins only "not echoed, rc never 0 or 1".
  {
    const std::string rc = rcOf(session, "display \"d10|\" 1/0");
    const std::string txt = shown(session, "display \"d10|\" 1/0");
    dfCheck(txt.find("1/0") == std::string::npos && rc != "1" && (rc != "0" || txt == "d10|."),
            "BUG-817 FU-201: `display \"d10|\" 1/0` is `d10|.` or a named refusal -- main "
            "printed `d10|1/0` at rc 0. Got rc " + rc + " `" + txt + "`");
  }
  {
    const std::string rc = rcOf(session, "display (foo)");
    dfCheck(rc == "111",
            "BUG-817: `display (foo)` with no variable or scalar foo is r(111), never `(foo)` -- "
            "main echoed it at rc 0. Got rc " + rc);
  }
  // FU-141, review round: a bare unbound name under a pending format is an
  // expression, not prose. StataBE 19.5 (measured): `display %9.2f foo` ->
  // `foo not found` r(111); `display "a|" %9.2f foo "|"` -> `a|foo not found`.
  // Main echoed `foo` and `a|foo|` at rc 0.
  {
    const std::string rc = rcOf(session, "display %9.2f foo");
    const std::string msg = errOf(session, "display %9.2f foo");
    dfCheck(rc == "111" && msg == "foo not found" && shown(session, "display %9.2f foo").empty(),
            "BUG-817 FU-141: `display %9.2f foo` is r(111) `foo not found`. Got rc " + rc + " `" +
                msg + "`");
    const std::string line = "display \"a|\" %9.2f foo \"|\"";
    const std::string rc2 = rcOf(session, line);
    const std::string txt = shown(session, line);
    dfCheck(rc2 == "111" && txt == "a|foo not found",
            "BUG-817 FU-141: `" + line + "` prints StataBE's `a|foo not found` r(111), never "
            "`a|foo|`. Got rc " + rc2 + " `" + txt + "`");
  }
}

// FU-141, review round: a scalar the macro layer could not evaluate when it was
// set stores its source text; a pending numeric %fmt printed that text at rc 0.
void testScalarTextUnderFormat(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 3", session);
  (void)piet::dispatch("generate double xd = _n/3", session);
  (void)piet::dispatch("scalar b_04 = xd[_N-1]", session);
  // BUG-710: the assignment stores the VALUE of xd[2] (2/3), so StataBE's
  // `P|+1.5555555555555X-001|` is the only answer (measured 19.5; the
  // w2i_varsub b_04 setup with three observations). Main printed `P|xd[_N-1]|`.
  {
    const std::string line = "display \"P|\" %21x b_04 \"|\"";
    const std::string rc = rcOf(session, line);
    const std::string txt = shown(session, line);
    dfCheck(rc == "0" && txt == "P|+1.5555555555555X-001|",
            "BUG-817 FU-141 / BUG-710: `" + line +
                "` is the stored value -- main printed `P|xd[_N-1]|` at rc 0. Got rc " + rc + " `" +
                txt + "`");
    const std::string rc2 = rcOf(session, "display %9.2f b_04");
    dfCheck(rc2 == "0" && shown(session, "display %9.2f b_04") == "     0.67",
            "BUG-817 FU-141 / BUG-710: `display %9.2f b_04` is 0.67, never the text. Got rc " + rc2 +
                " `" + shown(session, "display %9.2f b_04") + "`");
  }
  // CONTROLS (StataBE 19.5, measured): a string scalar under a numeric format
  // prints its text; a numeric scalar is formatted.
  (void)piet::dispatch("scalar ss = \"abc\"", session);
  (void)piet::dispatch("scalar n1 = 1/3", session);
  dfCheck(shown(session, "display \"S|\" %9.2f ss \"|\"") == "S|abc|" &&
              rcOf(session, "display \"S|\" %9.2f ss \"|\"") == "0",
          "BUG-817 CONTROL: a string scalar under %9.2f prints its text, S|abc|");
  dfCheck(shown(session, "display \"N|\" %9.2f n1 \"|\"") == "N|     0.33|",
          "BUG-817 CONTROL: a numeric scalar under %9.2f is N|     0.33|");
}

void testSubscriptItems(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 2", session);
  (void)piet::dispatch("generate double d = mdy(3,4,2010)", session);
  (void)piet::dispatch("generate double c = clock(\"04mar2010 10:20:30\", \"DMYhms\")", session);
  (void)piet::dispatch("generate double x = -2.5", session);
  (void)piet::dispatch("generate double t = cond(_n==1, -1, 1)", session);
  (void)piet::dispatch("generate str3 s = \"ab\"", session);
  (void)piet::dispatch("replace s = \"q(\" in 2", session);
  // FU-853/FU-848/FU-845 CONTROLS (main's FU-207 binds a literal-index cell):
  // each line prints what the twin `generate double z = <expr>` cell prints.
  const auto twin = [&](const std::string& expr, const std::string& line,
                        const std::string& zline) {
    (void)piet::dispatch("capture drop z", session);
    (void)piet::dispatch("generate double z = " + expr, session);
    return shown(session, line) == shown(session, zline) && !shown(session, line).empty();
  };
  dfCheck(twin("ss(c)", "display \"A|\" %21x c[1] \"|\" ss(c[1]) \"|\"",
               "display \"A|\" %21x c[1] \"|\" z[1] \"|\"") &&
              twin("year(d)", "display \"B|\" year(d[1]) \"|\"", "display \"B|\" z[1] \"|\"") &&
              twin("year(d)", "display year(d[1])", "display z[1]") &&
              twin("abs(x)", "display abs(x[1])", "display z[1]") &&
              twin("t^2", "display \"C|\" %21x (t[1]^2) \"|\"", "display \"C|\" %21x z[1] \"|\"") &&
              twin("t*2", "display t[2]*2", "display z[2]"),
          "FU-853 CONTROL: the six subscript lines equal their generate twins");
  // BUG-818: what FU-207's literal-index pass leaves.
  (void)piet::dispatch("capture drop z", session);
  dfCheck(shown(session, "display length(s[1])") == "2" &&
              shown(session, "display (s[1]==\"ab\") (s[2]==\"q(\")") == "11",
          "BUG-818: a string cell inside a call or a comparison is its text (2, 11) -- main "
          "r(198) / the echo");
  {
    const std::string rc = rcOf(session, "display \"E|\" nosuchvar[1]*2 \"|\"");
    const std::string rc2 = rcOf(session, "display missing(nosuchvar[1])");
    dfCheck(rc == "111" && rc2 == "111" &&
                errOf(session, "display missing(nosuchvar[1])") == "nosuchvar not found",
            "BUG-818: a subscript of no variable or matrix is r(111) `nosuchvar not found` "
            "(the one-index rule of w2b_dataio.log:110-113) -- main echoed `E|nosuchvar[1]*2|` "
            "at rc 0 and gave r(198). Got " + rc + "/" + rc2);
  }
  // BUG-1419: this control held r(111) until a string cell was a string operand
  // in display; StataBE 19.5 prints q(c (lane expr-sem x1b/a30.do).
  dfCheck(rcOf(session, "display s[2]+\"c\"") == "0" &&
              shown(session, "display s[2]+\"c\"") == "q(c",
          "BUG-818 CONTROL: `display s[2]+\"c\"` is q(c, as in Stata (selftest_w2b_misc (9b))");
  // FU-349: a matrix index that is an expression.
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 10", session);
  (void)piet::dispatch("generate double x1 = _n", session);
  (void)piet::dispatch("generate double x2 = mod(_n*7, 5)", session);
  (void)piet::dispatch("generate double y = 2*x1 - x2 + mod(_n,3)", session);
  (void)piet::dispatch("quietly regress y x1 x2", session);
  (void)piet::dispatch("matrix cb = e(b)", session);
  {
    const std::string a = shown(session, "display \"E|\" cb[1,colsof(cb)-1] \"|\"");
    const std::string b = shown(session, "display \"E|\" cb[1,2] \"|\"");
    dfCheck(a == b && a.find("colsof") == std::string::npos && !b.empty(),
            "BUG-818 FU-349: `cb[1,colsof(cb)-1]` prints what `cb[1,2]` prints -- main echoed "
            "the subscript. Got `" + a + "` vs `" + b + "`");
    dfCheck(shown(session, "display %21x cb[1,colsof(cb)-1]") ==
                    shown(session, "display %21x cb[1,2]") &&
                rcOf(session, "display cb[1,1+1]*2") == "0",
            "BUG-818 FU-349: the same under %21x and inside arithmetic -- main echoed / r(111)");
  }
}

}  // namespace

void testW2DisplayFormat() {
  std::printf("[selftest] --- W2D-T03 display format: %%21x, %%w.0g, _b[]/_se[]/e() ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2displayformat").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  testHexRenderer();
  testGeneralSmallMagnitude();
  testDisplayHexItemKinds(workdir);
  testEstimateItemsHonourFormat(workdir);
  testPostedEMatrixElement(workdir);
  testNoEstimatesIs111(workdir);
  testAdjacentGroups(workdir);
  testUnevaluableItemRefused(workdir);
  testScalarTextUnderFormat(workdir);
  testSubscriptItems(workdir);

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2D-T03 display format: %d checks\n", gDisplayFormatChecks);
}
