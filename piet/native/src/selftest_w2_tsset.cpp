// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `tsset` -- selftest arms for goal tsset-panel-tsfill-full (BUG-896,
// BUG-897, BUG-898): `tsset panelvar timevar [, yearly|delta()]`, the bare
// re-display, tsset.ado's r() set, L./F./D. within panels after a panel tsset,
// `tsfill [, full]`, and the refusal rcs.
//
// Oracle (StataBE 19.5): tests/eval/golden/aer_w2/w2w_tsset_tsfill.do / .log.
// Fixture A = w2w_mk (14 obs, 4 unbalanced panels, str2 s, yr = 1999 + t),
// fixture B = w2w_mk2 (delta(2) panels), both verbatim from the do-file.
// On the frozen main binary every panel-tsset arm was r(198) "unexpected extra
// tokens after time variable" (FU-581), r(unit) after `tsset t` was a named
// r(111), `tsset t` on repeated times was accepted at rc 0, `tsfill, full` was
// refused by name, and the tsfill refusals were r(198) where Stata says
// 111/101/451.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

namespace fs = std::filesystem;

struct TsLine {
  std::string rc = "0";
  std::string msg;
  std::string echo;  // the last echo block that is not the command's own `.  cmd` line
};

TsLine tsRun(const std::string& cmd, piet::SessionState& s) {
  TsLine r;
  for (const auto& b : piet::dispatch(cmd, s)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") {
      r.rc = piet::eval::coarseRc(b);
      r.msg = b.value("message", std::string());
    } else if (type == "echo") {
      const std::string t = b.value("text", std::string());
      if (t.rfind(".  ", 0) != 0) r.echo = t;
    }
  }
  return r;
}

void mkA(piet::SessionState& s) {
  for (const char* c :
       {"clear", "set obs 14", "generate id = real(word(\"1 1 1 1 1 2 2 2 3 3 3 3 3 4\", _n))",
        "generate t = real(word(\"1 2 3 4 5 3 4 6 1 2 5 7 8 2\", _n))",
        "generate double y = mod(_n*_n*1854 + _n*5353, 10007)/10007",
        "generate double x = mod(_n*_n*733 + _n*2711, 10007)/10007", "generate yr = 1999 + t",
        "generate str2 s = word(\"aa bb cc dd\", id)"})
    (void)piet::dispatch(c, s);
}

void mkB(piet::SessionState& s) {
  for (const char* c :
       {"clear", "set obs 9", "generate id = real(word(\"1 1 1 1 2 2 2 3 3\", _n))",
        "generate t = real(word(\"2 4 6 10 4 6 8 2 8\", _n))",
        "generate double y = mod(_n*_n*1854 + _n*5353, 10007)/10007",
        "generate double x = mod(_n*_n*733 + _n*2711, 10007)/10007",
        "generate str2 s = word(\"aa bb cc\", id)"})
    (void)piet::dispatch(c, s);
}

std::string rs(const piet::SessionState& s, const char* k) {
  const std::string* v = s.eval.rStrings.find(k);
  return v == nullptr ? std::string("<absent>") : *v;
}

double rv(const piet::SessionState& s, const char* k) {
  const double* v = s.eval.rValues.find(k);
  return v == nullptr ? -999.0 : *v;
}

int64_t nRows(piet::SessionState& s) {
  piet::Datastore* ds = s.datastore();
  return ds == nullptr ? -1 : ds->nRows();
}

int64_t nMissing(piet::SessionState& s, const std::string& var) {
  piet::Datastore* ds = s.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "f64") return -1;
  const double* v = ds->col(var).f64();
  int64_t n = 0;
  for (int64_t i = 0; i < ds->nRows(); ++i)
    if (std::isnan(v[i])) ++n;
  return n;
}

std::string decl(const piet::SessionState& s) {
  return s.panelvar.value_or("<none>") + "/" + s.timevar.value_or("<none>");
}

