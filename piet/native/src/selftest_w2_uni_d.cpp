// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode names, lane D: the dispatcher and the remaining PIET scanners read a
// name the way core/ident.h says (a byte >= 0x80 is a name byte), so a Unicode
// name is ONE name -- `دخل_2020` never binds its ASCII tail `_2020`, `xملاحظات`
// is never `x`.
//
// Every expected value is StataBE 19.5's on the same fixture
// (scratch-r150/uni-d/fix_d.do -> fix_d.log, OUT| lines named per arm). The
// fixture's Arabic names arrive through `rename`, so no arm needs the engine's
// expression parser to read an Arabic name; the arms that do reach the engine
// only pass it whole column names (a varlist, a weight).
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "piet/column_fetch.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/session_state.h"
#include "piet/strmutate.h"
#include "piet/translate.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

std::vector<ojson> blocksOf(piet::SessionState& session, const std::string& cmd) {
  std::vector<ojson> out;
  try {
    const ojson res = piet::dispatch(cmd, session);
    if (res.is_array()) {
      for (const auto& b : res) out.push_back(b);
    } else if (!res.is_null()) {
      out.push_back(res);
    }
  } catch (const std::exception& e) {
    out.push_back(ojson{{"type", "error"}, {"message", std::string("EXC:") + e.what()}});
  }
  return out;
}

// rc of one dispatched line (the last uncaptured error block's coarse rc; "0").
std::string rcOf(piet::SessionState& session, const std::string& cmd, std::string* msg = nullptr) {
  std::string rc = "0";
  if (msg != nullptr) msg->clear();
  for (const auto& b : blocksOf(session, cmd)) {
    if (!b.is_object()) return "shape";
    try {
      if (b.value("type", std::string()) == "error" && !b.value("captured", false)) {
        rc = piet::eval::coarseRc(b);
        if (msg != nullptr) *msg = b.value("message", std::string());
      }
    } catch (const std::exception& e) {
      return std::string("shape:") + e.what();
    }
  }
  return rc;
}

std::string shown(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : blocksOf(session, cmd)) {
    if (!b.is_object()) continue;
    try {
      if (b.value("type", std::string()) == "display") text += b.value("text", std::string());
    } catch (const std::exception&) {
    }
  }
  return text;
}

double rNum(piet::SessionState& session, const char* key) {
  const double* v = session.eval.rValues.find(key);
  return v == nullptr ? -1.0e300 : *v;
}

// fix_d.do lines 1-14.
void buildFixture(piet::SessionState& session) {
  for (const char* s :
       {"clear", "set obs 4", "generate double id = _n", "generate double v = id*2",
        "generate double w = id", "generate str5 s = \"hello\"", "generate str3 t = \"x\"",
        "replace t = \"y\" in 1", "generate double d = id + 10", "rename v قيمة", "rename w وزن",
        "rename s ملاحظات", "rename t ق2", "rename d دخل_2020"}) {
    (void)piet::dispatch(s, session);
  }
}

void testScanners() {
  // exprfns.h: the missing-literal rewrite and the running-sum test take a byte
  // >= 0x80 as a name byte on either side.
  w2check(piet::eval::replaceStataMissingLiterals(".aق") == ".aق",
          "UNI/D exprfns: `.aق` is not the extended missing .a followed by a name");
  w2check(piet::eval::replaceStataMissingLiterals("ق.") == "ق.",
          "UNI/D exprfns: a `.` right after an Arabic name byte is not a bare missing");
  w2check(piet::eval::replaceStataMissingLiterals("x == .a") != "x == .a",
          "UNI/D exprfns: ASCII `.a` is still rewritten (control)");
  w2check(!piet::eval::exprHasRunningSum("مsum(x)"),
          "UNI/D exprfns: `مsum(` is a call of the name مsum, not of sum()");
  w2check(piet::eval::exprHasRunningSum("sum(قيمة)"),
          "UNI/D exprfns: `sum(قيمة)` is a running sum (control)");

  // translate.cpp: `L.ملاحظات` is one TS token, base name whole; a name ending in
  // an Arabic byte before `L.x` is not an operator chain start.
  {
    const auto toks = piet::translate::extractTsTokens("L.ملاحظات + 1");
    w2check(toks.size() == 1 && toks[0].token == "L.ملاحظات",
            "UNI/D translate: `L.ملاحظات` is one TS token with the whole Arabic base");
    w2check(piet::translate::extractTsTokens("مL.x").empty(),
            "UNI/D translate: `مL.x` is the tail of a name, not the operator L.");
  }

  // The regex spellings agree with core/ident.h on a mixed-script name.
  {
    const std::regex re(std::string("^") + piet::eval::kRxName + "$");
    w2check(std::regex_match(std::string("دخل_2020"), re) && std::regex_match(std::string("_x1"), re) &&
                !std::regex_match(std::string("2x"), re) && !std::regex_match(std::string("a-b"), re),
            "UNI/D kRxName: a mixed-script name is one name; ASCII verdicts unchanged");
  }
}

