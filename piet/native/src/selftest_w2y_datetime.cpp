// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2Y selftest lane: the COMMON_GE2 date/time functions (goal
// ge2-fn-uncovered-datetime). Every arm replays a statement shape of the captured
// case w2y_fn_datetime (StataBE 19.5, landed as
// tests/eval/golden/aer_w2/w2y_fn_datetime.{do,log}) or of its rule probe
// (internal notes w2y_fn_datetime_probe, p_* keys), and names
// the record it rests on. Each arm failed on the pre-change binary; the probes
// internal probe BUG-908..911.do replay the same shapes through
// `piet_native --do`.
//
// Layers: `display`/`local =`/`scalar =` go through the macro/arith layer
// (macros.cpp evalArith over exprfns.cpp's registry); generate/replace of a
// string argument through PIET's row evaluator (strmutate.cpp over the same
// registry); generate of numbers through the engine (frame.cpp), whose calendar
// is the shared src/data/stata_dates.h that tests/unit/test_d2.cpp pins.

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
          "W2Y " + label + ": `" + cmd + "` -> [" + want + "] -- got rc " + r.rc + " [" + r.text +
              "]");
}

void rcArm(piet::SessionState& s, const std::string& cmd, const std::string& rc,
           const std::string& label) {
  const Reply r = run(s, cmd);
  w2check(r.rc == rc, "W2Y " + label + ": `" + cmd + "` -> r(" + rc + ") -- got rc " + r.rc);
}

}  // namespace

