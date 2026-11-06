// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// T7 lane L2 selftest (ihstrans, package-007 unblock). The command is a loop of
// ordinary statements (generate double / label variable / quietly summarize /
// drop), so the arms assert STRUCTURE, never a hard-coded transform value: the
// new column equals the formula evaluated by the same evaluator on the same
// rows, the missing count is the one generate reports, the list arithmetic
// (numeric filter, keepusing, xtset exclusion, casewise sample) selects the
// variables the ado selects, and every refusal carries the rc Stata gives.
//
// What was checked against StataBE and what was not: the Stata primitives the ado
// leans on (`_xt` r(459) unless xtset with a panel variable, regexr's first
// literal-substring removal, `marksample, strok` treating an empty string as
// missing, `: format L.x` r(198), `summarize` of an all-missing variable r(N)=0,
// r() left behind by a non-r-class program) and the generate formula's bit
// pattern were probed in StataBE. `ihstrans` itself could not be run there (the
// package is not installed), so the arms below pin the contract read off the
// public ado, not a captured ihstrans log.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

int gIhsChecks = 0;
void ihsCheck(bool cond, const std::string& label) {
  ++gIhsChecks;
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

// How many error blocks a line returned, and whether the last block is one: a
// failure is reported once, and last.
std::pair<int, bool> errorShapeOf(piet::SessionState& session, const std::string& cmd) {
  int n = 0;
  bool last = false;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      last = b.value("type", std::string()) == "error";
      if (last) ++n;
    }
  } catch (const std::exception&) {
  }
  return {n, last};
}

// Text of one display line (empty when it errored or displayed nothing).
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

// r(N) after one line, -1 when the line did not set it.
double rN(piet::SessionState& session, const std::string& cmd) {
  (void)piet::dispatch(cmd, session);
  const double* n = session.eval.rValues.find("N");
  return n == nullptr ? -1.0 : *n;
}

bool hasVar(piet::SessionState& session, const std::string& name) {
  // `exact`: without it `ihs_t` names `ihs_tt` by abbreviation (BUG-1103).
  return rcOf(session, "confirm variable " + name + ", exact") == "0";
}

// Non-missing count of a variable.
double nonMissing(piet::SessionState& session, const std::string& name) {
  return rN(session, "quietly count if !missing(" + name + ")");
}

// Rows where `name` holds a value that differs from the transform of `base` (0 ==
// every filled row is the formula). Rows `name` left missing (outside an if/in or
// the casewise sample) are not in question here; the callers count those apart.
double formulaMisses(piet::SessionState& session, const std::string& name,
                     const std::string& base) {
  return rN(session, "quietly count if !missing(" + name + ") & " + name + " != log(" + base + "+sqrt(" +
                         base + "^2+1))");
}

// The n_missing the last data_mutation block that lists `var` carries for it --
// the figure generate's "(N missing values generated)" line is made from. -1 if
// no block lists it.
long newMissingOf(const ojson& blocks, const std::string& var) {
  long found = -1;
  for (const auto& b : blocks) {
    if (!b.is_object() || b.value("type", std::string()) != "data_mutation") continue;
    if (!b.contains("varlist_after") || !b["varlist_after"].is_array()) continue;
    for (const auto& v : b["varlist_after"])
      if (v.is_object() && v.value("name", std::string()) == var && v.contains("n_missing") &&
          v["n_missing"].is_number_integer())
        found = v["n_missing"].get<long>();
  }
  return found;
}

void fixture(piet::SessionState& session) {
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 6", session);
  (void)piet::dispatch("generate double x = (_n-3)*2.5", session);      // -5 -2.5 0 2.5 5 .
  (void)piet::dispatch("replace x = . in 6", session);
  (void)piet::dispatch("generate double y = _n*10", session);           // 10 . 30 40 50 60
  (void)piet::dispatch("replace y = . in 2", session);
  (void)piet::dispatch("generate str5 s = \"ab\"", session);
  (void)piet::dispatch("replace s = \"\" in 4", session);
  (void)piet::dispatch("generate double allm = .", session);
  (void)piet::dispatch("generate long id = ceil(_n/3)", session);
  (void)piet::dispatch("generate long t = mod(_n-1,3)+1", session);
  (void)piet::dispatch("generate long tt = _n", session);  // a time variable tsset can take
  (void)piet::dispatch("generate double a = _n", session);
  (void)piet::dispatch("generate double b = _n*2", session);
  (void)piet::dispatch("generate double c = _n*3", session);
}

