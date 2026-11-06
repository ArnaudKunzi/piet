// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2D lane `inrange` (2026-09-27) -- the `in` range qualifier ([U] 11.1.4) on
// keep/drop/list/summarize/replace/generate/count/egen, all read through ONE
// resolver (inrange_filter.cpp parseInRangeBounds). BUG-672: from-the-end
// bounds (`in -3/-1`) were refused (keep/drop/summarize/gen/replace, r(198)) or
// clamped to row 1 (list). BUG-673: a bound past _N was clamped (`keep in 1/50`
// on 12 rows kept all 12, rc 0). BUG-674: `list if EXP in a/b` windowed the
// if-filtered rows. BUG-622 (its egen.cpp sites only): `gen/replace/egen ...
// if EXP in a/b` read a missing EXP as false.
// Oracle: tests/eval/golden/aer_w2/w2d_uncovered.log, arms in_* (log lines
// 5101-5459, cited per row). Each arm replays its stanza of w2d_uncovered.do on
// the `w2d_mkin` fixture (x = 1..12, v = 0) in a fresh session and asserts rc
// AND every numeric field of the PREC line. in_keep_neg3_neg1 (main: rc 198)
// and in_keep_out_of_range (main: rc 0) are the seeded-RED arms. The rows
// after the oracle table pin the siblings the oracle does not print (listed
// rows, the if+in compositions) against [U] 11.1.4's own rule, and the
// refusal shapes StataBE 19.5 answered live with r(198) (no golden yet): rc
// only, plus the data left untouched.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gInRangeChecks = 0;
void irCheck(bool cond, const std::string& label) {
  ++gInRangeChecks;
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

// Text of `display EXP` (empty when the line errored or displayed nothing).
std::string shown(piet::SessionState& session, const std::string& exp) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch("display " + exp, session)) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
  } catch (const std::exception&) {
  }
  return text;
}

// First column of every row a `list` line shows, as integers ("" if the line
// showed no table). Values are the fixture's own integers.
std::string listed(piet::SessionState& session, const std::string& cmd) {
  std::string out;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object() || !b.contains("rows") || !b["rows"].is_array()) continue;
      for (const auto& row : b["rows"]) {
        if (!row.is_array() || row.empty() || !row[0].is_number()) {
          out += "?,";
          continue;
        }
        out += std::to_string(static_cast<long long>(row[0].get<double>())) + ",";
      }
    }
  } catch (const std::exception& e) {
    out = std::string("EXC:") + e.what();
  }
  return out;
}

// w2d_uncovered.do's `w2d_mkin`. The program's `ereturn clear` / return clear
// are a fresh session's state already.
void mkin(piet::SessionState& session) {
  for (const char* s0 : {"clear", "set obs 12", "generate x = _n", "generate v = 0"})
    (void)rcOf(session, s0);
}

// What the stanza reads after the command.
enum class Post {
  kSumX,     // quietly summarize x -> _N | r(sum) [| x[1]]
  kNone,     // _N
  kSumm,     // the command itself was summarize -> r(N) | r(mean)
  kSumV,     // quietly summarize v -> r(sum) | v[11]
  kVstat,    // w2d_vstat label -> r(nmiss) | r(sum)
};

struct Arm {
  const char* tag;
  int logLine;
  const char* pre;  // "" = none
  const char* cmd;
  Post post;
  bool withX1;  // kSumX: the record also prints x[1]
  const char* want;  // the PREC fields after the arm id, `|`-joined, %18.0g padding trimmed
};

std::string recordOf(const std::string& workdir, const Arm& a) {
  piet::SessionState session(workdir);
  mkin(session);
  if (*a.pre) (void)rcOf(session, a.pre);
  std::string rec = rcOf(session, a.cmd) + "|";
  switch (a.post) {
    case Post::kSumX:
      (void)rcOf(session, "quietly summarize x");
      rec += shown(session, "_N") + "|" + shown(session, "r(sum)") + "|";
      if (a.withX1) rec += shown(session, "x[1]") + "|";
      break;
    case Post::kNone:
      rec += shown(session, "_N") + "|";
      break;
    case Post::kSumm:
      rec += shown(session, "r(N)") + "|" + shown(session, "r(mean)") + "|";
      break;
    case Post::kSumV:
      (void)rcOf(session, "quietly summarize v");
      rec += shown(session, "r(sum)") + "|" + shown(session, "v[11]") + "|";
      break;
    case Post::kVstat:
      (void)rcOf(session, "quietly count if missing(label)");
      rec += shown(session, "r(N)") + "|";
      (void)rcOf(session, "quietly summarize label");
      rec += shown(session, "r(sum)") + "|";
      break;
  }
  return rec;
}

}  // namespace

