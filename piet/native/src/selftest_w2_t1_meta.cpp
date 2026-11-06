// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane META (findname xtdescribe cf fvunab numlabel labvars renpfix):
// each check below mirrors one arm of the StataBE 19.5 captures
// tests/eval/golden/aer_w2/w2ax_t1_official.log (xtdes_* cf_* fvunab_* numlabel_* renpfix_*)
// and w2ax_t1_community.log (findname_* labvars_*), named in its label, replaying the
// oracle do-file's own lines; the refusal checks pin what this build declines by name.
//
// numlabel is asserted on BOTH label stores directly (labelDefs, which `: label NAME #`
// reads, and the per-variable valueLabels that list/browse read), so a write that reaches
// one store and not the other fails here even though the oracle's display lines only see
// the first.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
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

struct Line {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, newline-joined
};

Line run(const std::string& cmd, piet::SessionState& s) {
  Line r;
  for (const auto& b : piet::dispatch(cmd, s)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") {
      r.rc = piet::eval::coarseRc(b);
      r.msg = b.value("message", std::string());
    } else if (type == "display") {
      if (!r.text.empty()) r.text += "\n";
      r.text += b.value("text", std::string());
    }
  }
  return r;
}

void runAll(std::initializer_list<const char*> cmds, piet::SessionState& s) {
  for (const char* c : cmds) (void)piet::dispatch(c, s);
}

std::string rStr(piet::SessionState& s, const char* k) {
  const std::string* v = s.eval.rStrings.find(k);
  return v == nullptr ? std::string("<unset>") : *v;
}

double rVal(piet::SessionState& s, const char* k) {
  const double* v = s.eval.rValues.find(k);
  return v == nullptr ? std::nan("") : *v;
}

std::string local(piet::SessionState& s, const char* k) {
  const std::string* v = s.eval.locals.find(k);
  return v == nullptr ? std::string("<unset>") : *v;
}

std::string varNames(piet::SessionState& s) {
  piet::Datastore* ds = s.datastore();
  if (ds == nullptr) return "<no data>";
  std::string out;
  for (const auto& n : ds->names()) out += (out.empty() ? "" : " ") + n;
  return out;
}

std::string varLabel(piet::SessionState& s, const std::string& v) {
  s.syncEvalVarLabels();
  const std::string* l = s.eval.varLabels.find(v);
  return l == nullptr ? std::string() : *l;
}

std::string lbl(piet::SessionState& s, const std::string& set, const std::string& code) {
  auto it = s.labelDefs.find(set);
  if (it == s.labelDefs.end()) return "<no set>";
  auto c = it->second.find(code);
  return c == it->second.end() ? std::string("<no code>") : c->second;
}

std::string varValueLabel(piet::SessionState& s, const std::string& v, int code) {
  auto it = s.valueLabels.find(v);
  if (it == s.valueLabels.end()) return "<none>";
  auto c = it->second.find(code);
  return c == it->second.end() ? std::string("<no code>") : c->second;
}