void testBareForm(piet::SessionState& session) {
  fixture(session);
  const ojson blocks = piet::dispatch("ihstrans x", session);
  // r() is what `summarize ihs_x` left (a non-r-class ado leaves its last r-class
  // command's r() behind -- StataBE probe); read before anything else runs.
  const double* rn = session.eval.rValues.find("N");
  ihsCheck(rn != nullptr && *rn == 5.0, "T7/ihstrans: r(N) after the command is summarize's N (5)");
  ihsCheck(hasVar(session, "ihs_x"), "T7/ihstrans: `ihstrans x` creates ihs_x");
  ihsCheck(shown(session, "display \"`: type ihs_x'\"") == "double",
           "T7/ihstrans: the new column is a double");
  ihsCheck(formulaMisses(session, "ihs_x", "x") == 0.0 && nonMissing(session, "ihs_x") == 5.0,
           "T7/ihstrans: ihs_x == log(x+sqrt(x^2+1)) on all 5 non-missing rows (0 included)");
  ihsCheck(shown(session, "display \"`: variable label ihs_x'\"") == "IHS trans. values of x",
           "T7/ihstrans: the label reads `IHS trans. values of x`");
  ihsCheck(newMissingOf(blocks, "ihs_x") == 1,
           "T7/ihstrans: generate's own missing count is carried (one missing x -> 1)");
  ihsCheck(!hasVar(session, "ihs_y") && !hasVar(session, "ihs_a"),
           "T7/ihstrans: only the named variable is transformed");
  // a second variable, several in one call
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans y a") == "0" && hasVar(session, "ihs_y") && hasVar(session, "ihs_a") &&
               formulaMisses(session, "ihs_y", "y") == 0.0 && formulaMisses(session, "ihs_a", "a") == 0.0,
           "T7/ihstrans: a varlist transforms each variable");
  // a range
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans a-c") == "0" && hasVar(session, "ihs_a") && hasVar(session, "ihs_b") &&
               hasVar(session, "ihs_c"),
           "T7/ihstrans: a-c is a range");
}

void testPrefixAndAbbreviations(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x, prefix(log_)") == "0" && hasVar(session, "log_x") &&
               !hasVar(session, "ihs_x"),
           "T7/ihstrans: prefix() replaces ihs_");
  ihsCheck(shown(session, "display \"`: variable label log_x'\"") == "IHS trans. values of x",
           "T7/ihstrans: the label names the SOURCE variable, whatever the prefix");
  ihsCheck(rcOf(session, "ihstrans x, p(zz_)") == "0" && hasVar(session, "zz_x"),
           "T7/ihstrans: `p()` is the declared one-letter abbreviation of prefix()");
  ihsCheck(rcOf(session, "ihstrans y, pre(yy_)") == "0" && hasVar(session, "yy_y"),
           "T7/ihstrans: `pre()` resolves as a prefix of prefix()");
}

void testQualifiers(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x if x > 0") == "0" && nonMissing(session, "ihs_x") == 2.0 &&
               formulaMisses(session, "ihs_x", "x") == 0.0,
           "T7/ihstrans: `if x > 0` fills the 2 positive rows and leaves the rest missing");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans y in 2/4") == "0" && nonMissing(session, "ihs_y") == 2.0,
           "T7/ihstrans: `in 2/4` covers rows 2-4; y is missing in row 2, so 2 values");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans y if x > 0 in 1/5") == "0" && nonMissing(session, "ihs_y") == 2.0,
           "T7/ihstrans: if and in together");
}

