// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `mergeappend` -- selftest arms (BUG-548..BUG-551).
//
// Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2_smalls.log section E -- merge keepusing() is a
//     VARLIST parsed against the using data: keepusing(mother*) rc 0 k=10,
//     keepusing(mother_a-father_a) rc 0 k=11, keepusing(zzz) r(111) k=8,
//     keepusing(moth) r(111) "moth ambiguous abbreviation" k=8.
//   tests/eval/golden/aer_w2/w2_data_io.log app1-app14 -- `append using F,
//     keep(varlist)`: narrowing, a name absent from the using data (r(111), data
//     untouched), kept columns in the USING file's order, keep() with
//     generate(), a unique abbreviation (keep(i)), a glob and a range, and a
//     type clash on an excluded variable that no longer matters.
//   w2_data_io.log app15-app17 (BUG-550) -- a numeric/string clash on a KEPT
//     variable is r(106) (data untouched); under force the using cells are
//     missing of the master's type.
//   w2_data_io.log app8-app13 (BUG-551) -- generate()'s marker sits after the
//     master's variables with the `_append` value label ("Master" /
//     "Appended dataset 1"); the using file's value-label definitions (alab)
//     are copied unless nolabel.
// Every arm fails on the frozen main binary: there keepusing(mother*) brought
// zero using columns at rc 0, keepusing(zzz)/keepusing(moth) were rc 0,
// `append ..., keep()` was r(198) "option `keep` not allowed", a clash was
// rc 0 with the string codes 0..3 as the using values (force a no-op), no
// label definition crossed over, and the marker came last and unlabelled.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

namespace fs = std::filesystem;

struct LineResult {
  std::string rc = "0";
  std::string msg;
};

LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  LineResult r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    if (b.value("type", std::string()) != "error") continue;
    r.rc = piet::eval::coarseRc(b);
    r.msg = b.value("message", std::string());
  }
  return r;
}

std::string joinNames(piet::SessionState& session) {
  std::string out;
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr) return "<no data>";
  for (const auto& n : ds->names()) {
    if (!out.empty()) out += " ";
    out += n;
  }
  return out;
}

int64_t nRows(piet::SessionState& session) {
  piet::Datastore* ds = session.datastore();
  return ds == nullptr ? -1 : ds->nRows();
}

double cell(piet::SessionState& session, const std::string& var, int64_t row) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "f64" || row >= ds->nRows()) return -999;
  return ds->col(var).f64()[row];
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

