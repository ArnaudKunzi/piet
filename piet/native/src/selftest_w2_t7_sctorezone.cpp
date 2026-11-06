// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// T7 lane L3 selftest (sctorezone, SSC package scto) -- package-007 unblock.
//
// What each kind of expectation rests on (see the handler's header comment):
//   * hexadecimal values (%21x) and the report lines for a given shift come from
//     StataBE 19.5 runs of the Stata PRIMITIVES the ado is built from -- a float
//     tempvar from real(), `replace v = v + shifttime*60*60*1000` on a float and
//     on a double %tc variable, levelsof/count/abs for the report -- run on the
//     same fixture (scratch probes t7l3_cmp_stata.do / p1.do). The package
//     itself is not installed here, so the COMMAND was never compared with a run
//     of itself.
//     The probe files named in the labels (p1.do, t7l3_cmp_stata.do,
//     t7l3_levels.do, t7l3_settype.do, t7l3_syntax*.do) live under
//     a scratch directory (not in the repo).
//   * error codes for the `syntax` line (rc 100 / 198 / 111) were checked
//     against StataBE with a program carrying the same syntax line.
//   * the rc-1 shift errors, the string-time and automatic-mode behaviour, the
//     empty-dataset path and the order of checks are read from the ado, and so
//     are asserted STRUCTURALLY (rc, a phrase of the message, "nothing changed")
//     rather than by a number.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// The whole result of one line: its display text, its last error block, and
// whether it produced a data_mutation block (the front end's refresh cue).
struct Out {
  std::string text;
  ojson error;
  bool mutation = false;
  std::string rc = "0";
};

Out run(piet::SessionState& session, const std::string& cmd) {
  Out o;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "display") o.text += b.value("text", std::string());
      if (t == "data_mutation") o.mutation = true;
      if (t == "error") {
        o.error = b;
        o.rc = piet::eval::coarseRc(b);
      }
    }
  } catch (const std::exception& e) {
    o.rc = std::string("EXC:") + e.what();
  }
  return o;
}

// rc of one dispatched line (the last error block's coarse rc; "0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  return run(session, cmd).rc;
}

std::string msgOf(const Out& o) { return o.error.is_object() ? o.error.value("message", std::string()) : ""; }