void testNumericFilterAndDrop(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans allm") == "0" && !hasVar(session, "ihs_allm"),
           "T7/ihstrans: an all-missing result is dropped again (summarize r(N)==0), rc 0");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans s") == "0" && !hasVar(session, "ihs_s"),
           "T7/ihstrans: a string variable (format %..s) is skipped, rc 0");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans") == "0" && hasVar(session, "ihs_x") && hasVar(session, "ihs_y") &&
               hasVar(session, "ihs_id") && hasVar(session, "ihs_t") && hasVar(session, "ihs_a") &&
               !hasVar(session, "ihs_s") && !hasVar(session, "ihs_allm"),
           "T7/ihstrans: a bare `ihstrans` takes every numeric variable, not the string, "
           "not the all-missing one");
  fixture(session);
  ihsCheck(rcOf(session, "format a %9.2f") == "0" && rcOf(session, "ihstrans a") == "0" &&
               hasVar(session, "ihs_a"),
           "T7/ihstrans: a numeric format without an `s` stays numeric");
}

void testKeepusing(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x y a, keepusing(y)") == "0" && hasVar(session, "ihs_x") &&
               hasVar(session, "ihs_a") && !hasVar(session, "ihs_y"),
           "T7/ihstrans: keepusing(y) keeps y out of the list");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x y a, k(y)") == "0" && !hasVar(session, "ihs_y") &&
               hasVar(session, "ihs_x"),
           "T7/ihstrans: `k()` is the declared one-letter abbreviation of keepusing()");
  // The ado removes keepusing with regexr(list, "a c", ""): ONE literal match of the
  // whole space-joined string. "a b c" has no "a c", so nothing is removed.
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans a b c, keepusing(a c)") == "0" && hasVar(session, "ihs_a") &&
               hasVar(session, "ihs_b") && hasVar(session, "ihs_c"),
           "T7/ihstrans: keepusing(a c) is a no-op unless \"a c\" is contiguous in the list "
           "(regexr removes one literal substring, as the ado does)");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans a b c, keepusing(b c)") == "0" && hasVar(session, "ihs_a") &&
               !hasVar(session, "ihs_b") && !hasVar(session, "ihs_c"),
           "T7/ihstrans: keepusing(b c) removes the contiguous pair");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x, keepusing(nosuch)") == "111",
           "T7/ihstrans: keepusing() of an unknown variable is r(111)");
}

void testPanelExclusion(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "xtset id t") == "0" && rcOf(session, "ihstrans") == "0" &&
               hasVar(session, "ihs_x") && hasVar(session, "ihs_a") && !hasVar(session, "ihs_id") &&
               !hasVar(session, "ihs_t"),
           "T7/ihstrans: with xtset id t, the panel and time variables are left out of a bare list");
  // `_xt` is r(459) for time-series-only data (StataBE probe), so a tsset time
  // variable is NOT excluded.
  fixture(session);
  ihsCheck(rcOf(session, "tsset tt") == "0", "T7/ihstrans CONTROL: tsset tt accepted");
  ihsCheck(rcOf(session, "ihstrans") == "0" && hasVar(session, "ihs_tt") && hasVar(session, "ihs_id") &&
               hasVar(session, "ihs_t"),
           "T7/ihstrans: with tsset (no panel variable) nothing is excluded");
}

void testCasewise(piet::SessionState& session) {
  fixture(session);
  // y is missing in row 2, x in row 6: casewise keeps rows 1, 3, 4, 5.
  ihsCheck(rcOf(session, "ihstrans x y, casewise") == "0" && nonMissing(session, "ihs_x") == 4.0 &&
               nonMissing(session, "ihs_y") == 4.0 && formulaMisses(session, "ihs_x", "x") == 0.0,
           "T7/ihstrans: casewise keeps the rows complete on every variable of the varlist");
  ihsCheck(!hasVar(session, "__ihs_touse"), "T7/ihstrans: casewise leaves no marker column behind");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x y") == "0" && nonMissing(session, "ihs_x") == 5.0 &&
               nonMissing(session, "ihs_y") == 5.0,
           "T7/ihstrans CONTROL: without casewise each variable keeps its own rows (5 and 5)");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x y, c") == "0" && nonMissing(session, "ihs_x") == 4.0,
           "T7/ihstrans: `c` is the declared one-letter abbreviation of casewise");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x y, casewise") == "0" && rcOf(session, "ihstrans a, casewise") == "0" &&
               nonMissing(session, "ihs_a") == 6.0,
           "T7/ihstrans: casewise on a fully observed variable keeps all 6 rows");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x if x > 100, casewise") == "2000" && !hasVar(session, "ihs_x") &&
               !hasVar(session, "__ihs_touse"),
           "T7/ihstrans: an empty casewise sample is r(2000) and leaves nothing behind");
  // The full varlist counts: allm is missing in every row, so a bare casewise
  // run marks out every observation (and a string's empty value is missing too).
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans, casewise") == "2000",
           "T7/ihstrans: casewise over the default varlist sees the all-missing column -> r(2000)");
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x s, casewise") == "0" && nonMissing(session, "ihs_x") == 4.0 &&
               !hasVar(session, "ihs_s"),
           "T7/ihstrans: a string in the varlist marks out its empty rows (row 4) but is not "
           "transformed");
}