std::string dtaChar(const piet::SessionState& s, const char* name) {
  const std::string* v = s.dtaChar(name);
  return v == nullptr ? std::string("<unset>") : *v;
}

}  // namespace

void testW2Tsset() {
  std::printf("[selftest] --- W2 tsset: panel tsset, tsset r(), tsfill full ---\n");
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2tsset").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState s(workdir);

  // (A) ts_panel -- w2w_tsset_tsfill.log:96-102.
  mkA(s);
  {
    auto r = tsRun("tsset id t", s);
    w2check(r.rc == "0" && decl(s) == "id/t",
            "W2 tsset ts_panel (log:102 rc 0): `tsset id t` declares the panel (FU-581). Got rc " +
                r.rc + " `" + r.msg + "` decl " + decl(s));
    w2check(r.echo ==
                "Panel variable: id (unbalanced)\n Time variable: t, 1 to 8, but with gaps\n"
                "         Delta: 1 unit",
            "W2 tsset ts_panel echo (log:97-99). Got `" + r.echo + "`");
    w2check(rs(s, "panelvar") == "id" && rs(s, "timevar") == "t" && rv(s, "tmin") == 1 &&
                rv(s, "tmax") == 8 && rv(s, "tdelta") == 1 && rv(s, "imin") == 1 &&
                rv(s, "imax") == 4 && rv(s, "gaps") == 1 && rs(s, "balanced") == "unbalanced" &&
                rs(s, "unit") == "<absent>" && rs(s, "unit1") == "." && rs(s, "tsfmt") == "%9.0g" &&
                rs(s, "tmins") == "1" && rs(s, "tmaxs") == "8" && rs(s, "tdeltas") == "1 unit",
            "W2 tsset ts_panel r() (log:102: id|t|1|8|1|1|4|1|unbalanced|<none>|.|%9.0g|1|8|1 "
            "unit). Got unit1 `" + rs(s, "unit1") + "` tsfmt `" + rs(s, "tsfmt") + "` tdeltas `" +
                rs(s, "tdeltas") + "` balanced `" + rs(s, "balanced") + "` gaps " +
                std::to_string(rv(s, "gaps")));
    w2check(dtaChar(s, "_TSpanel") == "id" && dtaChar(s, "_TStvar") == "t" &&
                dtaChar(s, "iis") == "id" && dtaChar(s, "tis") == "t",
            "W2 tsset ts_panel chars (_TSpanel/_TStvar/iis/tis, as xtset writes). Got " +
                dtaChar(s, "_TSpanel") + "|" + dtaChar(s, "_TStvar"));
    // An empty r() local is not posted, and reading it is "" at rc 0 (log:102 `[]`),
    // not the named r(111) of a declared-but-unposted name.
    auto u = tsRun("local w2ts_u \"`r(unit)'\"", s);
    w2check(u.rc == "0" && s.eval.locals.get("w2ts_u", "<unset>").empty(),
            "W2 tsset ts_panel r(unit) reads \"\" at rc 0 (log:102). Got rc " + u.rc);
  }
  // ts_redisplay -- log:119-125; ts_xtread -- log:132-138.
  {
    auto r = tsRun("tsset", s);
    w2check(r.rc == "0" && decl(s) == "id/t" && rs(s, "balanced") == "unbalanced" &&
                r.echo.rfind("Panel variable: id (unbalanced)", 0) == 0,
            "W2 tsset ts_redisplay (log:121-125 rc 0): bare `tsset` re-displays. Got rc " + r.rc +
                " `" + r.msg + "` echo `" + r.echo + "`");
    auto x = tsRun("xtset", s);
    w2check(x.rc == "0" && rs(s, "panelvar") == "id" && rs(s, "timevar") == "t" &&
                rv(s, "imax") == 4,
            "W2 tsset ts_xtread (log:138 rc 0): bare `xtset` after `tsset id t` re-displays. Got "
            "rc " + x.rc + " `" + x.msg + "`");
  }
  // xt_tsread -- log:156-162.
  mkA(s);
  (void)piet::dispatch("xtset id t", s);
  {
    auto r = tsRun("tsset", s);
    w2check(r.rc == "0" && rs(s, "panelvar") == "id" && rv(s, "gaps") == 1,
            "W2 tsset xt_tsread (log:162 rc 0): bare `tsset` after `xtset id t`. Got rc " + r.rc +
                " `" + r.msg + "`");
  }
  // ts_tsonly -- log:198-204.
  mkA(s);
  (void)piet::dispatch("keep if id == 3", s);
  {
    auto r = tsRun("tsset t", s);
    w2check(r.rc == "0" && decl(s) == "<none>/t" && rv(s, "gaps") == 1 &&
                rs(s, "unit1") == "." && rs(s, "tdeltas") == "1 unit" &&
                rs(s, "panelvar") == "<absent>" && rv(s, "imin") == -999.0,
            "W2 tsset ts_tsonly (log:204: rc 0, no panel r(), gaps 1, unit1 ., 1 unit). Got rc " +
                r.rc + " gaps " + std::to_string(rv(s, "gaps")) + " unit1 `" + rs(s, "unit1") +
                "`");
    auto u = tsRun("local w2ts_u \"`r(unit)'\"", s);
    w2check(u.rc == "0",
            "W2 tsset ts_tsonly: reading r(unit) is rc 0 (log:204; main r(111)). Got rc " + u.rc +
                " `" + u.msg + "`");
  }
  // ts_yearly_panel -- log:224-230; ts_yearly_redisplay -- log:236-246.
  mkA(s);
  {
    auto r = tsRun("tsset id yr, yearly", s);
    w2check(r.rc == "0" && rs(s, "unit") == "yearly" && rs(s, "unit1") == "y" &&
                rs(s, "tsfmt") == "%ty" && rs(s, "tmins") == "2000" && rs(s, "tmaxs") == "2007" &&
                rs(s, "tdeltas") == "1 year" && s.varMeta["yr"].format.value_or("") == "%ty",
            "W2 tsset ts_yearly_panel (log:230: yearly|y|%ty|2000|2007|1 year, format %ty). Got "
            "rc " + r.rc + " `" + r.msg + "` unit `" + rs(s, "unit") + "` tsfmt `" +
                rs(s, "tsfmt") + "` tmins `" + rs(s, "tmins") + "`");
    w2check(r.echo ==
                "Panel variable: id (unbalanced)\n Time variable: yr, 2000 to 2007, but with "
                "gaps\n         Delta: 1 year",
            "W2 tsset ts_yearly_panel echo (log:225-227). Got `" + r.echo + "`");
    auto b = tsRun("tsset", s);
    w2check(b.rc == "0" && rs(s, "unit") == "yearly" && rs(s, "tsfmt") == "%ty",
            "W2 tsset ts_yearly_redisplay (log:246: the unit follows the %ty format). Got rc " +
                b.rc + " unit `" + rs(s, "unit") + "`");
  }
  // ts_delta2 -- log:334-340.
  mkB(s);
  {
    auto r = tsRun("tsset id t, delta(2)", s);
    w2check(r.rc == "0" && rv(s, "tmin") == 2 && rv(s, "tmax") == 10 && rv(s, "tdelta") == 2 &&
                rv(s, "imax") == 3 && rs(s, "tdeltas") == "2 units" && s.delta.value_or(0) == 2,
            "W2 tsset ts_delta2 (log:340: 2|10|2|1|3|2 units). Got rc " + r.rc + " `" + r.msg +
                "` tdeltas `" + rs(s, "tdeltas") + "`");
    (void)piet::dispatch("generate double ly = L.y", s);
    w2check(nMissing(s, "ly") == 5,
            "W2 tsset ts_delta2_gen_L (log:745: 5 missing): L. steps by delta within panels. Got " +
                std::to_string(nMissing(s, "ly")));
  }
  // (C) L./F./D. within panels after `tsset id t` -- log:382, :414, :446, :478.
  mkA(s);
  (void)piet::dispatch("capture quietly tsset id t", s);
  {
    auto r = tsRun("generate double ly = L.y", s);
    (void)piet::dispatch("generate double fy = F.y", s);
    (void)piet::dispatch("generate double dy = D.y", s);
    (void)piet::dispatch("generate double l2y = L2.y", s);
    w2check(r.rc == "0" && nMissing(s, "ly") == 7 && nMissing(s, "fy") == 7 &&
                nMissing(s, "dy") == 7 && nMissing(s, "l2y") == 9,
            "W2 tsset ts_gen_L/F/D/L2 (log:382/414/446/478: 7/7/7/9 missing). Got rc " + r.rc +
                " `" + r.msg + "` " + std::to_string(nMissing(s, "ly")) + "/" +
                std::to_string(nMissing(s, "fy")) + "/" + std::to_string(nMissing(s, "dy")) +
                "/" + std::to_string(nMissing(s, "l2y")));
  }
  // (D) tsfill -- log:852 tsfill_panel, :917 tsfill_full_panel, :935 tsfill_full_redecl.
  mkA(s);
  (void)piet::dispatch("capture quietly tsset id t", s);
  {
    auto r = tsRun("tsfill", s);
    w2check(r.rc == "0" && nRows(s) == 18 && nMissing(s, "y") == 4,
            "W2 tsset tsfill_panel (log:852: 18 rows, 4 missing y). Got rc " + r.rc + " N " +
                std::to_string(nRows(s)));
  }
  mkA(s);
  (void)piet::dispatch("capture quietly tsset id t", s);
  {
    auto r = tsRun("tsfill , full", s);
    w2check(r.rc == "0" && nRows(s) == 32 && nMissing(s, "y") == 18 && nMissing(s, "yr") == 18,
            "W2 tsset tsfill_full_sp (log:1032: `tsfill , full` 32 rows, 18 missing y/yr). Got "
            "rc " + r.rc + " `" + r.msg + "` N " + std::to_string(nRows(s)));
    auto d = tsRun("tsset", s);
    w2check(d.rc == "0" && rs(s, "balanced") == "strongly balanced" && rv(s, "gaps") == 0 &&
                d.echo == "Panel variable: id (strongly balanced)\n Time variable: t, 1 to 8\n"
                          "         Delta: 1 unit",
            "W2 tsset tsfill_full_redecl (log:929-935: strongly balanced, no gaps). Got `" +
                d.echo + "`");
  }
  mkA(s);
  (void)piet::dispatch("keep if id == 3", s);
  (void)piet::dispatch("capture quietly tsset t", s);
  {
    auto r = tsRun("tsfill, full", s);
    w2check(r.rc == "0" && nRows(s) == 8,
            "W2 tsset tsfill_full_tsonly (log:1233: 8 rows after `tsset t`). Got rc " + r.rc +
                " N " + std::to_string(nRows(s)));
  }
  mkB(s);
  (void)piet::dispatch("capture quietly tsset id t, delta(2)", s);
  {
    auto r = tsRun("tsfill, full", s);
    w2check(r.rc == "0" && nRows(s) == 15 && nMissing(s, "y") == 6,
            "W2 tsset tsfill_full_d2 (log:1339: 15 rows, 6 missing y). Got rc " + r.rc + " N " +
                std::to_string(nRows(s)));
  }
  mkA(s);
  (void)piet::dispatch("replace t = . if _n == 5", s);
  (void)piet::dispatch("capture quietly tsset id t", s);
  {
    auto r = tsRun("tsfill", s);
    w2check(r.rc == "0" && nRows(s) == 18,
            "W2 tsset rf_tsfill_tmiss_ctl (log:1729: a missing time stays, 18 rows). Got rc " +
                r.rc + " `" + r.msg + "` N " + std::to_string(nRows(s)));
  }

  // (E) refusals -- Stata's rc, data untouched.
  struct Rf {
    const char* arm;
    const char* setup;
    const char* cmd;
    const char* rc;
  };
  const Rf rfs[] = {
      {"rf_ts_dup (log:1428)", "expand 2 if _n == 6", "tsset id t", "451"},
      {"rf_ts_tsonly_rep (log:1444)", "", "tsset t", "451"},
      {"rf_ts_three (log:1460)", "", "tsset id t x", "103"},
      {"rf_ts_strtime (log:1477)", "", "tsset id s", "109"},
      {"rf_ts_strpanel (log:1494)", "", "tsset s t", "109"},
      {"rf_ts_novar (log:1510)", "", "tsset id nosuchvar", "111"},
      {"rf_ts_badopt (log:1526)", "", "tsset id t, bogus", "198"},
      {"rf_ts_twoper (log:1542)", "", "tsset id yr, yearly quarterly", "198"},
      {"rf_ts_delta2_odd (log:1558)", "", "tsset id t, delta(2)", "451"},
      {"rf_ts_bare_none (log:1574)", "", "tsset", "111"},
      {"rf_tsfill_none (log:1590)", "", "tsfill, full", "111"},
      {"rf_tsfill_none_ctl (log:1606)", "", "tsfill", "111"},
      {"rf_tsfill_badopt (log:1624)", "capture quietly tsset id t", "tsfill, bogus", "198"},
      {"rf_tsfill_if (log:1642)", "capture quietly tsset id t", "tsfill if id == 1", "101"},
      {"rf_tsfill_full_tmiss (log:1662)", "replace t = . if _n == 5", "tsfill, full", "451"},
      // tsset.ado's syntax line has no [if]: the r(101) of rf_tsfill_if (log:1642), the
      // same `syntax` rule (main dropped the qualifier and declared at rc 0).
      {"tsset if (tsset.ado syntax; log:1642 shape)", "", "tsset id t if id == 1", "101"},
  };
  int rfIdx = 0;
  for (const auto& rf : rfs) {
    const std::string rfDir = workdir + "_rf" + std::to_string(rfIdx++);
    fs::remove_all(rfDir, ec);
    fs::create_directories(rfDir, ec);
    piet::SessionState s2(rfDir);
    mkA(s2);
    if (std::string(rf.arm).rfind("rf_tsfill_full_tmiss", 0) == 0) {
      (void)piet::dispatch(rf.setup, s2);
      (void)piet::dispatch("capture quietly tsset id t", s2);
    } else if (*rf.setup) {
      (void)piet::dispatch(rf.setup, s2);
    }
    const int64_t before = nRows(s2);
    auto r = tsRun(rf.cmd, s2);
    w2check(r.rc == rf.rc && nRows(s2) == before,
            std::string("W2 tsset ") + rf.arm + ": `" + rf.cmd + "` is r(" + rf.rc +
                ") and leaves _N. Got rc " + r.rc + " `" + r.msg + "` N " +
                std::to_string(nRows(s2)));
    fs::remove_all(rfDir, ec);
  }
  // tsset: the checks run before the old declaration is cleared, so a refused declaration leaves the
  // live one in place (rf_ts_delta2_odd after a good declaration).
  mkA(s);
  (void)piet::dispatch("tsset id t", s);
  (void)piet::dispatch("capture tsset id t, delta(2)", s);
  w2check(decl(s) == "id/t" && !s.delta.has_value(),
          "W2 tsset: a refused `tsset ..., delta(2)` (log:1558 r(451)) keeps the live declaration "
          "(tsset.ado clears only after its checks). Got " + decl(s));
  // `tsset, clear` -- tsset.ado's Clear (its syntax line takes CLEAR), the twin of
  // `xtset, clear` (w2d_uncovered.log:309-314).
  {
    auto r = tsRun("tsset, clear", s);
    w2check(r.rc == "0" && !s.panelvar.has_value() && !s.timevar.has_value() &&
                dtaChar(s, "_TStvar") == "<unset>",
            "W2 tsset `tsset, clear` drops the declaration and its chars. Got rc " + r.rc + " " +
                decl(s));
  }

  fs::remove_all(workdir, ec);
}