// `display %21x var[i]` -- the exact stored bits.
std::string hexAt(piet::SessionState& session, const std::string& var, int i) {
  return run(session, "display %21x " + var + "[" + std::to_string(i) + "]").text;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The fixture of the StataBE probe p1.do: six rows, a FLOAT %tc variable (an
// untyped `generate`, exactly as Wave 2 cleaning.do:73 makes it), rows 2 and 4
// missing, row 5 the extended missing .a, row 3 the epoch, row 6 the year 2099.
// A double copy, a %tC copy and a %td variable ride along: they must not move
// when only(fullstartdate) is given, and the %tC/%td ones never move at all.
void fixture(piet::SessionState& session) {
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 6", session);
  (void)piet::dispatch("generate fullstartdate = clock(\"2020-03-04 10:11:12\", \"YMDhms\")", session);
  (void)piet::dispatch("replace fullstartdate = clock(\"1960-01-01 00:00:00\", \"YMDhms\") in 3", session);
  (void)piet::dispatch("replace fullstartdate = clock(\"2099-12-31 23:00:00\", \"YMDhms\") in 6", session);
  (void)piet::dispatch("replace fullstartdate = . in 2", session);
  (void)piet::dispatch("replace fullstartdate = . in 4", session);
  (void)piet::dispatch("replace fullstartdate = .a in 5", session);
  (void)piet::dispatch("format fullstartdate %tc", session);
  (void)piet::dispatch("generate double dstart = fullstartdate", session);
  (void)piet::dispatch("format dstart %tc", session);
  (void)piet::dispatch("generate ctime = fullstartdate", session);
  (void)piet::dispatch("format ctime %tC", session);
  (void)piet::dispatch("generate dday = 21915", session);
  (void)piet::dispatch("format dday %td", session);
}

// `%21x` of the system missing `.` (StataBE, p1.do AFT2/AFT4).
constexpr const char* kMissingHex = "+1.0000000000000X+3ff";

void testCorpusForm(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  const std::string pre1 = hexAt(session, "fullstartdate", 1);
  w2check(pre1 == "+1.ba21660000000X+028",
          "T7/sctorezone: the fixture is a FLOAT tc (04mar2020 10:11:12 -> 10:11:17, p1.do ROW1)");
  const std::string dPre1 = hexAt(session, "dstart", 1);
  const std::string cPre = hexAt(session, "ctime", 1);

  // The line package 007 writes (Wave 2 cleaning.do:79), verbatim.
  const Out o = run(session, "sctorezone +9, force only(fullstartdate)");
  w2check(o.rc == "0", "T7/sctorezone: `sctorezone +9, force only(fullstartdate)` is rc 0 -- got " + o.rc +
                           " " + msgOf(o));
  w2check(o.text == "The following variable(s) have been shifted:  fullstartdate\n"
                    "- 6 observations shifted forwards by 9 hours.\n",
          "T7/sctorezone: the report (two blanks after the colon, N = _N, plural hours). Got `" + o.text + "`");
  w2check(o.mutation, "T7/sctorezone: a data_mutation block reaches the front end");
  w2check(hexAt(session, "fullstartdate", 1) == "+1.ba23540000000X+028",
          "T7/sctorezone: float row 1 re-rounded to float (19:10:52, p1.do AFT1)");
  w2check(hexAt(session, "fullstartdate", 3) == "+1.ee62800000000X+018",
          "T7/sctorezone: float row 3 is 01jan1960 09:00:00 (p1.do AFT3)");
  w2check(hexAt(session, "fullstartdate", 6) == "+1.012ab60000000X+02a",
          "T7/sctorezone: float row 6 carries the float rounding, not the exact sum (p1.do AFT6)");
  w2check(hexAt(session, "fullstartdate", 2) == kMissingHex && hexAt(session, "fullstartdate", 4) == kMissingHex,
          "T7/sctorezone: missing stays missing");
  w2check(hexAt(session, "fullstartdate", 5) == kMissingHex,
          "T7/sctorezone: .a + shift is the system missing (p1.do AFT5)");
  w2check(hexAt(session, "dstart", 1) == dPre1 && hexAt(session, "ctime", 1) == cPre,
          "T7/sctorezone: only(fullstartdate) leaves the double, %tC and %td variables alone");

  // Second shift on the same variable: -5.5 after +9 (p1.do NEG1/NEG3/NEG6).
  const Out o2 = run(session, "sctorezone -5.5, force only(fullstartdate)");
  w2check(o2.text == "The following variable(s) have been shifted:  fullstartdate\n"
                     "- 6 observations shifted backwards by 5.5 hours.\n",
          "T7/sctorezone: a negative fractional shift reads `backwards by 5.5 hours`. Got `" + o2.text + "`");
  w2check(hexAt(session, "fullstartdate", 1) == "+1.ba22260000000X+028" &&
              hexAt(session, "fullstartdate", 3) == "+1.8085800000000X+017" &&
              hexAt(session, "fullstartdate", 6) == "+1.012a6a0000000X+02a",
          "T7/sctorezone: +9 then -5.5 lands on StataBE's bits (p1.do NEG1/NEG3/NEG6)");
  w2check(piet::eval::findOptVocab("sctorezone") != nullptr,
          "T7/sctorezone: the option vocabulary is registered at namespace scope");
}

void testSweepAndExclude(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  const std::string cPre = hexAt(session, "ctime", 6);
  const std::string dayPre = hexAt(session, "dday", 1);
  const Out o = run(session, "sctorezone +9, force");
  w2check(o.rc == "0" && o.text == "The following variable(s) have been shifted:  fullstartdate dstart\n"
                                    "- 6 observations shifted forwards by 9 hours.\n",
          "T7/sctorezone: with neither only() nor exclude() every %tc variable, in column order, and "
          "only %tc ones. Got `" + o.text + "`");
  w2check(hexAt(session, "dstart", 6) == "+1.012ab598a0000X+02a",
          "T7/sctorezone: the DOUBLE copy is the exact sum (p1.do AFT6 d=)");
  w2check(hexAt(session, "fullstartdate", 6) == "+1.012ab60000000X+02a",
          "T7/sctorezone: ... while the float copy is rounded to float (p1.do AFT6 f=)");
  w2check(hexAt(session, "ctime", 6) == cPre && hexAt(session, "dday", 1) == dayPre,
          "T7/sctorezone: %tC (case-sensitive format match) and %td variables are not shifted");

  fixture(session);
  (void)run(session, "scalar t7pre = dstart[1]");
  const Out e = run(session, "sctorezone +9, force exclude(dstart)");
  w2check(e.rc == "0" && e.text == "The following variable(s) have been shifted:  fullstartdate\n"
                                    "- 6 observations shifted forwards by 9 hours.\n",
          "T7/sctorezone: exclude(dstart) shifts the other %tc variable only. Got `" + e.text + "`");
  w2check(run(session, "display dstart[1] - scalar(t7pre)").text == "0",
          "T7/sctorezone: the excluded double did not move");

  fixture(session);
  (void)run(session, "scalar t7pre = dstart[1]");
  const Out d = run(session, "sctorezone +9, force only(dstart)");
  w2check(d.rc == "0" && run(session, "display dstart[1] - scalar(t7pre)").text == "32400000",
          "T7/sctorezone: nine hours is 32,400,000 ms on a double, exactly");

  // `ds` of a varlist keeps order AND duplicates (StataBE: `ds b a b, has(...)`
  // answers `b a b`), so only(a a) names and shifts the variable twice.
  fixture(session);
  const Out dup = run(session, "sctorezone +1, force only(dstart fullstartdate dstart)");
  w2check(dup.rc == "0" &&
              dup.text == "The following variable(s) have been shifted:  dstart fullstartdate dstart\n"
                          "- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: only() keeps the user's order and duplicates, as ds does. Got `" + dup.text + "`");

  // An exclude() that removes every variable leaves `ds , has(...)` with an empty
  // list, which means EVERY variable (StataBE-checked primitive).
  fixture(session);
  const Out all = run(session, "sctorezone +1, force exclude(_all)");
  w2check(all.rc == "0" && has(all.text, "shifted:  fullstartdate dstart\n"),
          "T7/sctorezone: exclude(_all) falls through to every variable, as the ado's ds does. Got `" +
              all.text + "`");

  // No %tc variable among the candidates: the plain sentence, data untouched.
  fixture(session);
  const std::string c1 = hexAt(session, "ctime", 1);
  const Out none = run(session, "sctorezone +1, force only(ctime dday)");
  w2check(none.rc == "0" && none.text == "No variables have been shifted.\n" && hexAt(session, "ctime", 1) == c1,
          "T7/sctorezone: only %tC/%td candidates -> `No variables have been shifted.`. Got `" + none.text + "`");
}

void testReportWording(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  auto rep = [&](const std::string& shift) {
    return run(session, "sctorezone " + shift + ", force only(fullstartdate)").text;
  };
  const std::string head = "The following variable(s) have been shifted:  fullstartdate\n";
  // Each line is the StataBE-run levelsof/count/abs report for that shift (cmp run).
  w2check(rep("+1") == head + "- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: |shift| = 1 is singular `hour`");
  w2check(rep("-1") == head + "- 6 observations shifted backwards by 1 hour.\n",
          "T7/sctorezone: -1 is `backwards by 1 hour`");
  w2check(rep("+0.25") == head + "- 6 observations shifted forwards by .25 hour.\n",
          "T7/sctorezone: a fraction under one is singular, no leading zero");
  w2check(rep("+7.2") == head + "- 6 observations shifted forwards by 7.199999809265137 hours.\n",
          "T7/sctorezone: +7.2 is held as a float and reported at 16 digits (levelsof text)");
  w2check(rep("+5.3") == head + "- 6 observations shifted forwards by 5.300000190734863 hours.\n",
          "T7/sctorezone: +5.3 likewise, and its text parses back, so the count is _N");
  w2check(rep("+0.1") == head + "- 0 observations shifted forwards by .1000000014901161 hour.\n",
          "T7/sctorezone: +0.1: levelsof's text does not parse back to the float, so count if == finds 0 "
          "(the ado's own quirk, StataBE-checked)");
  w2check(rep("+0") == head + "- 6 observations not shifted.\n" && rep("-0") == head + "- 6 observations not shifted.\n",
          "T7/sctorezone: a zero shift reads `not shifted`");
  w2check(rep("+0.00001") == head + "- 0 observations shifted forwards by 9.99999974738e-06 hour.\n",
          "T7/sctorezone: a tiny shift is reported in the exponent form StataBE prints");
  w2check(rep("+1e1") == head + "- 6 observations shifted forwards by 10 hours.\n",
          "T7/sctorezone: real() accepts an exponent: +1e1 is 10 hours");
}

void testSyntaxAndShiftErrors(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  const std::string pre1 = hexAt(session, "fullstartdate", 1);
  // `syntax`-line errors: codes checked against StataBE with the same syntax line.
  w2check(rcOf(session, "sctorezone +9, only(fullstartdate)") == "198",
          "T7/sctorezone: no force -> rc 198 (`option force required`, NOT 100)");
  w2check(rcOf(session, "sctorezone, force") == "100",
          "T7/sctorezone: no shift -> rc 100");
  w2check(rcOf(session, "sctorezone +9, fo only(fullstartdate)") == "198",
          "T7/sctorezone: `fo` is not an abbreviation of force -> rc 198");
  w2check(rcOf(session, "sctorezone +9, FORCE only(fullstartdate)") == "198",
          "T7/sctorezone: `FORCE` is not accepted -> rc 198");
  w2check(rcOf(session, "sctorezone +9, force only(nonexistent)") == "111",
          "T7/sctorezone: only(unknown variable) -> rc 111");
  w2check(rcOf(session, "sctorezone +9, force only") == "198",
          "T7/sctorezone: `only` with no parentheses -> rc 198");
  w2check(rcOf(session, "sctorezone +9, force only(fullstartdate-nonexistent)") == "111" &&
              rcOf(session, "sctorezone +9, force only(zz*)") == "111" &&
              rcOf(session, "sctorezone +9, force exclude(nonexistent)") == "111",
          "T7/sctorezone: a bad range end, a wildcard matching nothing and an unknown exclude() item "
          "are all rc 111 (StataBE: `variable zz* not found (error in option only())`)");
  // The ado's own `error 1` checks: rc 1 with its wording.
  Out e = run(session, "sctorezone 9, force");
  w2check(e.rc == "1" && has(msgOf(e), "Shift must either be a string variable or in the form '+3' or '-4'."),
          "T7/sctorezone: a shift without a sign -> rc 1 and the ado's message. Got " + e.rc + " " + msgOf(e));
  e = run(session, "sctorezone +abc, force");
  w2check(e.rc == "1" && has(msgOf(e), "in the form '+3' or '-4'"),
          "T7/sctorezone: an unparsable shift -> rc 1");
  e = run(session, "sctorezone +1e39, force");
  w2check(e.rc == "1" && has(msgOf(e), "in the form '+3' or '-4'"),
          "T7/sctorezone: a shift beyond the float range is missing in the tempvar -> rc 1 (StataBE: real(\"1e39\") is . as float)");
  e = run(session, "sctorezone +24, force");
  w2check(e.rc == "1" && has(msgOf(e), "between +/-24 (exclusive)"),
          "T7/sctorezone: |shift| = 24 -> rc 1, `between +/-24 (exclusive)`");
  w2check(rcOf(session, "sctorezone -24, force") == "1" && rcOf(session, "sctorezone +100, force") == "1",
          "T7/sctorezone: -24 and +100 -> rc 1");
  w2check(rcOf(session, "sctorezone +23.9999999, force") == "1",
          "T7/sctorezone: 23.9999999 is held as the float 24 -> rc 1 (StataBE: the tempvar reads 24)");
  e = run(session, "sctorezone +9, force only(fullstartdate) exclude(dstart)");
  w2check(e.rc == "1" && has(msgOf(e), "not both"),
          "T7/sctorezone: only() together with exclude() -> rc 1, `not both`");
  w2check(hexAt(session, "fullstartdate", 1) == pre1,
          "T7/sctorezone: every refused or failed call left the data untouched");
}

void testRefusalsByName(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  (void)run(session, "generate str12 stext = \"12:34:56 PM\"");
  (void)run(session, "generate str6 plain = \"abc\"");
  (void)run(session, "generate str6 empty = \"\"");
  const std::string pre1 = hexAt(session, "fullstartdate", 1);

  // Automatic mode: the shift is the NAME of a string variable.
  Out a = run(session, "sctorezone plain, force");
  w2check(a.rc == "199" && a.error.value("reason", std::string()).rfind("refused:", 0) == 0 &&
              has(msgOf(a), "not implemented"),
          "T7/sctorezone: automatic mode (shift = a string variable) is refused by name, rc 199, "
          "message says not implemented. Got " + a.rc + " " + msgOf(a));
  w2check(hexAt(session, "fullstartdate", 1) == pre1, "T7/sctorezone: the automatic-mode refusal changed nothing");
  w2check(rcOf(session, "sctorezone fullstartdate, force") == "1",
          "T7/sctorezone: a NUMERIC variable name is not automatic mode: the ado reads it as a manual shift -> rc 1");

  // A SurveyCTO string time among the candidates: refused before anything moves.
  Out s = run(session, "sctorezone +1, force");
  w2check(s.rc == "199" && s.error.value("reason", std::string()).rfind("refused:", 0) == 0 &&
              has(msgOf(s), "stext") && has(msgOf(s), "not implemented"),
          "T7/sctorezone: a string time variable in the candidate set is refused by name, naming it. Got " + s.rc +
              " " + msgOf(s));
  w2check(hexAt(session, "fullstartdate", 1) == pre1, "T7/sctorezone: the string-time refusal changed nothing");
  // Restricting the command to the %tc variable is fine; the strings that do not
  // qualify (non-matching, all missing) are skipped silently when swept.
  Out ok = run(session, "sctorezone +1, force only(fullstartdate)");
  w2check(ok.rc == "0" && has(ok.text, "shifted:  fullstartdate\n"),
          "T7/sctorezone: only(%tc varlist) avoids the refused branch");
  (void)run(session, "drop stext");
  Out sweep = run(session, "sctorezone +1, force");
  w2check(sweep.rc == "0" && has(sweep.text, "shifted:  fullstartdate dstart\n"),
          "T7/sctorezone: strings that never match *:??:?? ?M, or are all missing, are skipped (rc 0)");
}

void testAbbreviationsAndEmptyOptions(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  const std::string head = "The following variable(s) have been shifted:  ";
  w2check(run(session, "sctorezone +1, force onl(dstart)").text == head + "dstart\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: onl(...) reaches the handler as only(...)");
  w2check(run(session, "sctorezone +1, force o(dstart)").text == head + "dstart\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: o(...) is the one-letter abbreviation of only()");
  w2check(run(session, "sctorezone +1, force e(dstart)").text == head + "fullstartdate\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: e(...) is the one-letter abbreviation of exclude()");
  // StataBE: `only()` with nothing inside is the same as no only() at all.
  w2check(run(session, "sctorezone +1, force only()").text == head + "fullstartdate dstart\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: only() with nothing inside is no only()");
  // A variable abbreviation inside only(), as `syntax Only(varlist)` allows.
  w2check(run(session, "sctorezone +1, force only(dst)").text == head + "dstart\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: a unique variable abbreviation inside only() names the variable");
}

void testQualifiersAndSetType(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  const std::string head = "The following variable(s) have been shifted:  ";
  // `[if] [in]` are in the ado's syntax line but never read: an `if` is accepted
  // and every row moves (read from the ado; the tokenizer lets `if` through).
  const Out q = run(session, "sctorezone +1 if _n == 1, force only(dstart)");
  w2check(q.rc == "0" && q.text == head + "dstart\n- 6 observations shifted forwards by 1 hour.\n",
          "T7/sctorezone: an `if` qualifier is accepted and ignored, as the ado ignores it. Got " + q.rc +
              " `" + q.text + "`");
  // `in` is refused by the tokenizer's in-range allow-list (registry allowInRange=false):
  // a loud refusal, never a silent partial shift.
  fixture(session);
  const std::string pre6 = hexAt(session, "dstart", 6);
  const Out in = run(session, "sctorezone +1 in 1/2, force only(dstart)");
  w2check(in.rc != "0" && hexAt(session, "dstart", 6) == pre6,
          "T7/sctorezone: an `in` qualifier is refused loudly and nothing moves (known divergence). Got " + in.rc);

  // `gen shifttime = real(...)` follows `set type`: under double the tempvar holds
  // 0.1 exactly, its text (.1) parses back, and the count is _N (StataBE:
  // t7l3_settype.do -> L=[.1] count=N, 5.3 -> L=[5.3]).
  fixture(session);
  (void)run(session, "set type double");
  const Out d1 = run(session, "sctorezone +0.1, force only(dstart)");
  const Out d2 = run(session, "sctorezone +5.3, force only(dstart)");
  (void)run(session, "set type float");
  w2check(d1.text == head + "dstart\n- 6 observations shifted forwards by .1 hour.\n" &&
              d2.text == head + "dstart\n- 6 observations shifted forwards by 5.3 hours.\n",
          "T7/sctorezone: under `set type double` the shift is a double and the report counts every row. Got `" +
              d1.text + "` / `" + d2.text + "`");
}

void testEmptyDataset(const std::string& workdir) {
  piet::SessionState session(workdir);
  fixture(session);
  (void)run(session, "drop if _n > 0");
  // With no observations the ado's generated tempvar is empty, its assert/count
  // checks are vacuous, nothing is replaced, and the report has only its first line.
  const Out o = run(session, "sctorezone +9, force only(fullstartdate)");
  w2check(o.rc == "0" && o.text == "The following variable(s) have been shifted:  fullstartdate\n",
          "T7/sctorezone: a dataset with no observations prints only the first report line (read from the ado). Got " +
              o.rc + " `" + o.text + "`");
  w2check(rcOf(session, "sctorezone 9, force") == "1",
          "T7/sctorezone: the sign check does not depend on the data");
}

}  // namespace

void testW2T7Sctorezone() {
  std::printf("[selftest] --- T7 sctorezone (SSC scto): manual %%tc shifts, refusals by name ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_t7sctorezone").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  testCorpusForm(workdir);
  testSweepAndExclude(workdir);
  testReportWording(workdir);
  testSyntaxAndShiftErrors(workdir);
  testRefusalsByName(workdir);
  testAbbreviationsAndEmptyOptions(workdir);
  testQualifiersAndSetType(workdir);
  testEmptyDataset(workdir);
  fs::remove_all(workdir, ec);
}