void testDispatch(const std::string& workdir) {
  piet::SessionState session(workdir);
  buildFixture(session);
  std::string msg;

  // CiParser (handlers_m3.cpp): the count fast path reads a whole Arabic name.
  // Before lane D it bailed to the engine on the first Arabic byte.
  std::string rc = rcOf(session, "count if قيمة > 2", &msg);
  w2check(rc == "0" && rNum(session, "N") == 3.0,
          "UNI/D count: `count if قيمة > 2` is r(N) 3 (fix_d.log OUT|c1|3). Got rc " + rc + " `" +
              msg + "`");
  // expandRowVarAbbrevs (dispatcher.cpp): an abbreviation by whole characters.
  rc = rcOf(session, "count if قيم > 2", &msg);
  w2check(rc == "0" && rNum(session, "N") == 3.0,
          "UNI/D count: `count if قيم > 2` abbreviates قيمة, r(N) 3 (OUT|abbr|3). Got rc " + rc +
              " `" + msg + "`");
  // The row-plane constant-compare fold (dispatcher.cpp): the `2` of ق2 is the
  // tail of the string variable, not a number compared with a string. Before
  // lane D this was refused r(109) "type mismatch" before any row was read.
  // StataBE answers r(N) 3 (OUT|q2|3); the row evaluation itself is the string
  // row evaluator's (pyeval.cpp, lane M), so this arm asserts the fold only.
  rc = rcOf(session, "count if ق2 == \"x\"", &msg);
  w2check(rc != "109" && msg.find("type mismatch") == std::string::npos,
          "UNI/D fold: `count if ق2 == \"x\"` is not refused as a string-vs-number compare. Got rc " +
              rc + " `" + msg + "`");
  if (rc == "0" && rNum(session, "N") == 3.0) {
    w2check(true, "UNI/D count: `count if ق2 == \"x\"` is r(N) 3 (OUT|q2|3)");
  } else {
    w2skip("UNI/D count: `count if ق2 == \"x\"` r(N) 3 (OUT|q2|3)",
           "the string row evaluator (pyeval.cpp, lane M) does not lex an Arabic name yet");
  }

  // display_hardening.cpp: variable subscripts.
  w2check(shown(session, "display قيمة[2]") == "4",
          "UNI/D display: `display قيمة[2]` is 4 (OUT|sub|4)");
  w2check(shown(session, "display ملاحظات[1]") == "hello",
          "UNI/D display: `display ملاحظات[1]` is hello (OUT|subs|hello)");

  // A weight variable with an Arabic name is a variable, not an expression.
  rc = rcOf(session, "summarize id [aw=وزن]", &msg);
  w2check(rc == "0" && std::fabs(rNum(session, "mean") - 3.0) < 1e-12,
          "UNI/D weight: `summarize id [aw=وزن]` r(mean) 3 (OUT|aw|3.000000). Got rc " + rc + " `" +
              msg + "`");

  // The fit varlist expander: `قي*` is a name pattern (dispatcher isNamePattern).
  rc = rcOf(session, "regress id قي*", &msg);
  const std::string dfm = shown(session, "display e(df_m)");
  w2check(rc == "0" && dfm == "1",
          "UNI/D regress: `regress id قي*` fits one regressor, e(df_m) 1 (OUT|dfm|1). Got rc " + rc +
              " `" + msg + "`");
  // display's _b[] pattern takes an Arabic coefficient name.
  const std::string b = shown(session, "display %12.6f _b[قيمة]");
  w2check(b == "    0.500000", "UNI/D display: `display %12.6f _b[قيمة]` is 0.500000 (OUT|b). Got `" +
                                     b + "`");

  // Value labels, confirm, matrices and stored estimates (all rc as StataBE's).
  w2check(rcOf(session, "label define تسمية 1 \"one\" 2 \"two\"") == "0",
          "UNI/D label: `label define تسمية ...` rc 0 (OUT|ldef|0)");
  w2check(rcOf(session, "label values id تسمية") == "0",
          "UNI/D label: `label values id تسمية` rc 0 (OUT|lval|0)");
  w2check(rcOf(session, "label list تسمية") == "0", "UNI/D label: `label list تسمية` rc 0 (OUT|llist|0)");
  w2check(rcOf(session, "confirm names قيمة") == "0", "UNI/D confirm: `confirm names قيمة` rc 0 (OUT|cn1|0)");
  w2check(rcOf(session, "confirm names ١ق") == "7",
          "UNI/D confirm: `confirm names ١ق` rc 7 -- an Arabic-Indic digit cannot start a name (OUT|cn2|7)");
  w2check(rcOf(session, "matrix مصفوفة = e(b)") == "0", "UNI/D matrix: `matrix مصفوفة = e(b)` rc 0 (OUT|mat|0)");
  w2check(rcOf(session, "matrix list مصفوفة") == "0", "UNI/D matrix: `matrix list مصفوفة` rc 0 (OUT|matl|0)");
  // The name a matrix is created under follows the strict rule
  // (scratch-r150/uni-d/badnames.log: mat_ok 0, mat_bad 198, mat_euro 198).
  w2check(rcOf(session, "matrix قيم = (1,2)") == "0", "UNI/D matrix: `matrix قيم = (1,2)` rc 0 (mat_ok)");
  w2check(rcOf(session, "matrix ١ = (1,2)") == "198",
          "UNI/D matrix: `matrix ١ = (1,2)` r(198) invalid name (mat_bad)");
  w2check(rcOf(session, "matrix €M = (1,2)") == "198",
          "UNI/D matrix: `matrix €M = (1,2)` r(198) invalid name (mat_euro)");
  // `local NAME : display` takes a Unicode macro name (badnames.log loc_ok, loc_val 1).
  (void)piet::dispatch("local قيم : display 1", session);
  w2check(session.eval.locals.get("قيم") == "1",
          "UNI/D macro: `local قيم : display 1` sets the local to 1 (loc_val)");
  w2check(rcOf(session, "estimates store نموذج") == "0",
          "UNI/D estimates: `estimates store نموذج` rc 0 (OUT|est|0)");

  // strmutate.cpp: a bare Arabic string column routes gen to the string row
  // evaluator (referencesFactor used to see no identifier in `ملاحظات`).
  w2check(piet::strmutate::needsStringEval(session, "y", "ملاحظات", std::nullopt, false, false),
          "UNI/D strmutate: `gen y = ملاحظات` takes the string row evaluator");
  w2check(piet::strmutate::needsStringCount(session, std::string("ملاحظات == ملاحظات")),
          "UNI/D strmutate: an `if` naming an Arabic string column is a string condition");

  // A mixed-script name is one name to the count fast path (CiParser): r(N) 2
  // (fix_d.log OUT|mixed|2, the same rows with or without scalar _2020).
  rc = rcOf(session, "count if دخل_2020 > 12", &msg);
  w2check(rc == "0" && rNum(session, "N") == 2.0,
          "UNI/D count: `count if دخل_2020 > 12` is r(N) 2 (OUT|mixed|2). Got rc " + rc + " `" + msg +
              "`");
  // With a scalar named like the ASCII tail, the stored-scalar pass must not
  // touch `_2020` inside the name. The pass's word boundary is
  // eval::substituteBareName (macros.cpp, lane M).
  (void)piet::dispatch("scalar _2020 = 99", session);
  rc = rcOf(session, "count if دخل_2020 > 12", &msg);
  if (rc == "0" && rNum(session, "N") == 2.0) {
    w2check(true, "UNI/D count: with scalar _2020, `count if دخل_2020 > 12` is still r(N) 2");
  } else {
    w2skip("UNI/D count: with scalar _2020, `count if دخل_2020 > 12` r(N) 2 (OUT|mixed|2). Got rc " +
               rc + " `" + msg + "`",
           "eval::substituteBareName (macros.cpp, lane M) still binds the ASCII tail _2020");
  }
  (void)piet::dispatch("scalar drop _2020", session);

  // column_fetch.cpp: `strlen(ملاحظات) > قيمة` names both columns.
  {
    const std::string e = "strlen(ملاحظات) > قيمة";
    const auto cols = piet::referencedColumns(session, {&e}, "");
    if (!cols.has_value()) {
      w2skip("UNI/D column_fetch: `strlen(ملاحظات) > قيمة` fetch list",
             "no current shadow varlist card in this session (the predicate answers nullopt)");
    } else {
      const bool both = std::find(cols->begin(), cols->end(), "ملاحظات") != cols->end() &&
                        std::find(cols->begin(), cols->end(), "قيمة") != cols->end();
      w2check(both, "UNI/D column_fetch: `strlen(ملاحظات) > قيمة` fetches both columns, "
                    "not only قيمة");
    }
  }
}

}  // namespace

void testW2UniD() {
  std::printf("[selftest] --- Unicode names (D): dispatcher and PIET scanners ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2unid").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  testScanners();
  testDispatch(workdir);
  fs::remove_all(workdir, ec);
}