void testW2MergeAppend() {
  std::printf("[selftest] --- W2 mergeappend: keepusing()/keep() are varlists over the USING data ---\n");
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2mergeappend").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ---------------------------------------------------------------- merge (BUG-548)
  // w2_smalls.do's frame, trimmed to the columns section E needs; the master
  // carries 5 variables here, so the oracle's k=8 -> 10 / 11 reads as +2 / +3.
  const std::string mUsing = workdir + "/w2s_using.dta";
  auto smallsMaster = [&](piet::SessionState& s) {
    for (const char* c : {"clear", "set obs 12", "generate id = ceil(_n/3)",
                          "generate t = mod(_n-1,3)+1", "generate x = mod(_n*7,11)",
                          "generate x2 = mod(_n*3,5)", "generate y = 2*x - x2 + mod(_n,4)"})
      (void)piet::dispatch(c, s);
  };
  {
    piet::SessionState setup(workdir);
    smallsMaster(setup);
    for (const char* c : {"keep id t", "generate mother_a = id*10 + t",
                          "generate mother_b = id*100 + t", "generate father_a = -id"})
      (void)piet::dispatch(c, setup);
    (void)piet::dispatch("save \"" + mUsing + "\", replace", setup);
  }
  {
    piet::SessionState s(workdir);
    smallsMaster(s);
    auto r = runLine("merge 1:1 id t using \"" + mUsing + "\", keepusing(mother*) nogenerate", s);
    w2check(r.rc == "0" && joinNames(s) == "id t x x2 y mother_a mother_b",
            "BUG-548 (w2_smalls.log keepusing_glob rc 0 k=10): keepusing(mother*) brings "
            "mother_a mother_b and not father_a. Got rc " + r.rc + " `" + r.msg + "` names `" +
                joinNames(s) + "`");
    w2check(cell(s, "mother_b", 4) == 202.0,
            "BUG-548: the glob-kept column carries the using values (row 5: id 2, t 2 -> 202)");
  }
  {
    piet::SessionState s(workdir);
    smallsMaster(s);
    auto r = runLine(
        "merge 1:1 id t using \"" + mUsing + "\", keepusing(mother_a-father_a) nogenerate", s);
    w2check(r.rc == "0" && joinNames(s) == "id t x x2 y mother_a mother_b father_a",
            "BUG-548 (w2_smalls.log keepusing_range rc 0 k=11): keepusing(mother_a-father_a) is "
            "the using-order range of three. Got rc " + r.rc + " names `" + joinNames(s) + "`");
  }
  {
    piet::SessionState s(workdir);
    smallsMaster(s);
    auto r = runLine("merge 1:1 id t using \"" + mUsing + "\", keepusing(zzz) nogenerate", s);
    w2check(r.rc == "111" && has(r.msg, "variable zzz not found") &&
                joinNames(s) == "id t x x2 y",
            "BUG-548 (w2_smalls.log keepusing_absent r(111) k=8): a keepusing() name absent "
            "from the using data is r(111) `variable zzz not found` and the master is untouched. "
            "Got rc " + r.rc + " `" + r.msg + "` names `" + joinNames(s) + "`");
  }
  {
    piet::SessionState s(workdir);
    smallsMaster(s);
    auto r = runLine("merge 1:1 id t using \"" + mUsing + "\", keepusing(moth) nogenerate", s);
    w2check(r.rc == "111" && has(r.msg, "moth ambiguous abbreviation") &&
                joinNames(s) == "id t x x2 y",
            "BUG-548 (w2_smalls.log keepusing_abbrev_ambiguous r(111) k=8): `moth` abbreviates "
            "both mother_a and mother_b. Got rc " + r.rc + " `" + r.msg + "`");
  }

  // ---------------------------------------------------------------- append (BUG-549)
  // w2_data_io.do's two files, verbatim.
  const std::string aUsing = workdir + "/w1bio_u.dta";
  const std::string aMaster = workdir + "/w1bio_m.dta";
  const std::string aUsingHbin = workdir + "/w1bio_u.hbin";
  {
    piet::SessionState setup(workdir);
    for (const char* c : {"clear", "set obs 4", "generate id = _n", "generate a = _n*10",
                          "generate str3 s = \"u\" + string(_n)", "generate z = _n + 100",
                          "label define alab 10 \"ten\" 20 \"twenty\"", "label values a alab"})
      (void)piet::dispatch(c, setup);
    (void)piet::dispatch("save \"" + aUsing + "\", replace", setup);
    (void)piet::dispatch("save \"" + aUsingHbin + "\", replace", setup);
    for (const char* c : {"clear", "set obs 3", "generate id = _n + 10", "generate a = _n",
                          "generate m = _n*2"})
      (void)piet::dispatch(c, setup);
    (void)piet::dispatch("save \"" + aMaster + "\", replace", setup);
  }
  struct AppArm {
    const char* label;
    std::string usingPath;
    const char* opts;
    const char* rc;
    int64_t n;
    const char* names;
  };
  const std::vector<AppArm> arms = {
      {"app1", aUsing, "keep(id a)", "0", 7, "id a m"},
      {"app1/hbin", aUsingHbin, "keep(id a)", "0", 7, "id a m"},
      {"app2", aUsing, "keep(z s id)", "0", 7, "id a m s z"},
      {"app3", aUsing, "keep(id zz)", "111", 3, "id a m"},
      {"app4", aUsing, "keep(id m)", "111", 3, "id a m"},
      {"app5", aUsing, "keep(i)", "0", 7, "id a m"},
      {"app6", aUsing, "keep(i* z)", "0", 7, "id a m z"},
      {"app7", aUsing, "keep(id-s)", "0", 7, "id a m s"},
      {"app10", aUsing, "generate(src) keep(id a)", "0", 7, "id a m src"},
  };
  for (const auto& arm : arms) {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    auto r = runLine("append using \"" + arm.usingPath + "\", " + arm.opts, s);
    w2check(r.rc == arm.rc && nRows(s) == arm.n && joinNames(s) == arm.names,
            std::string("BUG-549 (w2_data_io.log ") + arm.label + "): `append using F, " +
                arm.opts + "` -> rc " + arm.rc + ", _N " + std::to_string(arm.n) + ", `" +
                arm.names + "`. Got rc " + r.rc + " `" + r.msg + "` _N " +
                std::to_string(nRows(s)) + " `" + joinNames(s) + "`");
    if (std::string(arm.rc) == "111") {
      w2check(has(r.msg, "not found") && has(r.msg, "keep(varlist)"),
              std::string("BUG-549 (") + arm.label +
                  "): the message is Stata's `variable X not found` + the keep(varlist) note. "
                  "Got `" + r.msg + "`");
    }
    if (std::string(arm.label) == "app5") {
      // keep(i) kept id only: a is master's own and missing on the using rows.
      w2check(cell(s, "id", 3) == 1.0 && std::isnan(cell(s, "a", 3)),
              "BUG-549 (app5): keep(i) takes only `id` from the using rows -- `a` is missing there");
    }
    if (std::string(arm.label) == "app7") {
      w2check(cell(s, "a", 3) == 10.0,
              "BUG-549 (app7): keep(id-s) includes `a`, so the using rows carry a = 10");
    }
  }
  {
    // app14: s is numeric in the master and str3 in the using; keep(id a)
    // excludes it, so the clash never arises and s is missing on the using rows.
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    (void)piet::dispatch("generate s = 7", s);
    auto r = runLine("append using \"" + aUsing + "\", keep(id a)", s);
    w2check(r.rc == "0" && nRows(s) == 7 && joinNames(s) == "id a m s" &&
                cell(s, "s", 0) == 7.0 && std::isnan(cell(s, "s", 3)),
            "BUG-549 (w2_data_io.log app14): keep(id a) with a type clash on the EXCLUDED s is "
            "rc 0, s stays numeric, missing on the using rows. Got rc " + r.rc + " `" + r.msg +
                "` `" + joinNames(s) + "`");
  }

  // ---------------------------------------------------------------- type clash (BUG-550)
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    (void)piet::dispatch("generate s = 7", s);
    auto r = runLine("append using \"" + aUsing + "\", keep(id s)", s);
    w2check(r.rc == "106" && has(r.msg, "variable s is float in master but str") &&
                has(r.msg, " in using data") &&
                has(r.msg, "force option") && nRows(s) == 3 && joinNames(s) == "id a m s",
            "BUG-550 (w2_data_io.log app15 r(106) _N 3): numeric s in the master, str3 s kept "
            "from the using data, no force -> r(106), data untouched. Got rc " + r.rc + " `" +
                r.msg + "` _N " + std::to_string(nRows(s)));
  }
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    (void)piet::dispatch("generate s = 7", s);
    auto r = runLine("append using \"" + aUsing + "\", keep(id s) force", s);
    bool usingMissing = true;
    for (int64_t i = 3; i < 7; ++i) usingMissing = usingMissing && std::isnan(cell(s, "s", i));
    w2check(r.rc == "0" && nRows(s) == 7 && joinNames(s) == "id a m s" &&
                cell(s, "s", 0) == 7.0 && usingMissing,
            "BUG-550 (w2_data_io.log app16): keep(id s) force -> rc 0, _N 7, s stays numeric, "
            "s = . on the four using rows (not the string codes 0..3). Got rc " + r.rc + " `" +
                r.msg + "` `" + joinNames(s) + "` s[3] " + std::to_string(cell(s, "s", 3)));
  }
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    (void)piet::dispatch("generate s = 7", s);
    auto r = runLine("append using \"" + aUsing + "\", force", s);
    w2check(r.rc == "0" && nRows(s) == 7 && joinNames(s) == "id a m s z" &&
                std::isnan(cell(s, "s", 6)) && cell(s, "z", 6) == 104.0 && cell(s, "a", 4) == 20.0,
            "BUG-550 (w2_data_io.log app17): plain force -> rc 0, `id a m s z`, s = . and z "
            "carried on the using rows. Got rc " + r.rc + " `" + r.msg + "` `" + joinNames(s) +
                "` s[6] " + std::to_string(cell(s, "s", 6)));
  }
  {
    // The other direction: a string master variable, numeric in the using file.
    // No oracle arm measures it; the rule is the same clash (r(106) without force).
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    (void)piet::dispatch("generate str1 z = \"q\"", s);
    auto r = runLine("append using \"" + aUsing + "\"", s);
    w2check(r.rc == "106" && has(r.msg, "variable z is str") &&
                has(r.msg, " in master but ") && !has(r.msg, "force option") &&
                nRows(s) == 3,
            "BUG-550: string z in the master, numeric z in the using data -> r(106), not a "
            "silent promotion. Got rc " + r.rc + " `" + r.msg + "`");
  }

  // ---------------------------------------------------------------- labels (BUG-551)
  auto defined = [](piet::SessionState& s, const std::string& lbl, const std::string& code,
                    const std::string& text) {
    auto it = s.labelDefs.find(lbl);
    if (it == s.labelDefs.end()) return false;
    auto c = it->second.find(code);
    return c != it->second.end() && c->second == text;
  };
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    auto r = runLine("append using \"" + aUsing + "\", generate(src)", s);
    w2check(r.rc == "0" && joinNames(s) == "id a m src s z" && cell(s, "src", 2) == 0.0 &&
                cell(s, "src", 3) == 1.0,
            "BUG-551 (w2_data_io.log app8): generate(src)'s marker sits after the master's "
            "variables (`id a m src s z`), 0 on master rows, 1 on using rows. Got rc " + r.rc +
                " `" + r.msg + "` `" + joinNames(s) + "`");
    w2check(s.varLblname.count("src") && s.varLblname["src"] == "_append" &&
                defined(s, "_append", "0", "Master") &&
                defined(s, "_append", "1", "Appended dataset 1"),
            "BUG-551 (app8 lists `Master` / `Appended dataset 1`; the label is named "
            "_append): src carries value label _append");
    w2check(s.varMeta.count("src") && s.varMeta["src"].stataType == std::string("byte") &&
                s.varMeta["src"].label == std::string("Dataset source"),
            "BUG-551 (src is a byte variable labelled \"Dataset source\")");
    w2check(defined(s, "alab", "10", "ten") && !s.varLblname.count("a"),
            "BUG-551 (app8_alab_defined rc 0; app8 lists a = 10, not `ten`): alab's definition "
            "is copied from the using file, the master's `a` stays unlabelled");
  }
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    auto r = runLine("append using \"" + aUsing + "\", generate(src) keep(id a)", s);
    w2check(r.rc == "0" && joinNames(s) == "id a m src" && s.varLblname.count("src") &&
                defined(s, "_append", "1", "Appended dataset 1") && defined(s, "alab", "20", "twenty"),
            "BUG-551 (w2_data_io.log app10 + app10_alab_defined rc 0): generate(src) keep(id a) "
            "labels src and still defines alab. Got rc " + r.rc + " `" + joinNames(s) + "`");
  }
  struct LabArm {
    const char* label;
    const char* opts;
    bool alab;
  };
  for (const LabArm& arm : {LabArm{"app1", "keep(id a)", true}, LabArm{"app6", "keep(i* z)", true},
                            LabArm{"app11", "nolabel", false},
                            LabArm{"app12", "keep(id a) nolabel", false},
                            LabArm{"app13", "keep(id a) nonotes", true},
                            LabArm{"app3", "keep(id zz)", false}}) {
    piet::SessionState s(workdir);
    (void)piet::dispatch("use \"" + aMaster + "\", clear", s);
    auto r = runLine("append using \"" + aUsing + "\", " + arm.opts, s);
    w2check(defined(s, "alab", "10", "ten") == arm.alab &&
                (std::string(arm.label) == "app3" ? r.rc == "111" : r.rc == "0"),
            std::string("BUG-551 (w2_data_io.log ") + arm.label + "_alab_defined " +
                (arm.alab ? "rc 0" : "r(111)") + "): after `append using F, " + arm.opts +
                "` alab is " + (arm.alab ? "" : "NOT ") + "defined. Got rc " + r.rc + " `" +
                r.msg + "`");
  }

  // ------------------------------------------------------- widening notes (BUG-1092)
  // StataBE 19.5 (measured 2026-11-03): using saved as y double, s str5, x double;
  // master x float, s str1, y float. `append` prints its three "(variable V was T,
  // now U to accommodate using data's values)" lines in the USING file's order:
  // y, s, x. The probe's EXPECT-OUTPUT lines cannot see an order.
  {
    const std::string wUsing = workdir + "/w2ma_widen_u.dta";
    piet::SessionState s(workdir);
    for (const std::string c :
         {std::string("clear"), std::string("set obs 1"), std::string("generate double y = 0.5"),
          std::string("generate str5 s = \"hello\""), std::string("generate double x = 0.5"),
          "save \"" + wUsing + "\", replace", std::string("clear"), std::string("set obs 1"),
          std::string("generate x = 1"), std::string("generate str1 s = \"a\""),
          std::string("generate y = 1")})
      (void)piet::dispatch(c, s);
    std::string order;
    for (const auto& b : piet::dispatch("append using \"" + wUsing + "\"", s)) {
      if (!b.is_object() || b.value("type", std::string()) != "display") continue;
      const std::string text = b.value("text", std::string());
      if (has(text, "to accommodate using data's values")) order += text.substr(10, 1);
    }
    w2check(order == "ysx",
            "BUG-1092: append's widening notes follow the using file's variable order (StataBE "
            "y, s, x for using `y s x` onto master `x s y`). Got `" + order + "`");
  }
  fs::remove_all(workdir, ec);
}