void testW2yDatetime() {
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2ydatetime").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState s(workdir);
  (void)run(s, "clear");

  // --- TCL / BUG-446: a clock literal's ':' reached the whitelist and the
  // expression echoed (display) or was r(198) (local).
  displayArm(s, "display hh(tc(10:30)) \"|\" mm(tc(10:30)) \"|\" minutes(tc(10:30))", "10|30|630",
             "T01 clock literal inside hh/mm/minutes (fd_hhmm_edges f11 f12, fd_minutes_edges f9)");
  displayArm(s, "display tc(11:00) \"|\" tc(10:30am) \"|\" tc(10:30 pm)",
             "39600000|37800000|81000000",
             "T02 clock literals, meridian honoured (fd_tc_edges f3; probe p_lit_16, p_lit_17)");
  {
    const Reply r = run(s, "local w446 = tc(12:00)");
    const std::string got = s.eval.locals.get("w446", "?");
    w2check(r.rc == "0" && got == "43200000",
            "W2Y T03 `local w = tc(12:00)` -> 43200000 (BUG-446, b3 oracle tc.timeonly) -- got rc " +
                r.rc + " [" + got + "]");
  }

  // --- MISS: hh/mm/minutes of a missing argument computed on the sentinel.
  displayArm(s, "display hh(.) \"|\" mm(.) \"|\" minutes(.)", ".|.|.",
             "M01 hh(.) mm(.) minutes(.) are . (fd_hhmm_edges f9 f10, fd_minutes_edges f7)");

  // --- LITR / D1: Stata refuses these with r(198) on every surface.
  rcArm(s, "scalar w2y_tq = tq(2010q5)", "198", "L01 scalar = tq(2010q5) (fd_tq_bad)");
  rcArm(s, "scalar w2y_tm = tm(2010m13)", "198", "L02 scalar = tm(2010m13) (fd_tm_bad)");
  rcArm(s, "scalar w2y_d1 = date(\"01jan2010\")", "198",
        "L03 scalar = one-argument date() (fd_date1)");

  // --- the shared calendar, through the macro/arith layer.
  displayArm(s, "display ym(2000,2.5) \"|\" ym(2000.5,2) \"|\" yq(2020.5,3) \"|\" ym(100,1)",
             "481.5|487|244|.", "C01 ym/yq are linear, years from 1000 (fd_ym_edges f7 f8 f12; fd_yq_real)");
  displayArm(s, "display mdy(1,1.9,2000) \"|\" mdy(1,1,99) \"|\" year(2936550) \"|\" dow(2936550)",
             "14610|.|.|6", "C02 mdy truncates; calendar 0100..9999 (fd_mdy_edges2; p_mdy_99, "
             "p_year_2936550, p_dow_2936550)");

  // --- the mask rules (stata_datemask.h).
  displayArm(s,
             "display date(\"1/15/2020\",\"MDY#\") \"|\" date(\"1/15/2020\",\"#MDY\") \"|\" "
             "date(\"1/15/2020 1 2\",\"MDY#\")",
             "21929|.|21929", "K01 '#' ends the mask: rest ignored (fd_date_hash f13; p_date_h9, h3)");
  displayArm(s,
             "display hh(clock(\"1/15/2020 10:30 PM\",\"MDYhm#\")) \"|\" "
             "hh(clock(\"1/15/2020 10:30 PM\",\"MDYhm\")) \"|\" "
             "hh(clock(\"2020-01-15 13:30 pm\",\"YMDhm\"))",
             "10|22|13", "K02 meridian: '#' swallows it, a leftover one is honoured (p_clock_c5, c4, c26)");
  displayArm(s,
             "display date(\"Jan152010\",\"MDY\") \"|\" date(\"152010jan\",\"DYM\") \"|\" "
             "mm(clock(\"20200115 103045\",\"YMDhms\"))",
             "18277|18277|30", "K03 packed digits split (fd_date_edges f5; p_date_d9; fd_clock_ymdhms)");
  displayArm(s,
             "display date(\"2017m6\",\"YM\") \"|\" clock(\"2020-01-15T10:30:45\",\"YMDhms\") \"|\" "
             "date(\"15jan2010x\",\"DMY\")",
             ".|.|.", "K04 date/clock drop no letters (fd_date_ym; fd_clock_ymdhms f9; p_date_d16)");
  displayArm(s,
             "display monthly(\"2017m6\",\"YM\") \"|\" monthly(\"2017m 6\",\"YM\") \"|\" "
             "quarterly(\"2010 q 3\",\"YQ\") \"|\" quarterly(\"1q2010\",\"QY\") \"|\" "
             "monthly(\"201706\",\"YM\")",
             "689|.|.|200|.", "K05 period scale letter touches the period (p_monthly_m1 m3 m19; "
             "fd_quarterly_edges; p_quarterly_q5)");
  displayArm(s, "display quarterly(\"2010q3\",\"yq\") \"|\" monthly(\"2017-06\",\"ym\")", "202|689",
             "K06 period masks fold case (fd_quarterly_lc; p_monthly_m6)");
  displayArm(s,
             "display monthly(\"6 2017\",\"M20Y\") \"|\" date(\"15jan2017\",\"DM19Y\") \"|\" "
             "date(\"15 1 017\",\"DM20Y\")",
             "689|20834|.", "K07 19Y/20Y take a 4-digit year as written (fd_monthly_m20y; "
             "p_date_d13, d14)");
  displayArm(s,
             "display date(\"1/15/2020 24:00\",\"MDYhm\") \"|\" date(\"15 sept 2010\",\"DMY\") \"|\" "
             "date(\"15 juneteenth 2010\",\"DMY\")",
             ".|18520|.", "K08 date() validates the time; month-name prefixes (fd_date_mdyhm; "
             "p_date_d23, d24)");

  // --- generate: the row evaluator and the engine.
  (void)run(s, "set obs 2");
  (void)run(s, "generate str20 w2ys = \"1/15/2020 23:59\"");
  rcArm(s, "generate double w2y_g1 = tq(2010q5)", "198", "G01 generate = tq(2010q5) (fd_tq_bad gen_rc)");
  rcArm(s, "generate double w2y_g2 = td(31feb2010)", "198",
        "G02 generate = td(31feb2010) (fd_td_bad gen_rc)");
  rcArm(s, "generate double w2y_g3 = date(\"01jan2010\")", "198",
        "G03 generate = one-argument date() (fd_date1 gen_rc)");
  // The corpus shape (007: gen fullstartdate = clock(...), then gen minute = mm(...)),
  // beside the same store into double, whose minute stays 59.
  (void)run(s, "generate w2y_clk = clock(w2ys, \"MDYhm\")");
  (void)run(s, "generate w2y_mm = mm(w2y_clk)");
  (void)run(s, "generate double w2y_clkd = clock(w2ys, \"MDYhm\")");
  (void)run(s, "generate w2y_mmd = mm(w2y_clkd)");
  displayArm(s, "display w2y_mm[1] \"|\" w2y_mmd[1]", "58|59",
             "G04 an untyped clock() variable is stored as float, so its minute is 58; double keeps 59 "
             "(fd_mm_float f5; fd_clock_mdyhm_f f5 = 1894751928320; fd_clock_mdyhm_d)");
  (void)run(s, "generate double w2y_ym = ym(100, 1)");
  displayArm(s, "display w2y_ym[1]", ".", "G05 engine ym(100,1) is . (fd_ym_edges f12; p_ym_100_1)");

  fs::remove_all(workdir, ec);
}