void testRefusals(piet::SessionState& session) {
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans x") == "0" && rcOf(session, "ihstrans x") == "110" &&
               formulaMisses(session, "ihs_x", "x") == 0.0,
           "T7/ihstrans: a second run hits generate's own `already defined` r(110)");
  {
    // The failing generate's error block reaches the caller once, as the last block
    // (earlier output precedes it; nothing is repeated after the casewise cleanup).
    const auto shape = errorShapeOf(session, "ihstrans x");
    ihsCheck(shape.first == 1 && shape.second,
             "T7/ihstrans: a failing generate is reported exactly once, and last");
    fixture(session);
    (void)piet::dispatch("generate double ihs_y = 1", session);
    const auto cw = errorShapeOf(session, "ihstrans x y, casewise");
    ihsCheck(cw.first == 1 && cw.second && !hasVar(session, "__ihs_touse") && hasVar(session, "ihs_x"),
             "T7/ihstrans: a mid-list failure under casewise: one error, last, marker dropped, "
             "earlier variable kept");
  }
  fixture(session);
  ihsCheck(rcOf(session, "ihstrans nosuch") == "111", "T7/ihstrans: an unknown variable is r(111)");
  ihsCheck(rcOf(session, "ihstrans x, bogus") == "198", "T7/ihstrans: an unknown option is r(198)");
  ihsCheck(rcOf(session, "ihstrans x, prefix") == "198",
           "T7/ihstrans: prefix with no argument is r(198)");
  ihsCheck(rcOf(session, "ihstrans x, keepusing") == "198",
           "T7/ihstrans: keepusing with no argument is r(198)");
  ihsCheck(rcOf(session, "ihstrans x, casewise(1)") == "198",
           "T7/ihstrans: casewise takes no argument (r(198))");
  // `: format L.x` is r(198) in the ado, before anything is generated.
  ihsCheck(rcOf(session, "tsset tt") == "0", "T7/ihstrans CONTROL: tsset tt accepted (refusals)");
  ihsCheck(rcOf(session, "ihstrans L.x") == "198" && !hasVar(session, "ihs_x"),
           "T7/ihstrans: a time-series-operator varlist is r(198) and generates nothing");
  // A name longer than 32 characters is generate's r(198), after the earlier
  // variables of the list were already created (the ado does not unwind).
  fixture(session);
  (void)piet::dispatch("generate double a_very_long_variable_name_xxxxxx = 1", session);
  ihsCheck(rcOf(session, "ihstrans a a_very_long_variable_name_xxxxxx") == "198" && hasVar(session, "ihs_a"),
           "T7/ihstrans: a too-long new name stops the loop at r(198); earlier variables stay");
}

void testEmptyDataset() {
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_t7_ihstrans_empty").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    const std::string rc = rcOf(session, "ihstrans x");
    ihsCheck(rc == "111",
             "T7/ihstrans: with no data in memory the command is r(111), as summarize is");
  }
  fs::remove_all(workdir, ec);
}

}  // namespace

void testW2T7Ihstrans() {
  std::printf("[selftest] --- T7 L2 ihstrans: the IHS transform loop ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_t7_ihstrans").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState session(workdir);
    testBareForm(session);
    testPrefixAndAbbreviations(session);
    testQualifiers(session);
    testNumericFilterAndDrop(session);
    testKeepusing(session);
    testPanelExclusion(session);
    testCasewise(session);
    testRefusals(session);
  }
  testEmptyDataset();
  fs::remove_all(workdir, ec);
  std::printf("[selftest] T7 L2 ihstrans: %d checks\n", gIhsChecks);
}
