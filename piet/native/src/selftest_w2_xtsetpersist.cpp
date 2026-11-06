// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `xtsetpersist` -- selftest arms: the tsset/xtset declaration travels
// WITH THE FILE, as `_dta` characteristics.
//
// Oracle (StataBE 19.5): tests/eval/golden/aer_w2/w2_eststo_prefix.do:90-119 /
// .log:425-582 (PART B). Two files are saved from one frame, one before and one
// after `xtset id t`; then:
//   B1 (.log:441) `use` of the un-xtset file -> `xtreg, fe` r(459): the earlier
//      declaration does not leak;            (.log:447) `gen L.x` r(111).
//   B2 (.log:478-500) `use` of the xtset file restores it: xtreg rc 0 with
//      _b[x] = .5053425508221226 and N_g = 40, r(panelvar)/r(timevar) = id/t,
//      `gen L.x` rc 0 (40 missing), chars _TSpanel/_TStvar/iis/tis = id/t/id/t.
//   B3 (.log:541) master un-xtset, using xtset -> xtreg rc 0 (merge fills the
//      master's missing declaration from the using side);
//      (.log:582) master xtset, using not -> xtreg rc 0 (CONTROL).
// On the frozen main binary B1 ran the estimation at rc 0 on the stale panel id
// and `gen L.x` succeeded; `xtset` never wrote a char, so the B2 char arm and the
// B2x arm (a file's declaration REPLACES a different live one) failed too. The
// B2/B3 rc-0 arms are controls there (the stale id happened to be the right one):
// they pin that the fix clears only what the file does not state.
//
// BUG-657: the B1 refusals carry the oracle's exact rc and text (r(459) via the
// errors.h builder, r(111) via coarseRc on the engine's ts-operator message).
// BUG-659: `.tmp`/`.hbin` checkpoints restate the declaration live at `save`.
//
// Not asserted here (outside this task's write set, named in the task result):
// bare `xtset` replay (dispatcher.cpp rejects it; the panelvar arm reads the
// session instead) and the `: char` extended macro (eval/macros.cpp; the char
// arm reads the table).
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

namespace fs = std::filesystem;

struct LineResult {
  std::string rc = "0";
  std::string msg;
  double bx = std::nan("");
  double ng = std::nan("");
};

LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  LineResult r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") {
      r.rc = piet::eval::coarseRc(b);
      r.msg = b.value("message", std::string());
    } else if (type == "estimation") {
      if (b.contains("coeftable") && b["coeftable"].is_array())
        for (const auto& row : b["coeftable"])
          if (row.is_object() && row.value("name", std::string()) == "x" &&
              row.contains("b") && row["b"].is_number())
            r.bx = row["b"].get<double>();
      if (b.contains("absorbed") && b["absorbed"].is_object() &&
          b["absorbed"].contains("id") && b["absorbed"]["id"].is_number())
        r.ng = b["absorbed"]["id"].get<double>();
    }
  }
  return r;
}

std::string dtaChar(const piet::SessionState& s, const char* name) {
  const std::string* v = s.dtaChar(name);
  return v == nullptr ? std::string("<unset>") : *v;
}

std::string decl(const piet::SessionState& s) {
  return s.panelvar.value_or("<none>") + "/" + s.timevar.value_or("<none>");
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

// .5053425508221226 = w2_eststo_prefix.log:481 (%18.0g); rtol 1e-9 (TOLERANCE.md T1).
bool bxMatches(double b) { return std::fabs(b - 0.5053425508221226) <= 1e-9 * 0.5053425508221226; }

}  // namespace