// %18.0g-printed oracle numbers, compared at rtol 1e-9 (TOLERANCE.md T1).
bool near(double got, double want) {
  return std::fabs(got - want) <= 1e-9 * std::max(1.0, std::fabs(want));
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

void testW2T1Meta() {
  std::printf("[selftest] --- AER tier-1 META: findname xtdescribe cf fvunab numlabel labvars renpfix ---\n");
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2t1meta").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ---- findname (w2ax_t1_community.do (7)) --------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 1", "generate p1 = 1", "label variable p1 \"p 1 thing\"",
            "generate p2 = 1", "label variable p2 \"Price 1\"", "generate q = 1",
            "label variable q \"other p 1\"", "generate nolab = 1"},
           s);
    auto r = run("findname, varlabeltext(\"p 1*\")", s);
    w2check(r.rc == "0" && rStr(s, "varlist") == "p1" && r.text == "p1",
            "t1meta findname_glob (community.log:517 `p1`): the glob is matched against the "
            "whole label. Got rc " + r.rc + " r(varlist) `" + rStr(s, "varlist") + "` text `" +
                r.text + "`");
    r = run("findname, varlabeltext(\"*p 1*\")", s);
    w2check(r.rc == "0" && rStr(s, "varlist") == "p1 q" && r.text == "p1  q",
            "t1meta findname_glob2 (community.log:521-523 `p1  q`): columns of longest name + 2. "
            "Got r(varlist) `" + rStr(s, "varlist") + "` text `" + r.text + "`");
    r = run("findname, varlabeltext(\"P*\")", s);
    w2check(rStr(s, "varlist") == "p2",
            "t1meta findname_case (community.log:529 `p2`): case-sensitive -- `P*` finds only "
            "\"Price 1\". Got `" + rStr(s, "varlist") + "`");
    r = run("findname, varlabeltext(\"zzz*\")", s);
    w2check(r.rc == "0" && rStr(s, "varlist") == "<unset>" && r.text.empty(),
            "t1meta findname_none (community.log:534 rc 0, empty r(varlist)): no match is rc 0, "
            "silent, and r() is replaced. Got rc " + r.rc + " r(varlist) `" +
                rStr(s, "varlist") + "`");
    r = run("findname, varlabeltext(\"?rice 1\")", s);
    w2check(rStr(s, "varlist") == "p2",
            "t1meta findname_qmark (community.log:540 `p2`): `?` is one character. Got `" +
                rStr(s, "varlist") + "`");
    r = run("findname", s);
    w2check(rStr(s, "varlist") == "p1 p2 q nolab" && r.text == "p1     p2     q      nolab",
            "t1meta findname_bare (community.log:544-546): every variable, width 5+2 columns. "
            "Got `" + rStr(s, "varlist") + "` text `" + r.text + "`");
    r = run("findname p*, varlabeltext(\"*1*\")", s);
    w2check(rStr(s, "varlist") == "p1 p2",
            "t1meta findname_varlist (community.log:552 `p1 p2`): a leading varlist restricts "
            "the search. Got `" + rStr(s, "varlist") + "`");
    r = run("findname, type(numeric)", s);
    w2check(r.rc == "198" && has(r.msg, "type"),
            "t1meta findname refusal: type() is a real findname option this build does not "
            "implement -- refused by name, not ignored. Got rc " + r.rc + " `" + r.msg + "`");
  }

  // ---- xtdescribe (w2ax_t1_official.do (3)) ------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 60", "generate id = ceil(_n/6)", "generate t  = mod(_n-1,6)+1",
            "generate y  = mod(_n*7,11)",
            "drop if (mod(id,3)==0 & t>4) | (id==2 & t==3) | (id==7 & t<3)",
            "generate byte esamp1 = mod(_n,4) != 0"},
           s);
    auto r = run("xtdescribe", s);
    w2check(r.rc == "459" && r.msg == "must specify panelvar and timevar; use xtset",
            "t1meta xtdes_notset (official.log:383-386 r(459)). Got rc " + r.rc + " `" + r.msg + "`");
    (void)piet::dispatch("xtset id t", s);
    r = run("xtdescribe", s);
    w2check(r.rc == "0" && rVal(s, "N") == 10 && rVal(s, "sum") == 51 && near(rVal(s, "mean"), 5.1) &&
                near(rVal(s, "sd"), .9944289260117533) && near(rVal(s, "skewness"), -.2000894874618736) &&
                near(rVal(s, "kurtosis"), 1.153515970205782) && rVal(s, "min") == 4 &&
                rVal(s, "p5") == 4 && rVal(s, "p25") == 4 && rVal(s, "p50") == 5.5 &&
                rVal(s, "p75") == 6 && rVal(s, "p99") == 6 && rVal(s, "max") == 6,
            "t1meta xtdes_basic (official.log:416): r() is summarize T_i, detail over the 10 "
            "panels (N 10, sum 51, sd .9944289260117533, p50 5.5). Got N " +
                std::to_string(rVal(s, "N")) + " sum " + std::to_string(rVal(s, "sum")) + " p50 " +
                std::to_string(rVal(s, "p50")) + " sd " + std::to_string(rVal(s, "sd")));
    w2check(has(r.text, "      id:  1, 2, ..., 10                                     n =         10") &&
                has(r.text, "                         4       4       4         6         6       6       6") &&
                has(r.text, "        5     50.00   50.00 |  111111") &&
                has(r.text, "        1     10.00   90.00 |  ..1111\n        1     10.00  100.00 |  11.111") &&
                has(r.text, "       10    100.00         |  XXXXXX"),
            "t1meta xtdes_basic display (official.log:390-406): header, T_i line (p50 5.5 prints "
            "6), patterns by frequency with ties ascending (`..1111` before `11.111`). Got\n" + r.text);
    r = run("xtdescribe if esamp1", s);
    w2check(r.rc == "0" && rVal(s, "N") == 10 && rVal(s, "sum") == 39 && rVal(s, "p50") == 4 &&
                near(rVal(s, "sd"), .8755950357709131) && near(rVal(s, "skewness"), .1884300309168565),
            "t1meta xtdes_if (official.log:443): `if esamp1` restricts the observations (sum 39, "
            "sd .8755950357709131). Got sum " + std::to_string(rVal(s, "sum")) + " sd " +
                std::to_string(rVal(s, "sd")));
    r = run("xtdescribe if t > 2", s);
    w2check(r.rc == "0" && rVal(s, "sum") == 33 && rVal(s, "min") == 2 &&
                has(r.text, "       t:  3, 4, ..., 6                                      T =          4"),
            "t1meta xtdes_if_t (official.log:449-466): the header reports the restricted times "
            "(3..6, T 4), r(sum) 33. Got sum " + std::to_string(rVal(s, "sum")) + "\n" + r.text);
    r = run("xtdescribe, patterns(3)", s);
    w2check(r.rc == "0" && rVal(s, "N") == 10 &&
                has(r.text, "        1     10.00  100.00 | (other patterns)"),
            "t1meta xtdes_patterns3 (official.log:479-489): three patterns, the rest pooled. Got\n" +
                r.text);
    r = run("xtdescribe, patterns(0)", s);
    w2check(r.rc == "0" && rVal(s, "sum") == 51 && !has(r.text, "Pattern") &&
                has(r.text, "Distribution of T_i:"),
            "t1meta xtdes_patterns0 (official.log:492-503): no pattern table, r() unchanged in "
            "kind. Got\n" + r.text);
    r = run("xtdescribe, patterns(-1)", s);
    w2check(r.rc == "198", "t1meta xtdescribe refusal (mx4.log XD|patneg r(198)): a negative "
                           "patterns() is invalid syntax. Got rc " + r.rc);
    (void)piet::dispatch("xtset id", s);
    r = run("xtdescribe", s);
    w2check(r.rc == "459" && r.msg == "must specify timevar; use xtset",
            "t1meta xtdes_notime (official.log:510-513 r(459)). Got rc " + r.rc + " `" + r.msg + "`");
  }

  // ---- cf (w2ax_t1_official.do (10)) -------------------------------------
  {
    piet::SessionState s(workdir);
    const std::string f = workdir + "/t1cf";
    runAll({"clear", "set obs 5", "generate a = _n", "generate b = _n * 2",
            "generate str3 s = \"x\" + string(_n)"},
           s);
    (void)piet::dispatch("save \"" + f + "\", replace", s);
    auto r = run("cf * using \"" + f + "\"", s);
    w2check(r.rc == "0" && rStr(s, "Nsum") == "0" && r.text.empty(),
            "t1meta cf_same (official.log:978-981 rc 0, r(Nsum) 0, silent). Got rc " + r.rc +
                " Nsum `" + rStr(s, "Nsum") + "` text `" + r.text + "`");
    runAll({"replace b = 99 in 2", "replace b = 98 in 4"}, s);
    r = run("cf * using \"" + f + "\"", s);
    w2check(r.rc == "9" && rStr(s, "Nsum") == "2" && r.text == "               b: 2 mismatches",
            "t1meta cf_diff (official.log:990-993 rc 9, r(Nsum) 2). Got rc " + r.rc + " Nsum `" +
                rStr(s, "Nsum") + "` text `" + r.text + "`");
    r = run("cf a s using \"" + f + "\"", s);
    w2check(r.rc == "0" && rStr(s, "Nsum") == "0",
            "t1meta cf_subset (official.log:998 rc 0): only the named variables. Got rc " + r.rc);
    r = run("cf b using \"" + f + "\", verbose", s);
    w2check(r.rc == "9" && rStr(s, "Nsum") == "2" &&
                r.text == "               b: 2 mismatches\n"
                          "                  obs 2. 99 in master; 4 in using\n"
                          "                  obs 4. 98 in master; 8 in using",
            "t1meta cf_verbose (official.log:1001-1006): one line per differing observation. "
            "Got rc " + r.rc + " text\n" + r.text);
    (void)piet::dispatch("generate c = 1", s);
    r = run("cf c using \"" + f + "\"", s);
    w2check(r.rc == "9" && r.text == "               c: does not exist in using",
            "t1meta cf_notinusing (official.log:1011-1014 rc 9). Got rc " + r.rc + " `" + r.text + "`");
    (void)piet::dispatch("drop in 5", s);
    r = run("cf a using \"" + f + "\"", s);
    w2check(r.rc == "9" && r.msg == "master has 4 obs, using 5" && rStr(s, "Nsum") == "<unset>",
            "t1meta cf_nobs (official.log:1020-1023 rc 9): _N is compared first, r() left "
            "empty. Got rc " + r.rc + " `" + r.msg + "` Nsum `" + rStr(s, "Nsum") + "`");
    r = run("cf * using \"" + workdir + "/t1nosuch\"", s);
    w2check(r.rc == "601" && has(r.msg, "t1nosuch.dta not found"),
            "t1meta cf_nofile (official.log:1026-1029 r(601), .dta appended). Got rc " + r.rc +
                " `" + r.msg + "`");
    piet::Datastore* ds = s.datastore();
    w2check(ds != nullptr && ds->nRows() == 4 && varNames(s) == "a b s c" &&
                ds->col("b").f64()[1] == 99.0,
            "t1meta cf leaves the data untouched (official.log:1020: after `drop in 5` the master "
            "is still 4 obs with b[2] = 99). Got " + varNames(s));
  }

  // ---- fvunab (w2ax_t1_official.do (6)) ----------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 10", "generate t = _n", "generate tau1 = _n", "generate tau2 = _n^2",
            "generate tau3 = -_n", "generate other = 1"},
           s);
    auto r = run("fvunab t1L : l.tau1", s);
    w2check(r.rc == "111" && r.msg == "time variable not set",
            "t1meta fvunab ts operator without tsset (mx1.log FV|notset r(111)). Got rc " + r.rc +
                " `" + r.msg + "`");
    (void)piet::dispatch("tsset t", s);
    r = run("fvunab t1L : l.tau*", s);
    w2check(r.rc == "0" && local(s, "t1L") == "L.tau1 L.tau2 L.tau3",
            "t1meta fvunab_lag (official.log:761). Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : l7.tau*", s);
    w2check(local(s, "t1L") == "L7.tau1 L7.tau2 L7.tau3",
            "t1meta fvunab_lag7 (official.log:766). Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : tau* other", s);
    w2check(local(s, "t1L") == "tau1 tau2 tau3 other",
            "t1meta fvunab_plain (official.log:771). Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : i.tau1 c.tau2", s);
    w2check(local(s, "t1L") == "i.tau1 tau2",
            "t1meta fvunab_fv (official.log:776): `c.` is dropped. Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : d.tau1 f.tau2 L2.tau3", s);
    w2check(local(s, "t1L") == "D.tau1 F.tau2 L2.tau3",
            "t1meta fvunab_ops (official.log:781): canonical operator case. Got `" +
                local(s, "t1L") + "`");
    r = run("fvunab t1L : nosuch*", s);
    w2check(r.rc == "111", "t1meta fvunab_nomatch (official.log:786-787 r(111)). Got rc " + r.rc);
    r = run("fvunab t1L : tau1-tau3", s);
    w2check(local(s, "t1L") == "tau1 tau2 tau3",
            "t1meta fvunab_range (official.log:792). Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : L(1/2).tau1 dl.tau2 l2f.tau3", s);
    w2check(local(s, "t1L") == "L.tau1 L2.tau1 LD.tau2 L.tau3",
            "t1meta fvunab numlist + combined operators (mx1/mx2.log FV|L(1/2).tau1, FV|dl.tau1, "
            "FV|l2f.tau1). Got `" + local(s, "t1L") + "`");
    r = run("fvunab t1L : i.tau1#c.tau2", s);
    w2check(r.rc == "199" && has(r.msg, "interactions"),
            "t1meta fvunab refusal: an interaction (Stata: i.tau1#c.tau2, mx1.log) is refused by "
            "name, not mangled. Got rc " + r.rc + " `" + r.msg + "`");
  }

  // ---- numlabel (w2ax_t1_official.do (8)) --------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 3", "generate v = _n", "generate w = _n",
            "label define t1s 1 \"a\" 2 \"b\" 3 \"c\"", "label define t1u 1 \"one\" 2 \"two\"",
            "label values v t1s", "label values w t1u"},
           s);
    auto r = run("numlabel, add", s);
    w2check(r.rc == "0" && lbl(s, "t1s", "1") == "1. a" && lbl(s, "t1s", "3") == "3. c" &&
                lbl(s, "t1u", "2") == "2. two" && varValueLabel(s, "v", 1) == "1. a" &&
                varValueLabel(s, "w", 2) == "2. two",
            "t1meta numlabel_add (official.log:890 `1. a|3. c|2. two`), in both label stores. Got `" +
                lbl(s, "t1s", "1") + "|" + lbl(s, "t1s", "3") + "|" + lbl(s, "t1u", "2") +
                "` valueLabels v[1] `" + varValueLabel(s, "v", 1) + "`");
    r = run("numlabel, add", s);
    w2check(r.rc == "0" && lbl(s, "t1s", "1") == "1. a" && r.text == "(no value label to be modified)",
            "t1meta numlabel_add_again (official.log:893-896): no double prefix. Got `" +
                lbl(s, "t1s", "1") + "` text `" + r.text + "`");
    r = run("numlabel t1s, remove", s);
    w2check(lbl(s, "t1s", "1") == "a" && lbl(s, "t1u", "1") == "1. one",
            "t1meta numlabel_remove (official.log:901 `a|1. one`): only the named set. Got `" +
                lbl(s, "t1s", "1") + "|" + lbl(s, "t1u", "1") + "`");
    r = run("numlabel t1s, add mask(\"[#] \")", s);
    w2check(lbl(s, "t1s", "2") == "[2] b",
            "t1meta numlabel_mask (official.log:906 `[2] b`). Got `" + lbl(s, "t1s", "2") + "`");
    r = run("numlabel t1s, remove mask(\"[#] \")", s);
    w2check(lbl(s, "t1s", "2") == "b",
            "t1meta numlabel_mask_remove (official.log:911 `b`). Got `" + lbl(s, "t1s", "2") + "`");
    r = run("numlabel t1u, add force", s);
    w2check(r.rc == "0" && lbl(s, "t1u", "1") == "1. one",
            "t1meta numlabel_force (official.log:914-917 rc 0 `1. one`). Got rc " + r.rc);
    r = run("numlabel", s);
    w2check(r.rc == "198" && r.msg == "option add or remove should be specified",
            "t1meta numlabel_none (official.log:920-923 r(198)). Got rc " + r.rc + " `" + r.msg + "`");
    r = run("numlabel t1nosuch, add", s);
    w2check(r.rc == "111" && r.msg == "value label t1nosuch not found",
            "t1meta numlabel_badlbl (official.log:926-929 r(111)). Got rc " + r.rc + " `" + r.msg + "`");
    runAll({"label define p1 1 \"1. a\" 2 \"b\" 3 \"c\""}, s);
    r = run("numlabel p1, add", s);
    w2check(r.rc == "180" && lbl(s, "p1", "2") == "b",
            "t1meta numlabel refusal (mx1.log NL|partial r(180)): a partially prefixed set stops "
            "without force, nothing changed. Got rc " + r.rc + " `" + lbl(s, "p1", "2") + "`");
  }

  // ---- labvars (w2ax_t1_community.do (2)) --------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 2", "generate v1 = 1", "generate v2 = 2", "generate v3 = 3",
            "generate NewSector = 4"},
           s);
    auto r = run("labvars v1 v2 \"Label one\" \"Label two\"", s);
    w2check(r.rc == "0" && varLabel(s, "v1") == "Label one" && varLabel(s, "v2") == "Label two",
            "t1meta labvars_two (community.log:83). Got `" + varLabel(s, "v1") + "|" +
                varLabel(s, "v2") + "`");
    r = run("labvars NewSector \"Sector\"", s);
    w2check(varLabel(s, "NewSector") == "Sector",
            "t1meta labvars_one (community.log:88). Got `" + varLabel(s, "NewSector") + "`");
    r = run("labvars v1 v2 v3 \"only\" \"two\"", s);
    w2check(r.rc == "198" && r.msg == "3 variables but 2 labels specified" &&
                varLabel(s, "v1") == "Label one" && varLabel(s, "v3").empty(),
            "t1meta labvars_fewer (community.log:91-94 r(198)): nothing is applied. Got rc " +
                r.rc + " `" + r.msg + "` v1 `" + varLabel(s, "v1") + "`");
    r = run("labvars v3 \"a\" \"b\"", s);
    w2check(r.rc == "198" && r.msg == "1 variable but 2 labels specified" && varLabel(s, "v3").empty(),
            "t1meta labvars_more (community.log:97-100 r(198)). Got rc " + r.rc + " `" + r.msg + "`");
    r = run("labvars v3 \"with \"\"inner\"\" quote\"", s);
    w2check(r.rc == "198" && r.msg == "1 variable but 3 labels specified",
            "t1meta labvars_dq (community.log:103-106 r(198): the doubled quotes make 3 labels). "
            "Got rc " + r.rc + " `" + r.msg + "`");
    r = run("labvars v3 \"three\"", s);
    (void)r;
    r = run("labvars, remove", s);
    w2check(r.rc == "0" && varLabel(s, "v3").empty() && varLabel(s, "NewSector").empty(),
            "t1meta labvars `, remove` with no varlist clears every label (mx3.log LV|bareremove "
            "rc 0). Got rc " + r.rc + " `" + r.msg + "` v3 `" + varLabel(s, "v3") + "`");
    runAll({"label variable v1 \"Label one\"", "label variable v2 \"Label two\""}, s);
    r = run("labvars v1 v2, remove", s);
    w2check(r.rc == "0" && varLabel(s, "v1").empty() && varLabel(s, "v2").empty(),
            "t1meta labvars_remove (community.log:111). Got `" + varLabel(s, "v1") + "|" +
                varLabel(s, "v2") + "`");
  }

  // ---- renpfix (w2ax_t1_official.do (11)) --------------------------------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 2", "generate tau1 = 1", "generate tau2 = 2", "generate pre_a = 3",
            "generate pre_b = 4", "generate other = 5"},
           s);
    auto r = run("renpfix tau tt", s);
    w2check(r.rc == "0" && varNames(s) == "tt1 tt2 pre_a pre_b other" && rStr(s, "varlist") == "tt1 tt2",
            "t1meta renpfix_basic (official.log:1055). Got `" + varNames(s) + "` r(varlist) `" +
                rStr(s, "varlist") + "`");
    r = run("renpfix pre_", s);
    w2check(r.rc == "0" && varNames(s) == "tt1 tt2 a b other",
            "t1meta renpfix_drop (official.log:1062): no new prefix strips the old. Got `" +
                varNames(s) + "`");
    r = run("renpfix zz yy", s);
    w2check(r.rc == "111" && r.msg == "variable zz* not found",
            "t1meta renpfix_nomatch (official.log:1065-1068 r(111)). Got rc " + r.rc + " `" + r.msg + "`");
    runAll({"generate ttx = 6", "generate ux = 7"}, s);
    r = run("renpfix tt u", s);
    w2check(r.rc == "110" && r.msg == "variable ux already defined" &&
                varNames(s) == "u1 u2 a b other ttx ux",
            "t1meta renpfix_collide (official.log:1073-1080 r(110)): renames in order, the ones "
            "before the clash stay done. Got rc " + r.rc + " `" + varNames(s) + "`");
  }

  // ---- review follow-ups (StataBE captures review-meta p1-p10, mx6/mx7.log) ----------
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 5", "gen price = _n*10", "gen mpg = _n", "gen str3 make = \"a\" + string(_n)"},
           s);
    auto r = run("findname if price > 1000", s);
    w2check(r.rc == "2000" && r.msg == "no observations",
            "t1meta review p1 fn_if0 (r(2000)): an empty if-sample stops findname. Got rc " + r.rc +
                " `" + r.msg + "`");
    r = run("findname in 1/2", s);
    w2check(r.rc == "0" && rStr(s, "varlist") == "price mpg make",
            "t1meta review p1 fn_in (rc 0, every variable). Got rc " + r.rc + " `" + rStr(s, "varlist") + "`");
    r = run("findname, indent(-1)", s);
    w2check(r.rc == "0" && r.text == "price  mpg    make",
            "t1meta review p1 fn_indneg: a negative indent() is 0. Got rc " + r.rc + " `" + r.text + "`");
  }
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 6", "gen double id = 1234567890 + 1000000000*(_n > 3)",
            "gen t = mod(_n-1,3) + 1", "xtset id t"},
           s);
    auto r = run("xtdescribe", s);
    w2check(r.rc == "0" && has(r.text, "      id:  1.235e+09, 2.235e+09, ..., 2.235e+09              n =          2"),
            "t1meta review p2 xd_bigid: the header prints ids as display does (1.235e+09). Got\n" + r.text);
    r = run("xtdescribe, width(2)", s);
    w2check(r.rc == "0" &&
                has(r.text, "width must be a positive integer greater than 2\n\n      id:") &&
                has(r.text, "        2    100.00  100.00 |  21") &&
                has(r.text, "        2    100.00         |  XX\n --------------------------------------\n"
                            " *Each column represents 2 periods.\n") &&
                r.text.back() == '\n',
            "t1meta review p2 xd_w2: width(2) is noted and used (two periods a column; the closing "
            "rule is all dashes, then a blank line). Got\n" + r.text);
    runAll({"clear", "set obs 3", "gen x = _n", "quietly summarize x"}, s);
    r = run("xtdescribe", s);
    w2check(r.rc == "459" && std::isnan(rVal(s, "N")),
            "t1meta review p2 xd_rnotset (459, r(N) `.`): r() is replaced even when xtdescribe "
            "stops at the panel check. Got rc " + r.rc + " r(N) " + std::to_string(rVal(s, "N")));
  }
  {
    piet::SessionState s(workdir);
    const std::string f = workdir + "/cfu3";
    runAll({"clear", "set obs 3", "gen x = _n", "gen double y = 1234567890 + _n", "gen z = _n/3"}, s);
    (void)piet::dispatch("save \"" + f + "\", replace", s);
    runAll({"replace y = y + 1 in 2", "replace z = z + 1 in 3"}, s);
    auto r = run("cf _all using \"" + f + "\", verbose", s);
    w2check(r.rc == "9" && rStr(s, "Nsum") == "2" &&
                has(r.text, "                  obs 2. 1.235e+09 in master; 1.235e+09 in using") &&
                has(r.text, "                  obs 3. 2 in master; 1 in using"),
            "t1meta review p3 cf_big: verbose values print as display does. Got rc " + r.rc + "\n" + r.text);
    r = run("cf x x using \"" + f + "\"", s);
    w2check(r.rc == "198" && r.msg == "nothing found where name expected",
            "t1meta review p3 cf_dup (r(198)): a variable named twice. Got rc " + r.rc + " `" + r.msg + "`");
  }
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 3", "gen b = _n - 2", "gen abc = _n",
            "label define yn -1 \"neg\" 0 \"zero\" 1 \"one\"", "label define ab 1 \"x1\" 2 \"x2\"",
            "label values b yn", "label values abc ab"},
           s);
    auto r = run("numlabel _all, add", s);
    w2check(r.rc == "0" && lbl(s, "yn", "1") == "1. one" && lbl(s, "ab", "2") == "2. x2",
            "t1meta review p6 nl_all: `_all` is every value label. Got rc " + r.rc + " `" +
                lbl(s, "yn", "1") + "|" + lbl(s, "ab", "2") + "`");
    r = run("numlabel y*, remove", s);
    w2check(r.rc == "198" && r.msg == "* invalid name" && lbl(s, "yn", "-1") == "-1. neg",
            "t1meta review p4 nl_wild (r(198) `* invalid name`). Got rc " + r.rc + " `" + r.msg + "`");
    runAll({"numlabel _all, remove", "numlabel yn, add mask(\"# - \")"}, s);
    (void)piet::dispatch("quietly summarize b", s);
    r = run("numlabel yn, remove mask(# - )", s);
    w2check(r.rc == "0" && lbl(s, "yn", "1") == " one" && std::isnan(rVal(s, "N")),
            "t1meta review p4/p5 nl_mask_sp2 + p9 nl_r: an unquoted mask loses its outer blanks "
            "(` one` keeps its own), and numlabel replaces r(). Got `" + lbl(s, "yn", "1") +
                "` r(N) " + std::to_string(rVal(s, "N")));
    (void)piet::dispatch("quietly summarize b", s);
    r = run("numlabel yn", s);
    w2check(r.rc == "198" && rVal(s, "N") == 3,
            "t1meta review p9 nl_r_err: the add/remove check stops before r() is touched. Got rc " +
                r.rc + " r(N) " + std::to_string(rVal(s, "N")));
  }
  {
    piet::SessionState s(workdir);
    runAll({"clear", "set obs 3", "gen b = _n - 2", "gen abc = _n", "gen a = 1"}, s);
    auto r = run("labvars b \"lab b\" if b > 0", s);
    w2check(r.rc == "198" && r.msg == "1 variable but 5 labels specified" && varLabel(s, "b").empty(),
            "t1meta review p5 lv_if (r(198)): the qualifier's words are labels. Got rc " + r.rc +
                " `" + r.msg + "`");
    r = run("labvars b \"x\", alternate names", s);
    w2check(r.rc == "198" && r.msg == "option names not allowed" && varLabel(s, "b").empty(),
            "t1meta review p5 lv_altnam (r(198)): refused before anything is labelled. Got rc " +
                r.rc + " `" + r.msg + "` label `" + varLabel(s, "b") + "`");
    r = run("labvars b \"a\\b\"", s);
    w2check(r.rc == "0" && varLabel(s, "b") == "a\\b",
            "t1meta review p7 lv_bs: a backslash inside quotes is text. Got rc " + r.rc + " `" +
                varLabel(s, "b") + "`");
    r = run("renpfix a long", s);
    w2check(r.rc == "198" && r.msg == "long invalid varname" && varNames(s) == "b longbc a",
            "t1meta review p7 rp_resv (r(198)): a reserved name stops the loop after the renames "
            "before it. Got rc " + r.rc + " `" + r.msg + "` vars `" + varNames(s) + "`");
    runAll({"gen t = _n", "tsset t"}, s);
    r = run("fvunab m : L99999999999999999999.b", s);
    w2check(r.rc == "198" && r.msg == "L99999999999999999999:  operator invalid",
            "t1meta review p10 fv_ovf (r(198)): an operator number past 2^31 is refused, not "
            "thrown. Got rc " + r.rc + " `" + r.msg + "`");
    r = run("fvunab m : L(1/101).b", s);
    const auto r2 = run("fvunab m : L(1/2000000000).b", s);
    const auto r3 = run("fvunab m : L(1/100).b", s);
    w2check(r.rc == "198" && has(r.msg, "numlist in operator invalid") && r2.rc == "198" &&
                r3.rc == "0",
            "t1meta mx7.log FV|L(1/101).a: an operator numlist holds at most 100 values (counted "
            "before it is filled). Got rc " + r.rc + "/" + r2.rc + "/" + r3.rc);
  }

  fs::remove_all(workdir, ec);
}