void testW2dInRange() {
  std::printf("[selftest] --- W2D inrange: the `in` range qualifier (w2d_uncovered.log in_*) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2dinrange").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // One row per PREC record of w2d_uncovered.log, fields copied verbatim.
  static const Arm kArms[] = {
      {"in_drop_1_3_094", 5101, "", "drop in 1/3", Post::kSumX, true, "0|9|72|4|"},
      {"in_drop_if_ctl", 5117, "", "drop if _n <= 3", Post::kSumX, true, "0|9|72|4|"},
      {"in_keep_1_091", 5133, "", "keep in 1", Post::kSumX, false, "0|1|1|"},
      {"in_keep_1_8_017", 5149, "", "keep in 1/8", Post::kSumX, false, "0|8|36|"},
      {"in_keep_if_ctl", 5165, "", "keep if _n <= 8", Post::kSumX, false, "0|8|36|"},
      {"in_drop_a_l_015", 5181, "", "drop in 8/l", Post::kSumX, false, "0|7|28|"},
      {"in_drop_1_macro_004", 5199, "local todrop = 4", "drop in 1/`todrop'", Post::kSumX,
       false, "0|8|68|"},
      // BUG-672 seeded-RED: main refused the from-the-end bound (rc 198, _N 12).
      {"in_keep_neg3_neg1", 5215, "", "keep in -3/-1", Post::kSumX, false, "0|3|33|"},
      {"in_drop_single_5", 5231, "", "drop in 5", Post::kSumX, false, "0|11|73|"},
      // BUG-673 seeded-RED: main clamped to _N and answered rc 0.
      {"in_keep_out_of_range", 5245, "", "keep in 1/50", Post::kNone, false, "198|12|"},
      {"in_list_l_1_5_044", 5268, "", "l in 1/5", Post::kNone, false, "0|12|"},
      {"in_list_1_10_079", 5297, "", "list x in 1/10", Post::kNone, false, "0|12|"},
      {"in_list_ctl", 5329, "", "list x", Post::kNone, false, "0|12|"},
      {"in_summarize_2_6", 5346, "", "summarize x in 2/6", Post::kSumm, false, "0|5|4|"},
      {"in_summarize_f_l", 5363, "", "summarize x in f/l", Post::kSumm, false, "0|12|6.5|"},
      {"in_summarize_ctl", 5380, "", "summarize x", Post::kSumm, false, "0|12|6.5|"},
      {"in_replace_single_017", 5396, "", "replace v = 8 in 11", Post::kSumV, false, "0|8|8|"},
      {"in_replace_eqN_059", 5412, "", "replace v = 1 in `=_N-1'", Post::kSumV, false,
       "0|1|1|"},
      {"in_replace_ctl", 5428, "", "replace v = 8", Post::kSumV, false, "0|96|8|"},
      {"in_generate_1_9_016", 5444, "", "gen label = _n in 1/9", Post::kVstat, false,
       "0|3|45|"},
      {"in_generate_ctl", 5459, "", "gen label = _n", Post::kVstat, false, "0|0|78|"},
  };
  for (const Arm& a : kArms) {
    const std::string got = recordOf(workdir, a);
    irCheck(got == a.want, std::string("W2D inrange ") + a.tag + " (w2d_uncovered.log:" +
                               std::to_string(a.logLine) + "): `" + a.cmd + "` -> " + a.want +
                               " -- got " + got);
  }

  // Siblings the oracle's fields cannot see, same fixture. The from-the-end rule
  // is [U] 11.1.4's (-1 = the last observation); the if+in rule is its AND over
  // the ORIGINAL observation numbers.
  {
    // BUG-672 on list: main listed row 1 only (its parser clamped -3 to 1).
    piet::SessionState s(workdir);
    mkin(s);
    const std::string rows = listed(s, "list x in -3/-1");
    irCheck(rows == "10,11,12,", "W2D inrange list: `list x in -3/-1` lists 10,11,12 -- got " + rows);
  }
  {
    // BUG-674: main listed the first five if-matches (3..7).
    piet::SessionState s(workdir);
    mkin(s);
    const std::string rows = listed(s, "list x if x > 2 in 1/5");
    irCheck(rows == "3,4,5,", "W2D inrange list: `list x if x > 2 in 1/5` lists 3,4,5 -- got " + rows);
  }
  {
    // BUG-673 on list/summarize/count/gen/egen: each clamped `1/50` and
    // answered rc 0; the oracle rule (w2d_uncovered.log:5245) is r(198).
    for (const char* cmd : {"list x in 1/50", "summarize x in 1/50", "count in 1/50",
                            "gen w = 1 in 1/50", "replace v = 1 in 1/50",
                            "egen t = total(x) in 1/50", "drop in 13"}) {
      piet::SessionState s(workdir);
      mkin(s);
      const std::string rc = rcOf(s, cmd);
      const std::string n = shown(s, "_N");
      (void)rcOf(s, "quietly summarize v");
      const std::string sv = shown(s, "r(sum)");
      irCheck(rc == "198" && n == "12" && sv == "0",
              std::string("W2D inrange out-of-range: `") + cmd +
                  "` is r(198) with the data untouched -- got rc " + rc + ", _N " + n +
                  ", sum(v) " + sv);
    }
  }
  {
    // BUG-672 on the other consumers (main: r(198) "not supported yet").
    struct F {
      const char* cmd;
      const char* post;
      const char* want;
    };
    for (const F& f : {F{"drop in -2/l", "_N", "10"}, F{"count in -5/-1", "r(N)", "5"},
                       F{"summarize x in -4/-2", "r(sum)", "30"},
                       F{"replace v = 1 in -1", "v[12]", "1"},
                       F{"gen w = _n in f/-11", "w[2]", "2"}}) {
      piet::SessionState s(workdir);
      mkin(s);
      const std::string rc = rcOf(s, f.cmd);
      const std::string got = shown(s, f.post);
      irCheck(rc == "0" && got == f.want, std::string("W2D inrange from-the-end: `") + f.cmd +
                                              "` -> " + f.post + " = " + f.want + " -- got rc " +
                                              rc + ", " + got);
    }
    // `gen w = _n in f/-11` stops at row 2: row 3 stays missing.
    piet::SessionState s(workdir);
    mkin(s);
    (void)rcOf(s, "gen w = _n in f/-11");
    (void)rcOf(s, "quietly count if missing(w)");
    const std::string nm = shown(s, "r(N)");
    irCheck(nm == "10", "W2D inrange from-the-end: `gen w = _n in f/-11` leaves 10 missing -- got " + nm);
  }
  {
    // BUG-622 (egen.cpp sites): gen/replace/egen `if EXP in a/b` with EXP
    // missing on one in-range row. Stata's `if` reads a missing condition as
    // true; main left row 2 out.
    piet::SessionState s(workdir);
    mkin(s);
    (void)rcOf(s, "generate z = x");
    (void)rcOf(s, "replace z = . in 2");
    (void)rcOf(s, "gen w = 1 if z in 1/4");
    (void)rcOf(s, "quietly count if w == 1");
    const std::string ng = shown(s, "r(N)");
    (void)rcOf(s, "replace v = 1 if z in 1/4");
    (void)rcOf(s, "quietly summarize v");
    const std::string sr = shown(s, "r(sum)");
    (void)rcOf(s, "egen t = total(x) if z in 1/4");
    const std::string et = shown(s, "t[2]");
    irCheck(ng == "4" && sr == "4" && et == "10",
            "W2D inrange if+in: a missing `if` is true under `in 1/4` (gen 4 rows, replace 4 "
            "rows, egen total 10 on row 2) -- got " + ng + ", " + sr + ", " + et);
  }
  {
    // Shapes StataBE 19.5 refused live with r(198) (not yet golden): `0`,
    // `-0`, a from-the-end bound before row 1, and a first bound after the
    // last. rc and untouched data only.
    for (const char* cmd : {"keep in 0", "keep in -0", "keep in 0/3", "keep in -13/-1",
                            "keep in 5/3", "keep in -3/3", "list x in 5/3"}) {
      piet::SessionState s(workdir);
      mkin(s);
      const std::string rc = rcOf(s, cmd);
      const std::string n = shown(s, "_N");
      irCheck(rc == "198" && n == "12", std::string("W2D inrange refusal: `") + cmd +
                                            "` is r(198), _N 12 -- got rc " + rc + ", _N " + n);
    }
  }

  // FU-35 (BUG-828): an unknown name in a row expression is r(111) in every
  // command that evaluates one. w2n_followups.log E12 :1056, E13 :1067,
  // E16 :1101, E17 :1111, E18 :1121, E19 :1131, E20 :1141, E21 :1151,
  // E22 :1161 (all PREC rc 111, message `nosuchvar not found`); main answered
  // rc 1 for each because coarseRc only knew "variable ... not found". The
  // data is untouched afterwards (keep/drop must not have run), and E12's
  // r(N) survives the failed count (log :1056 PREC E12|111|15).
  {
    struct U {
      const char* tag;
      int logLine;
      const char* cmd;
    };
    for (const U& u : {U{"E12", 1056, "count if nosuchvar > 0"},
                       U{"E13", 1067, "assert nosuchvar > 0"},
                       U{"E16", 1101, "generate z = nosuchvar + 1"},
                       U{"E17", 1111, "replace v = nosuchvar"},
                       U{"E18", 1121, "keep if nosuchvar > 0"},
                       U{"E19", 1131, "drop if nosuchvar > 0"},
                       U{"E20", 1141, "summarize x if nosuchvar > 0"},
                       U{"E21", 1151, "count if nosuchvar"},
                       U{"E22", 1161, "count if x > nosuchvar"}}) {
      piet::SessionState s(workdir);
      mkin(s);
      (void)rcOf(s, "quietly count if x > 9");
      const std::string rc = rcOf(s, u.cmd);
      const std::string n = shown(s, "_N");
      const std::string rn = shown(s, "r(N)");
      const bool rnOk = std::string(u.tag) != "E12" || rn == "3";
      irCheck(rc == "111" && n == "12" && rnOk,
              std::string("FU-35 ") + u.tag + " (w2n_followups.log:" + std::to_string(u.logLine) +
                  "): `" + u.cmd + "` is r(111), _N 12 (E12: r(N) 3 kept) -- got rc " + rc +
                  ", _N " + n + ", r(N) " + rn);
    }
    // Controls E14 :1078 (0|59|) and E15 :1090 (9|): a known name still works.
    piet::SessionState s(workdir);
    mkin(s);
    const std::string rcCount = rcOf(s, "count if x > 0");
    const std::string rnCount = shown(s, "r(N)");
    const std::string rcAssert = rcOf(s, "assert x > 1e9");
    irCheck(rcCount == "0" && rnCount == "12" && rcAssert == "9",
            "FU-35 controls E14/E15 (w2n_followups.log:1078,1090): `count if x > 0` rc 0 r(N) 12, "
            "`assert x > 1e9` rc 9 -- got " + rcCount + ", " + rnCount + ", " + rcAssert);
  }

  // FU-169 (BUG-829): `_rc` is readable in a count/assert expression.
  // aer_estim_oracle.log:412-414 (`capture confirm matrix e(sds)` then
  // `assert _rc == 0` passes silently) and :516-521 (_rc 111 displayed at :519,
  // then `assert _rc == 111` passes silently). Main refused both with
  // `expr: unknown variable _rc` (rc 1).
  {
    piet::SessionState s(workdir);
    mkin(s);
    (void)rcOf(s, "capture confirm variable x");
    const std::string a0 = rcOf(s, "assert _rc == 0");
    (void)rcOf(s, "capture confirm variable nosuchvar");
    const std::string shownRc = shown(s, "_rc");
    const std::string a111 = rcOf(s, "assert _rc == 111");
    (void)rcOf(s, "count if _rc == 111");
    const std::string c111 = shown(s, "r(N)");
    (void)rcOf(s, "count if _rc == 0");
    const std::string c0 = shown(s, "r(N)");
    const std::string aWrong = rcOf(s, "assert _rc == 0");
    irCheck(a0 == "0" && shownRc == "111" && a111 == "0" && aWrong == "9" && c111 == "12" &&
                c0 == "0",
            "FU-169 (aer_estim_oracle.log:414,519-521): `assert _rc == 0` after rc 0 passes; after "
            "rc 111 `assert _rc == 111` passes, `assert _rc == 0` is r(9), count if _rc == 111 -> 12 "
            "-- got " + a0 + "," + shownRc + "," + a111 + "," + aWrong + "," + c111 + "," + c0);
  }
  {
    // Whole identifier only: a longer name and a dataset variable named _rc
    // are the engine's own, not the system value.
    piet::SessionState s(workdir);
    mkin(s);
    (void)rcOf(s, "generate my_rc = 5");
    (void)rcOf(s, "capture confirm variable nosuchvar");
    const std::string rcLong = rcOf(s, "assert my_rc == 5");
    // BUG-1120: no variable may be named _rc ("_rc invalid varname" r(198), StataBE
    // 19.5; this arm used to create one), so `_rc` stays the system value.
    (void)rcOf(s, "capture generate _rc = 7");
    const std::string rcVar = rcOf(s, "assert _rc == 198");
    irCheck(rcLong == "0" && rcVar == "0",
            "FU-169: `my_rc` is not `_rc`, and `generate _rc` is refused r(198), leaving `_rc` "
            "the system value -- got " + rcLong + "," + rcVar);
  }

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2D inrange: %d checks executed\n", gInRangeChecks);
}