void testW2XtsetPersist() {
  std::printf("[selftest] --- W2 xtsetpersist: xtset/tsset travel with the file as _dta chars ---\n");
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2xtsetpersist").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string noxt = workdir + "/prec_noxt.dta";
  const std::string withxt = workdir + "/prec_withxt.dta";

  piet::SessionState s(workdir);
  // w2_eststo_prefix.do:3-10, verbatim.
  for (const char* c :
       {"clear all", "set obs 400", "generate id = ceil(_n/10)", "generate t = mod(_n-1,10)+1",
        "generate double z = sin(_n*0.37) + cos(id*1.3)",
        "generate double x = 0.6*z + sin(_n*1.7)*0.8 + id/40", "generate double w = cos(_n*0.91)",
        "generate double y = 1 + 0.5*x + 0.3*w + sin(_n*2.3) + mod(id,7)/7"})
    (void)piet::dispatch(c, s);
  (void)piet::dispatch("save \"" + noxt + "\", replace", s);
  (void)piet::dispatch("xtset id t", s);
  w2check(dtaChar(s, "_TSpanel") == "id" && dtaChar(s, "_TStvar") == "t" &&
              dtaChar(s, "iis") == "id" && dtaChar(s, "tis") == "t" &&
              dtaChar(s, "_TSdelta") == "+1.0000000000000X+000",
          "xtsetpersist (w2_eststo_prefix.do:94): `xtset id t` writes Stata's _dta chars "
          "(_TSdelta in %21x, as a StataBE-written file stores it). Got _TSpanel `" +
              dtaChar(s, "_TSpanel") + "` _TStvar `" + dtaChar(s, "_TStvar") + "` _TSdelta `" +
              dtaChar(s, "_TSdelta") + "`");
  (void)piet::dispatch("save \"" + withxt + "\", replace", s);

  // B1 -- .log:441 / :447
  (void)piet::dispatch("use \"" + noxt + "\", clear", s);
  {
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "459" && r.msg == "must specify panelvar; use xtset" && std::isnan(r.bx) &&
                !s.panelvar.has_value(),
            "xtsetpersist B1_use_file_without_xtset_no_leak_CONTROL (w2_eststo_prefix.log:437-441, "
            "r(459) 'must specify panelvar; use xtset'): after `use` of the file saved before "
            "xtset, `xtreg, fe` refuses -- the earlier declaration does not leak. Got rc " + r.rc +
                " `" + r.msg + "` decl " + decl(s));
  }
  {
    auto r = runLine("generate double lx0 = L.x", s);
    w2check(r.rc == "111",
            "xtsetpersist B1_Lop_without_tsset_CONTROL (w2_eststo_prefix.log:444-447, r(111) "
            "'time variable not set'): `gen L.x` with no time variable refuses. Got rc " + r.rc +
                " `" + r.msg + "`");
  }
  // The same two refusals from their sibling sites (BUG-657 class; Stata
  // answers every one of them with the codes above):
  // xtreg's RE arm, xtsum, and the model-path producer of the ts-operator error.
  for (const char* c : {"xtreg y x w", "xtreg y x w, re", "xtsum y"}) {
    auto r = runLine(c, s);
    w2check(r.rc == "459" && r.msg == "must specify panelvar; use xtset",
            std::string("xtsetpersist BUG-657 sibling (r(459), as "
                        "w2_eststo_prefix.log:437-441): `") + c + "` with no panel variable. Got rc " +
                r.rc + " `" + r.msg + "`");
  }
  {
    auto r = runLine("regress y L.x", s);
    w2check(r.rc == "111",
            "xtsetpersist BUG-657 sibling (r(111), as w2_eststo_prefix.log:444-447): "
            "`regress y L.x` with no time variable. Got rc " + r.rc + " `" + r.msg + "`");
  }

  // B2 -- .log:478-500
  (void)piet::dispatch("use \"" + withxt + "\", clear", s);
  {
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0",
            "xtsetpersist B2_use_file_with_xtset (w2_eststo_prefix.log:478, rc 0). Got rc " +
                r.rc + " `" + r.msg + "`");
    w2check(bxMatches(r.bx) && r.ng == 40.0,
            "xtsetpersist VAL|B2|b_x + N_g (w2_eststo_prefix.log:481): _b[x] .5053425508221226, "
            "N_g 40. Got " + std::to_string(r.bx) + " / " + std::to_string(r.ng));
  }
  w2check(decl(s) == "id/t",
          "xtsetpersist VAL|B2|panelvar/timevar (w2_eststo_prefix.log:490, id|t; read off the "
          "session -- bare `xtset` replay is not wired). Got " + decl(s));
  {
    auto r = runLine("generate double lx1 = L.x", s);
    w2check(r.rc == "0" && nMissing(s, "lx1") == 40,
            "xtsetpersist B2_Lop_after_use (w2_eststo_prefix.log:493-496, rc 0, 40 missing). Got "
            "rc " + r.rc + " missing " + std::to_string(nMissing(s, "lx1")));
  }
  w2check(dtaChar(s, "_TSpanel") == "id" && dtaChar(s, "_TStvar") == "t" &&
              dtaChar(s, "iis") == "id" && dtaChar(s, "tis") == "t",
          "xtsetpersist VAL|B2|chars (w2_eststo_prefix.log:500, id|t|id|t; the char table "
          "itself -- the `: char` macro is not wired). Got " + dtaChar(s, "_TSpanel") + "|" +
              dtaChar(s, "_TStvar") + "|" + dtaChar(s, "iis") + "|" + dtaChar(s, "tis"));

  // B2x -- the same law from the other side: the file's declaration REPLACES a
  // different live one (not in the oracle file verbatim; it is B2's `use` after a
  // declaration that disagrees with the file, which is what makes the arm fail on
  // the pre-fix binary: the stale t/id panel gave N_g 10).
  (void)piet::dispatch("xtset t id", s);
  (void)piet::dispatch("use \"" + withxt + "\", clear", s);
  {
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0" && bxMatches(r.bx) && r.ng == 40.0 && decl(s) == "id/t",
            "xtsetpersist B2x (w2_eststo_prefix.log:478-490 semantics): `use` of the xtset file "
            "replaces a live `xtset t id` with the file's id/t. Got rc " + r.rc + " N_g " +
                std::to_string(r.ng) + " decl " + decl(s));
  }

  // B3 -- .log:541 / :582
  (void)piet::dispatch("use \"" + noxt + "\", clear", s);
  (void)piet::dispatch("keep id t y", s);
  (void)piet::dispatch("merge 1:1 id t using \"" + withxt + "\", keepusing(x w) nogenerate", s);
  {
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0" && bxMatches(r.bx) && r.ng == 40.0 && decl(s) == "id/t",
            "xtsetpersist B3_xtset_only_on_merge_USING_side (w2_eststo_prefix.log:541, rc 0): "
            "an un-xtset master takes the using file's declaration. Got rc " + r.rc + " `" +
                r.msg + "` decl " + decl(s));
  }
  (void)piet::dispatch("use \"" + withxt + "\", clear", s);
  (void)piet::dispatch("keep id t y", s);
  (void)piet::dispatch("merge 1:1 id t using \"" + noxt + "\", keepusing(x w) nogenerate", s);
  {
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0" && bxMatches(r.bx) && decl(s) == "id/t",
            "xtsetpersist B3_xtset_on_merge_MASTER_side_CONTROL (w2_eststo_prefix.log:582, rc 0). "
            "Got rc " + r.rc + " decl " + decl(s));
  }

  // BUG-659 -- checkpoints. A Stata tempfile is a .dta whose _dta chars carry
  // the declaration, so B2's law (.log:478-500: a file saved declared restores
  // it on `use`) applies to the container; no oracle arm captures this exact
  // sequence. PIET's `.tmp`/`.hbin` has no char table: the declaration rides
  // beside the path. Each arm fails on the frozen main binary (the live
  // declaration of the moment leaked in) and on the base fix alone (a native
  // load kept whatever was live).
  const std::string ckHbin = workdir + "/ck_withxt.hbin";
  for (int disk = 0; disk < 2; ++disk) {
    const std::string arm = disk ? "disk .hbin" : "tempfile .tmp";
    const std::string ck = disk ? ("\"" + ckHbin + "\"") : std::string("\"`ck'\"");
    (void)piet::dispatch("use \"" + withxt + "\", clear", s);
    auto src = std::string("tempfile ck\n") + "save " + ck + ", replace\n" + "use \"" + noxt +
               "\", clear\n" + "xtset t id\n" + "use " + ck + ", clear\n";
    (void)piet::doexec::executeDoSource(src, s, piet::dispatch);
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0" && bxMatches(r.bx) && r.ng == 40.0 && decl(s) == "id/t" &&
                dtaChar(s, "_TSpanel") == "id",
            "xtsetpersist BUG-659 " + arm + " (B2 law, w2_eststo_prefix.log:478-490): a checkpoint "
            "saved under `xtset id t` restores id/t on `use`, over an intervening un-declared "
            "`use` and a live `xtset t id`. Got rc " + r.rc + " N_g " + std::to_string(r.ng) +
                " decl " + decl(s));
  }
  {
    (void)piet::dispatch("use \"" + noxt + "\", clear", s);
    auto src = std::string("tempfile ck0\nsave \"`ck0'\", replace\nuse \"") + withxt +
               "\", clear\nuse \"`ck0'\", clear\n";
    (void)piet::doexec::executeDoSource(src, s, piet::dispatch);
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "459" && !s.panelvar.has_value() && !s.timevar.has_value() &&
                dtaChar(s, "_TSpanel") == "<unset>",
            "xtsetpersist BUG-659 un-declared checkpoint (B1 law, w2_eststo_prefix.log:441): a "
            "tempfile saved with no declaration comes back with none, even after `use` of an "
            "xtset file. Got rc " + r.rc + " decl " + decl(s));
  }
  {
    // merge's fill rule (.log:541) with a native checkpoint on the using side.
    (void)piet::dispatch("use \"" + noxt + "\", clear", s);
    (void)piet::dispatch("keep id t y", s);
    (void)piet::dispatch("merge 1:1 id t using \"" + ckHbin + "\", keepusing(x w) nogenerate", s);
    auto r = runLine("xtreg y x w, fe", s);
    w2check(r.rc == "0" && r.ng == 40.0 && decl(s) == "id/t",
            "xtsetpersist BUG-659 merge using a declared .hbin checkpoint (B3 law, "
            "w2_eststo_prefix.log:541): an un-declared master takes its id/t. Got rc " + r.rc +
                " decl " + decl(s));
  }

  // Siblings of the same class (Stata semantics; no separate oracle line):
  // bare `clear` drops the char table and with it the declaration; `restore`
  // brings back the declaration that was live at `preserve`.
  (void)piet::dispatch("preserve", s);
  (void)piet::dispatch("clear", s);
  w2check(!s.panelvar.has_value() && !s.timevar.has_value(),
          "xtsetpersist clear: bare `clear` drops the declaration with the data. Got " + decl(s));
  (void)piet::dispatch("restore", s);
  w2check(decl(s) == "id/t" && dtaChar(s, "_TSpanel") == "id",
          "xtsetpersist restore: `restore` brings back the preserved declaration. Got " + decl(s));

  // W2D-T06 -- `xtset, clear` (w2b_dataio.log:323; w2d_uncovered.log:314-382).
  // Every arm fails on the frozen main binary: the empty varlist was refused at
  // rc 198 before any option was read, so the declaration survived `capture
  // xtset, clear` and `generate ly = L.y` ran at rc 0.
  const std::string clearedDta = workdir + "/prec_cleared.dta";
  (void)piet::dispatch("xtset id t", s);
  {
    std::size_t nb = 0;
    for (const auto& b : piet::dispatch("xtset, clear", s))
      if (b.is_object() && b.value("type", std::string()) == "error") ++nb;
    w2check(nb == 0 && !s.panelvar.has_value() && !s.timevar.has_value() && !s.delta.has_value() &&
                dtaChar(s, "_TSpanel") == "<unset>" && dtaChar(s, "_TStvar") == "<unset>" &&
                dtaChar(s, "_TSdelta") == "<unset>" && dtaChar(s, "iis") == "<unset>" &&
                dtaChar(s, "tis") == "<unset>",
            "xtset-clear xtset_clear (w2d_uncovered.log:309-314, rc 0 and the chars read empty): "
            "`xtset, clear` is served and drops the declaration and every ts char. Got errors " +
                std::to_string(nb) + " decl " + decl(s) + " _TSpanel " + dtaChar(s, "_TSpanel"));
  }
  {
    auto r = runLine("generate double ly = L.y", s);
    w2check(r.rc == "111",
            "xtset-clear xtset_clear_then_lag (w2d_uncovered.log:378-382, r(111) 'time variable "
            "not set'): `L.y` after the clear refuses. Got rc " + r.rc + " `" + r.msg + "`");
  }
  {
    auto r = runLine("xtset", s);
    w2check(r.rc == "459" && r.msg == "panel variable not set; use xtset varname ...",
            "xtset-clear xtset_query_after_clear (w2d_uncovered.log:326-332, r(459) 'panel variable "
            "not set; use xtset varname ...'). Got rc " + r.rc + " `" + r.msg + "`");
  }
  {
    auto r = runLine("xtset, clear", s);
    w2check(r.rc == "0" && r.msg.empty(),
            "xtset-clear xtset_clear_when_unset (w2d_uncovered.log:364, rc 0): clearing nothing is "
            "not an error. Got rc " + r.rc + " `" + r.msg + "`");
  }
  (void)piet::dispatch("xtset id t", s);
  (void)piet::dispatch("xtset, clear", s);
  (void)piet::dispatch("save \"" + clearedDta + "\", replace", s);
  (void)piet::dispatch("use \"" + withxt + "\", clear", s);
  (void)piet::dispatch("use \"" + clearedDta + "\", clear", s);
  w2check(!s.panelvar.has_value() && !s.timevar.has_value() && dtaChar(s, "_TSpanel") == "<unset>",
          "xtset-clear save/use (B1 law, w2_eststo_prefix.log:441, over w2d_uncovered.log:314): a "
          "file saved after `xtset, clear` comes back with no declaration, even after `use` of an "
          "xtset file. Got " + decl(s));
  (void)piet::dispatch("xtset id t", s);
  // StataBE 19.5 (oracle 2026-10-29, BUG-1163): `xtset id t, clear` is r(101)
  // "varlist not allowed", `xtset id, clear` r(101) "options not allowed unless a
  // time variable is specified", `xtset, clear delta(1)` r(198) "option delta()
  // not allowed". This arm used to pin r(198) for all three, unmeasured.
  for (const auto& [c, want] : std::vector<std::pair<const char*, const char*>>{
           {"xtset id t, clear", "101"}, {"xtset, clear delta(1)", "198"},
           {"xtset id, clear", "101"}}) {
    auto r = runLine(c, s);
    w2check(r.rc == want && decl(s) == "id/t",
            std::string("xtset-clear refusal: `") + c + "` is rc " + want +
                " and leaves the declaration alone. Got rc " + r.rc + " `" + r.msg + "` decl " +
                decl(s));
  }
  {
    // FU-429 interim half: the posted set is declared, so an absent name reads
    // missing (rc 0) where it used to be the unknown-set r(111).
    for (const char* c : {"xtset id t", "tsset t"}) {
      (void)piet::dispatch(c, s);
      auto r = runLine("display r(nosuch)", s);
      w2check(r.rc == "0",
              std::string("xtset-clear FU-429 interim: `display r(nosuch)` after `") + c +
                  "` reads missing at rc 0 (the command declares its r() set). Got rc " + r.rc +
                  " `" + r.msg + "`");
    }
    (void)piet::dispatch("xtset id t", s);
    auto r = runLine("display r(panelvar)", s);
    w2check(r.rc == "0", "xtset-clear FU-429 CONTROL: a posted name still reads at rc 0. Got rc " +
                             r.rc + " `" + r.msg + "`");
  }

  fs::remove_all(workdir, ec);
}
