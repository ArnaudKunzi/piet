// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `destring` -- selftest arms (BUG-664, BUG-665; closes BUG-440).
//
// Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2b_dataio.log:76-140 -- tostring_fmt_replace
//     ("big was double now str11", big[1] 12345678902), tostring_nofmt_ctl
//     ("big cannot be converted reversibly; no replace", rc 0, big still
//     numeric), tostring_gen_fmt / tostring_gen_nofmt_ctl ("x cannot be
//     converted reversibly; no generate", rc 0, no xs), tostring_gen_force
//     (xs3[1] "7.010000229": the float value, rendered %12.0g).
//   w2b_dataio.log:330-403 -- destring_dpcomma (num 1.1 / 12.2, double),
//     destring_nodp_ctl ("num: contains nonnumeric characters; no replace",
//     rc 0, num still str5), destring_float (float), destring_nofloat_ctl
//     (double), destring_g_force (`g(sn)` rc 0, byte), destring_ignore
//     (ignore("a-z") -> sn2 1, 2).
//   tests/eval/golden/aer_b4/pins_oracle_r.log:25-45 -- B4REC tostring
//     replace_force (mpg int 22 -> "22", str2) and generate (mpgs str2).
//   tostring's shipped ado (version 3.0.0) and [D] destring -- the varlist
//     loop, the per-variable skip at rc 0, the generate() word-count r(198).
//   The storage-type arms (BUG-665, tostring's float-vs-double reading) rest on
//     StataBE 19.5 runs made for this task, not on a golden file:
//     `generate double d = 0.1` -> "d was double now str2", d[1] ".1";
//     `generate f = 0.1` -> "f cannot be converted reversibly; no replace";
//     drdid2x2_v118.dta (x1 double), `keep in 1/4`, format(%21.17f) -> sx1
//     "0.30471707975443135" / "-1.03998410624049553", str20, and the plain
//     `tostring x1, replace` -> the skip note; `egen double em = mean(xd)` ->
//     str2, `egen em2 = mean(xd)` -> skip note (egen defaults to float);
//     `collapse (mean) mxd=xd (sum) sli=li` -> mxd double ".3", sli double
//     "3"; `generate byte b = 1` / `replace b = 0.1 in 1` -> "b was byte now
//     float" and tostring skips, the same on a long -> "was long now double"
//     and tostring converts (".1", str2); merge keeps the using file's type
//     (a double stays double). PIET records no storage type for a collapse
//     output under a new name, so where float and double would give different
//     answers it refuses by name (r(198)) instead of guessing. merge/append now
//     carry a .dta using file's types (BUG-666). StataBE 19.5 (2026-09-27):
//     merge keeps yd double / yf float / il long and tostring il stores
//     "19950313"; append promotes float+double, long+double and long+float
//     to double and byte+int to int; `set type double` makes undeclared
//     gen/egen double (tostring id -> "123456789", x -> ".1") and survives
//     `clear all`; `set type int|DOUBLE|` (bare) are r(198).
//   tests/eval/golden/aer_w2/w2f_residuals.log:1824/1856/1898 (section F) --
//     append widens a float master to double over its float-rounded values:
//     master `generate x = 0.07*100` / `generate y = 123456789`, using double x
//     0.1 / long y 123456789 -> x, y double; x[1] +1.c000000000000X+002 (7),
//     x[3] +1.999999999999aX-004 (0.1), y[1] +1.d6f3460000000X+01a
//     (123456792), y[3] +1.d6f3454000000X+01a (123456789); tostring x ->
//     str2 "7" ".1"; tostring y -> str9 "123456792" "123456789".
//   internal capture exprfns_contract_oracle.log:646/655/735-737 --
//     a variable keeps the display format it was created with when its type
//     changes: "b1 was byte now float" -> b1 float %8.0g, "i1 was int now
//     long" -> i1 long %8.0g, `recast double y` of a float -> y double %9.0g.
//     tostring.ado's `usedisplayformat` reads that format; `generate` sets it
//     per storage type (w2b_dataio.log:204, double big %10.0g).
//   The destring corners rest on StataBE black-box runs: `float` rounds the stored value (and the /100 of
//     `percent`) to float, a float result is never demoted to long, and
//     `percent` alone takes the ignore() wording of the skip note.
// Every arm fails on the frozen main binary: there a varlist of two or more
// was refused outright, format()/float/dpcomma were r(198) "not allowed", a
// nonnumeric string was an error instead of a note, and tostring had no
// reversibility test (it converted big and x silently).
//
// BUG-928 (goal strconv-generate-column-order): destring's generate() puts
// NEWVAR right after its source (as StataBE does),
// oracle w2ab_strconv_order (StataBE 19.5 capture) arms so_d1_gen
// (w2ab_strconv_order.log:413), so_d3_gen_two (:464), so_d4_gen_force (:488),
// so_d5_gen_ignore (:534), so_d6_replace (:557), so_d7_range (:578). On bin/main 11df6141 the new
// column is appended last (`s t u w sn`, `s t u w sn tn`, `ds s-t` = `s t`).
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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
  std::string text;  // concatenated display-block text (the per-variable notes)
};

LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  LineResult r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "display") r.text += b.value("text", std::string()) + "\n";
    if (type != "error") continue;
    r.rc = piet::eval::coarseRc(b);
    r.msg = b.value("message", std::string());
  }
  return r;
}

std::string dtypeOf(piet::SessionState& session, const std::string& var) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var)) return "<absent>";
  return ds->dtype(var);
}

double num(piet::SessionState& session, const std::string& var, int64_t row) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "f64" || row >= ds->nRows()) return -999;
  return ds->col(var).f64()[row];
}

std::string str(piet::SessionState& session, const std::string& var, int64_t row) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "factor" || row >= ds->nRows())
    return "<not string>";
  const int32_t code = ds->col(var).codes()[row];
  const auto& lv = ds->col(var).levels;
  return (code >= 0 && static_cast<size_t>(code) < lv.size()) ? lv[static_cast<size_t>(code)] : "";
}

// A session's workdir must exist before the first command writes into it.
std::string subdir(const std::string& wd, const std::string& name) {
  const std::string d = wd + "/" + name;
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// The repo's own tests/ tree, from this file's compile-time path.
std::string repoFixture(const std::string& rel) {
  const std::string f = std::string(__FILE__);
  const auto pos = f.rfind("/piet/native/src/selftest_w2b_destring.cpp");
  return (pos == std::string::npos ? std::string(".") : f.substr(0, pos)) + "/" + rel;
}

std::string typeOf(piet::SessionState& session, const std::string& var) {
  auto it = session.varMeta.find(var);
  if (it == session.varMeta.end() || !it->second.stataType.has_value()) return "<none>";
  return *it->second.stataType;
}

// The dataset's column order as `ds` posts it in r(varlist) (the oracle's
// order field, w2ab_strconv_order.do `quietly ds` / `r(varlist)`).
std::string dsOrder(piet::SessionState& session, const std::string& args = "") {
  LineResult d = runLine("quietly ds" + args, session);
  if (d.rc != "0") return "<ds rc " + d.rc + ": " + d.msg + ">";
  std::string t = runLine("display \"`r(varlist)'\"", session).text;
  if (!t.empty() && t.back() == '\n') t.pop_back();
  return t;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b)); }

// w2b_dataio.do:1-16, less the columns these arms never read.
void setupBase(piet::SessionState& s) {
  (void)piet::dispatch("clear", s);
  (void)piet::dispatch("set obs 12", s);
  (void)piet::dispatch("generate id = ceil(_n/3)", s);
  (void)piet::dispatch("generate x = mod(_n*7,11) + _n/100", s);
  (void)piet::dispatch("generate double big = 12345678901 + _n*_n", s);
  (void)piet::dispatch("generate str6 s = \"a-\" + string(mod(_n,4)) + \"-z\"", s);
  (void)piet::dispatch("generate str5 num = string(_n) + \",\" + string(mod(_n,10))", s);
  (void)piet::dispatch("format x %9.2f", s);
}

}  // namespace

void testW2bDestring() {
  std::printf("[selftest] --- W2B destring/tostring varlist + Stata semantics (BUG-664/665) ---\n");
  const std::string wd = (fs::temp_directory_path() / "piet_selftest_w2b_destring").u8string();
  std::error_code ec;
  fs::remove_all(wd, ec);
  fs::create_directories(wd, ec);

  // ---- tostring (w2b_dataio.log:76-140) ----
  {
    piet::SessionState s(subdir(wd, "t1"));
    setupBase(s);
    auto r = runLine("tostring big, replace format(%20.0f)", s);
    w2check(r.rc == "0" && str(s, "big", 0) == "12345678902" && typeOf(s, "big") == "str11",
            "w2b_dataio tostring_fmt_replace: rc 0, big[1] \"12345678902\", str11 (got rc " +
                r.rc + " " + r.msg + ", big[1] `" + str(s, "big", 0) + "', " + typeOf(s, "big") +
                ")");
  }
  {
    piet::SessionState s(subdir(wd, "t2"));
    setupBase(s);
    auto r = runLine("tostring big, replace", s);
    w2check(r.rc == "0" && dtypeOf(s, "big") == "f64" && near(num(s, "big", 0), 12345678902.0) &&
                r.text.find("big cannot be converted reversibly; no replace") != std::string::npos,
            "w2b_dataio tostring_nofmt_ctl: %12.0g loses digits -> note, rc 0, big still numeric "
            "(got rc " + r.rc + ", dtype " + dtypeOf(s, "big") + ", text `" + r.text + "')");
  }
  {
    piet::SessionState s(subdir(wd, "t3"));
    setupBase(s);
    auto r = runLine("tostring x, generate(xs) format(%9.2f)", s);
    w2check(r.rc == "0" && dtypeOf(s, "xs") == "<absent>" &&
                r.text.find("x cannot be converted reversibly; no generate") != std::string::npos,
            "w2b_dataio tostring_gen_fmt: float x under %9.2f is irreversible -> no xs, rc 0 (got "
            "rc " + r.rc + " " + r.msg + ", xs " + dtypeOf(s, "xs") + ")");
    auto r2 = runLine("tostring x, generate(xs2)", s);
    w2check(r2.rc == "0" && dtypeOf(s, "xs2") == "<absent>",
            "w2b_dataio tostring_gen_nofmt_ctl: float x under %12.0g is irreversible -> no xs2, rc "
            "0 (got rc " + r2.rc + ", xs2 " + dtypeOf(s, "xs2") + ")");
    auto r3 = runLine("tostring x, generate(xs3) force", s);
    w2check(r3.rc == "0" && str(s, "xs3", 0) == "7.010000229" && typeOf(s, "xs3") == "str11",
            "w2b_dataio tostring_gen_force: xs3[1] \"7.010000229\" (the FLOAT value at %12.0g), "
            "str11 (got rc " + r3.rc + ", `" + str(s, "xs3", 0) + "', " + typeOf(s, "xs3") + ")");
  }
  // ---- B4REC tostring (pins_oracle_r.log:25-45) ----
  {
    piet::SessionState s(subdir(wd, "t4"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 3", s);
    (void)piet::dispatch("generate int mpg = 22", s);
    auto g = runLine("tostring mpg, generate(mpgs)", s);
    w2check(g.rc == "0" && str(s, "mpgs", 0) == "22" && typeOf(s, "mpgs") == "str2",
            "B4 pins B4REC tostring generate: mpgs \"22\" str2, rc 0 (got rc " + g.rc + ", `" +
                str(s, "mpgs", 0) + "', " + typeOf(s, "mpgs") + ")");
    auto r = runLine("tostring mpg, replace force", s);
    w2check(r.rc == "0" && str(s, "mpg", 0) == "22" && typeOf(s, "mpg") == "str2",
            "B4 pins B4REC tostring replace_force: rc 0, mpg \"22\" str2 (got rc " + r.rc + " " +
                r.msg + ", `" + str(s, "mpg", 0) + "', " + typeOf(s, "mpg") + ")");
  }

  // ---- destring (w2b_dataio.log:330-403) ----
  {
    piet::SessionState s(subdir(wd, "d1"));
    setupBase(s);
    auto r = runLine("destring num, replace dpcomma", s);
    w2check(r.rc == "0" && near(num(s, "num", 0), 1.1) && near(num(s, "num", 11), 12.2) &&
                typeOf(s, "num") == "double",
            "w2b_dataio destring_dpcomma: num[1] 1.1, num[12] 12.2, double (got rc " + r.rc + " " +
                r.msg + ", " + std::to_string(num(s, "num", 0)) + ", " + typeOf(s, "num") + ")");
  }
  {
    piet::SessionState s(subdir(wd, "d2"));
    setupBase(s);
    auto r = runLine("destring num, replace", s);
    w2check(r.rc == "0" && dtypeOf(s, "num") == "factor" &&
                r.text.find("num: contains nonnumeric characters; no replace") != std::string::npos,
            "w2b_dataio destring_nodp_ctl: note + rc 0, num still a string (got rc " + r.rc + " " +
                r.msg + ", dtype " + dtypeOf(s, "num") + ")");
  }
  {
    piet::SessionState s(subdir(wd, "d3"));
    setupBase(s);
    (void)piet::dispatch("generate str4 f = string(_n) + \".5\"", s);
    auto r = runLine("destring f, replace float", s);
    w2check(r.rc == "0" && near(num(s, "f", 0), 1.5) && typeOf(s, "f") == "float",
            "w2b_dataio destring_float: float (got rc " + r.rc + " " + r.msg + ", " +
                typeOf(s, "f") + ")");
  }
  {
    piet::SessionState s(subdir(wd, "d4"));
    setupBase(s);
    (void)piet::dispatch("generate str4 f = string(_n) + \".5\"", s);
    auto r = runLine("destring f, replace", s);
    w2check(r.rc == "0" && typeOf(s, "f") == "double",
            "w2b_dataio destring_nofloat_ctl: double (got rc " + r.rc + ", " + typeOf(s, "f") + ")");
    auto g = runLine("destring s, g(sn) force", s);
    w2check(g.rc == "0" && dtypeOf(s, "sn") == "f64" && std::isnan(num(s, "sn", 0)) &&
                typeOf(s, "sn") == "byte",
            "w2b_dataio destring_g_force: `g()` accepted, all missing, byte (got rc " + g.rc + " " +
                g.msg + ", " + typeOf(s, "sn") + ")");
    auto i = runLine("destring s, gen(sn2) ignore(\"a-z\")", s);
    w2check(i.rc == "0" && near(num(s, "sn2", 0), 1) && near(num(s, "sn2", 1), 2),
            "w2b_dataio destring_ignore: ignore(\"a-z\") is the characters a - z; sn2 1, 2 (got rc " +
                i.rc + " " + i.msg + ")");
  }

  // ---- the varlist itself (one conversion per variable, as in Stata) ----
  {
    piet::SessionState s(subdir(wd, "v1"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 3", s);
    (void)piet::dispatch("generate str3 a = string(_n)", s);
    (void)piet::dispatch("generate str3 b = \"x\" + string(_n)", s);
    (void)piet::dispatch("generate str5 c = string(_n) + \".25\"", s);
    auto r = runLine("destring a b c, replace", s);
    w2check(r.rc == "0" && dtypeOf(s, "a") == "f64" && typeOf(s, "a") == "byte" &&
                dtypeOf(s, "b") == "factor" && near(num(s, "c", 2), 3.25) &&
                typeOf(s, "c") == "double" &&
                r.text.find("b: contains nonnumeric characters; no replace") != std::string::npos,
            "destring 3-variable varlist: a byte, b left as is with the note, c double, rc 0 (got "
            "rc " + r.rc + " " + r.msg + ", a " + typeOf(s, "a") + ", b " + dtypeOf(s, "b") +
                ", c " + typeOf(s, "c") + ")");
    auto m = runLine("destring b c, gen(z1)", s);
    w2check(m.rc == "198" && dtypeOf(s, "z1") == "<absent>",
            "destring generate() with fewer names than variables is r(198), nothing created (got "
            "rc " + m.rc + ")");
    auto t = runLine("tostring a c, generate(sa sc)", s);
    w2check(t.rc == "0" && str(s, "sa", 1) == "2" && str(s, "sc", 2) == "3.25",
            "tostring 2-variable varlist with a 2-name generate(): sa \"2\", sc \"3.25\" (got rc " +
                t.rc + " " + t.msg + ", `" + str(s, "sa", 1) + "', `" + str(s, "sc", 2) + "')");
  }

  // ---- BUG-928: where generate() puts NEWVAR (w2ab_strconv_order) ----
  {
    auto setupStr = [&](piet::SessionState& t) {
      (void)piet::dispatch("clear", t);
      (void)piet::dispatch("set obs 3", t);
      (void)piet::dispatch("generate str1 s = string(_n)", t);
      (void)piet::dispatch("generate str2 t = string(20*_n)", t);
      (void)piet::dispatch("generate str2 u = \"x\" + string(_n)", t);
      (void)piet::dispatch("generate str5 w = \"1,00\" + string(_n)", t);
      (void)piet::dispatch("generate z = -_n", t);
    };
    // so_d1_gen / so_d3_gen_two / so_d7_range.
    piet::SessionState s(subdir(wd, "o1"));
    setupStr(s);
    auto d1 = runLine("destring s, gen(sn)", s);
    const std::string one = dsOrder(s);
    const std::string range = dsOrder(s, " s-t");
    (void)runLine("drop sn", s);
    auto d3 = runLine("destring s t, generate(sn tn)", s);
    const std::string two = dsOrder(s);
    w2check(d1.rc == "0" && d3.rc == "0" && one == "s sn t u w z" && range == "s sn t" &&
                two == "s sn t tn u w z" && num(s, "sn", 0) == 1.0 && num(s, "tn", 2) == 60.0,
            "w2ab_strconv_order so_d1_gen/so_d7_range/so_d3_gen_two: gen(sn) -> `s sn t u w z`, "
            "`ds s-t` -> `s sn t`, gen(sn tn) -> `s sn t tn u w z` (got `" + one +
                "' / `" + range + "' / `" + two + "')");
    // so_d4_gen_force / so_d5_gen_ignore / so_d6_replace.
    piet::SessionState f(subdir(wd, "o2"));
    setupStr(f);
    auto d4 = runLine("destring u, gen(un) force", f);
    auto d5 = runLine("destring w, gen(wn) ignore(\",\")", f);
    auto d6 = runLine("destring s, replace", f);
    const std::string all = dsOrder(f);
    w2check(d4.rc == "0" && d5.rc == "0" && d6.rc == "0" && all == "s t u un w wn z" &&
                num(f, "wn", 2) == 1003.0 && dtypeOf(f, "s") == "f64",
            "w2ab_strconv_order so_d4_gen_force/so_d5_gen_ignore/so_d6_replace: force and ignore() "
            "place un/wn after u/w, replace keeps s first: `s t u un w wn z` (got `" + all + "')");
  }

  // ---- tostring's storage-type reading (BUG-665) ----
  // A declared double renders the double; Stata's generate default (float)
  // renders float(v). Before, every undeclared column read as float.
  {
    piet::SessionState s(subdir(wd, "s1"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate double d = 0.1", s);
    auto r = runLine("tostring d, replace", s);
    w2check(r.rc == "0" && str(s, "d", 0) == ".1" && typeOf(s, "d") == "str2",
            "tostring on `generate double d = 0.1` converts: d \".1\", str2, rc 0 (got rc " +
                r.rc + " " + r.msg + ", `" + str(s, "d", 0) + "', " + typeOf(s, "d") + ")");
    (void)piet::dispatch("generate f = 0.1", s);
    auto f = runLine("tostring f, replace", s);
    w2check(f.rc == "0" && dtypeOf(s, "f") == "f64" &&
                f.text.find("f cannot be converted reversibly; no replace") != std::string::npos,
            "tostring on `generate f = 0.1` (float) skips with the note, rc 0, f still numeric "
            "(tostring_gen_nofmt_ctl) (got rc " + f.rc + ", dtype " + dtypeOf(s, "f") + ")");
  }
  {
    // `replace` promotes a declared byte/int to float and a long to double
    // when it stores a fraction; PIET keeps the old declaration.
    piet::SessionState s(subdir(wd, "s5"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate byte b = 1", s);
    (void)piet::dispatch("replace b = 0.1 in 1", s);
    (void)piet::dispatch("generate long l = 1", s);
    (void)piet::dispatch("replace l = 0.1 in 1", s);
    auto b = runLine("tostring b, replace", s);
    w2check(b.rc == "0" && dtypeOf(s, "b") == "f64" &&
                b.text.find("b cannot be converted reversibly; no replace") != std::string::npos,
            "tostring on a byte promoted to float by `replace b = 0.1` skips with the note (got rc " +
                b.rc + ", dtype " + dtypeOf(s, "b") + ")");
    auto l = runLine("tostring l, replace", s);
    w2check(l.rc == "0" && str(s, "l", 0) == ".1" && typeOf(s, "l") == "str2",
            "tostring on a long promoted to double by `replace l = 0.1` converts: \".1\", str2 "
            "(got rc " + l.rc + " " + l.msg + ", `" + str(s, "l", 0) + "')");
  }
  {
    const std::string dta = repoFixture("tests/eval/piet_d2d3/drdid2x2_v118.dta");
    const bool have = fs::exists(dta, ec);
    w2check(have, "fixture present: " + dta);
    if (have) {
      piet::SessionState s(subdir(wd, "s2"));
      (void)piet::dispatch("use \"" + dta + "\", clear", s);
      (void)piet::dispatch("keep in 1/4", s);
      auto r = runLine("tostring x1, generate(sx1) format(%21.17f)", s);
      w2check(r.rc == "0" && str(s, "sx1", 0) == "0.30471707975443135" &&
                  str(s, "sx1", 2) == "-1.03998410624049553" && typeOf(s, "sx1") == "str20",
              "tostring on a .dta double (drdid2x2_v118.dta x1) renders the double, not float(x1) "
              "(got rc " + r.rc + " " + r.msg + ", `" + str(s, "sx1", 0) + "', `" +
                  str(s, "sx1", 2) + "', " + typeOf(s, "sx1") + ")");
      auto d = runLine("tostring x1, replace", s);
      w2check(d.rc == "0" && dtypeOf(s, "x1") == "f64" &&
                  d.text.find("x1 cannot be converted reversibly; no replace") != std::string::npos,
              "tostring x1 at %12.0g skips with the note (a double that %12.0g cannot hold) "
              "(got rc " + d.rc + ", dtype " + dtypeOf(s, "x1") + ")");
    }
  }
  {
    piet::SessionState s(subdir(wd, "s3"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 4", s);
    (void)piet::dispatch("generate g = _n > 2", s);
    (void)piet::dispatch("generate double xd = cond(mod(_n,2), 0.25, 0.35)", s);
    (void)piet::dispatch("generate long li = _n", s);
    (void)piet::dispatch("egen double em = mean(xd)", s);
    (void)piet::dispatch("egen em2 = mean(xd)", s);
    auto e = runLine("tostring em, replace", s);
    w2check(e.rc == "0" && str(s, "em", 0) == ".3" && typeOf(s, "em") == "str2",
            "tostring on `egen double` converts: \".3\", str2 (got rc " + e.rc + " " + e.msg +
                ", `" + str(s, "em", 0) + "')");
    auto e2 = runLine("tostring em2, replace", s);
    w2check(e2.rc == "0" && dtypeOf(s, "em2") == "f64" &&
                e2.text.find("em2 cannot be converted reversibly; no replace") != std::string::npos,
            "tostring on a default (float) egen skips with the note (got rc " + e2.rc + ")");
    (void)piet::dispatch("generate xf = cond(mod(_n,2), 0.25, 0.35)", s);
    (void)piet::dispatch("collapse (mean) mxd=xd mxf=xf (sum) sli=li, by(g)", s);
    auto c = runLine("tostring sli, replace", s);
    w2check(c.rc == "0" && str(s, "sli", 0) == "3" && typeOf(s, "sli") == "str1",
            "tostring on a collapse output of float-exact values converts whatever its type: "
            "sli \"3\", str1 (got rc " + c.rc + " " + c.msg + ", `" + str(s, "sli", 0) + "')");
    // collapse makes the mean of a double source double, so mxd
    // has a recorded type and converts as StataBE does.
    auto m = runLine("tostring mxd, replace", s);
    w2check(m.rc == "0" && str(s, "mxd", 0) == ".3" && typeOf(s, "mxd") == "str2",
            "tostring on a collapse (mean) of a double converts: \".3\", str2 (got rc " + m.rc +
                " " + m.msg + ", `" + str(s, "mxd", 0) + "')");
    // The mean of an untyped (float) source is c(type), float:
    // the engine's mean of the float-held 0.25 and 0.35 renders ".3", which
    // reads back as neither float nor double, so StataBE skips with the note,
    // as on the default (float) egen above.
    auto f = runLine("tostring mxf, replace", s);
    // `: type' through the macro layer (a do-source line: dispatch alone does
    // not run the extended macro functions).
    (void)piet::doexec::executeDoSource("local w2bmxft : type mxf\n", s, piet::dispatch);
    const std::string ft = s.eval.locals.get("w2bmxft", "<unset>");
    w2check(f.rc == "0" && dtypeOf(s, "mxf") == "f64" && ft.rfind("float", 0) == 0 &&
                f.text.find("mxf cannot be converted reversibly; no replace") != std::string::npos,
            "tostring on a collapse (mean) of a float skips with the note (got rc " + f.rc + " " +
                f.msg + ", `: type' " + ft + ")");
  }
  {
    // BUG-666: a merged-in variable keeps the using file's storage type (a
    // double stays double, a long stays long, a float stays float). Before,
    // it had none: yd and il were refused by name, r(198).
    piet::SessionState s(subdir(wd, "s4"));
    const std::string usingPath = wd + "/s4_using.dta";
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate g = _n - 1", s);
    (void)piet::dispatch("generate double yh = g + 0.5", s);
    (void)piet::dispatch("generate double yd = g + 0.1", s);
    // 0.5, not 0.1: PIET keeps gen values unrounded, and the .dta writer honours
    // a float claim only when float is lossless (src/io/dta_writer.cpp), so a
    // PIET-saved g + 0.1 is written double. A Stata-written float is below.
    (void)piet::dispatch("generate yf = g + 0.5", s);
    (void)piet::dispatch("generate long il = 19950313 + g", s);
    (void)piet::dispatch("save \"" + usingPath + "\", replace", s);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 4", s);
    (void)piet::dispatch("generate g = _n > 2", s);
    auto mg = runLine("merge m:1 g using \"" + usingPath + "\", nogenerate", s);
    w2check(mg.rc == "0" && typeOf(s, "yd") == "double" && typeOf(s, "il") == "long" &&
                typeOf(s, "yf") == "float",
            "merge records the using file's storage types: yd double, il long, yf float (got rc " +
                mg.rc + ", " + typeOf(s, "yd") + ", " + typeOf(s, "il") + ", " + typeOf(s, "yf") +
                ")");
    auto h = runLine("tostring yh, replace", s);
    w2check(h.rc == "0" && str(s, "yh", 0) == ".5" && str(s, "yh", 3) == "1.5",
            "tostring on a merged-in column of float-exact values converts (got rc " + h.rc + " " +
                h.msg + ", `" + str(s, "yh", 0) + "', `" + str(s, "yh", 3) + "')");
    auto d = runLine("tostring yd, replace", s);
    w2check(d.rc == "0" && str(s, "yd", 0) == ".1" && str(s, "yd", 3) == "1.1" &&
                typeOf(s, "yd") == "str3",
            "tostring on a merged-in double converts: \".1\", \"1.1\", str3 (got rc " + d.rc + " " +
                d.msg + ", `" + str(s, "yd", 0) + "', `" + str(s, "yd", 3) + "', " +
                typeOf(s, "yd") + ")");
    auto l = runLine("tostring il, replace", s);
    w2check(l.rc == "0" && str(s, "il", 0) == "19950313" && str(s, "il", 3) == "19950314" &&
                typeOf(s, "il") == "str8",
            "tostring on a merged-in long above 2^24 renders the long, not float(il) (paper 089 "
            "Orbis/extract_data.do:281-296 incorpdate): \"19950313\" (got rc " + l.rc + " " + l.msg +
                ", `" + str(s, "il", 0) + "', `" + str(s, "il", 3) + "')");
    auto f = runLine("tostring yf, replace", s);
    w2check(f.rc == "0" && str(s, "yf", 0) == ".5" && str(s, "yf", 3) == "1.5",
            "tostring on a merged-in float of float-exact values converts (got rc " + f.rc + " " +
                f.msg + ")");
  }
  {
    // BUG-666: a Stata-written float (tests/eval/golden/f16_tw_fixture.dta y,
    // float 1..4) brought in by append keeps its float type.
    const std::string dta = repoFixture("tests/eval/golden/f16_tw_fixture.dta");
    const bool have = fs::exists(dta, ec);
    w2check(have, "fixture present: " + dta);
    if (have) {
      piet::SessionState s(subdir(wd, "s8"));
      (void)piet::dispatch("clear", s);
      (void)piet::dispatch("set obs 1", s);
      (void)piet::dispatch("generate double z = 1", s);
      auto ap = runLine("append using \"" + dta + "\"", s);
      w2check(ap.rc == "0" && typeOf(s, "y") == "float" && typeOf(s, "z") == "double",
              "append of a Stata-written float records y float (got rc " + ap.rc + " " + ap.msg +
                  ", " + typeOf(s, "y") + ", " + typeOf(s, "z") + ")");
    }
  }
  {
    // BUG-666: append. A variable only the using file has takes its type; one
    // both hold is promoted (master float x against using double x -> double,
    // so the float claim of the master's `generate x = 1` no longer rounds the
    // using rows' 0.1 to float and skips).
    piet::SessionState s(subdir(wd, "s6"));
    const std::string usingPath = wd + "/s6_using.dta";
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate double x = cond(_n == 1, 0.1, 1.1)", s);
    (void)piet::dispatch("generate double yd = _n + 0.1", s);
    (void)piet::dispatch("generate lf = 0.5", s);
    (void)piet::dispatch("generate int ib = 300", s);
    (void)piet::dispatch("save \"" + usingPath + "\", replace", s);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate x = 1", s);
    (void)piet::dispatch("generate long lf = 5", s);
    (void)piet::dispatch("generate byte ib = 1", s);
    auto ap = runLine("append using \"" + usingPath + "\"", s);
    w2check(ap.rc == "0" && typeOf(s, "x") == "double" && typeOf(s, "yd") == "double" &&
                typeOf(s, "lf") == "double" && typeOf(s, "ib") == "int",
            "append records x double (float+double), yd double (using only), lf double "
            "(long+float), ib int (byte+int) (got rc " + ap.rc + " " + ap.msg + ", " +
                typeOf(s, "x") + ", " + typeOf(s, "yd") + ", " + typeOf(s, "lf") + ", " +
                typeOf(s, "ib") + ")");
    auto x = runLine("tostring x, replace", s);
    w2check(x.rc == "0" && str(s, "x", 0) == "1" && str(s, "x", 1) == ".1" &&
                str(s, "x", 2) == "1.1",
            "tostring on the appended double x converts: \"1\", \".1\", \"1.1\" (got rc " + x.rc +
                " " + x.msg + ", `" + str(s, "x", 1) + "', `" + str(s, "x", 2) + "')");
    auto y = runLine("tostring yd, replace", s);
    w2check(y.rc == "0" && str(s, "yd", 0) == "." && str(s, "yd", 1) == "1.1" &&
                str(s, "yd", 2) == "2.1",
            "tostring on a using-only double converts: \".\", \"1.1\", \"2.1\" (got rc " + y.rc +
                " " + y.msg + ", `" + str(s, "yd", 1) + "')");
  }
  {
    // BUG-666: append's promotion to double over master rows PIET holds
    // unrounded. StataBE's master `gen x = 0.1` is float(0.1); widened to
    // double it renders ".1000000015" under %12.0g (the float rendering of
    // w2b_dataio.log:132 tostring_gen_force, "7.010000229"), so tostring skips
    // with the note; `gen y = 123456789` is stored as 123456792
    // (w2f_residuals.log:1824/1898). append rounds a float master's rows to
    // float before the widening, so x and y are claimed double over Stata's
    // values. A renamed gen (q) keeps its float claim (BUG-471 T2's rename), so
    // the same append rule (w2f_residuals.log:1824/1856/1898) rounds it to
    // float(0.1) and claims double: tostring q skips with the note and
    // `force` stores ".1000000015", str11. A float-exact master (z = 0.5) is
    // promoted as before. Seeded RED: on the round-4 code x and y were left
    // with no claim and tostring refused both, r(198).
    piet::SessionState s(subdir(wd, "s6b"));
    const std::string usingPath = wd + "/s6b_using.dta";
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate double x = 0.25", s);
    (void)piet::dispatch("generate long y = 5", s);
    (void)piet::dispatch("generate double q = 0.25", s);
    (void)piet::dispatch("generate double z = 0.1", s);
    (void)piet::dispatch("save \"" + usingPath + "\", replace", s);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate x = 0.1", s);
    (void)piet::dispatch("generate y = 123456789", s);
    (void)piet::dispatch("generate q0 = 0.1", s);
    (void)piet::dispatch("rename q0 q", s);  // keeps the float claim (BUG-471 T2)
    (void)piet::dispatch("generate z = 0.5", s);
    const std::string qBefore = typeOf(s, "q");
    auto ap = runLine("append using \"" + usingPath + "\"", s);
    w2check(ap.rc == "0" && typeOf(s, "x") == "double" && typeOf(s, "y") == "double" &&
                typeOf(s, "q") == "double" && typeOf(s, "z") == "double" &&
                num(s, "x", 0) == static_cast<double>(static_cast<float>(0.1)) &&
                num(s, "y", 0) == 123456792.0 &&
                num(s, "q", 0) == static_cast<double>(static_cast<float>(0.1)),
            "append rounds the float masters x, y and the renamed q to float and claims double; "
            "z double (got rc " + ap.rc + " " + ap.msg + ", " +
                typeOf(s, "x") + ", " + typeOf(s, "y") + ", " + qBefore + "/" + typeOf(s, "q") +
                ", " + typeOf(s, "z") + ")");
    auto x = runLine("tostring x, replace", s);
    w2check(x.rc == "0" && dtypeOf(s, "x") == "f64" &&
                x.text.find("x cannot be converted reversibly; no replace") != std::string::npos,
            "tostring x after append skips with the note: float(0.1) widened renders "
            "\".1000000015\" (got rc " + x.rc + " " + x.msg + ", " + dtypeOf(s, "x") + ")");
    auto y = runLine("tostring y, replace", s);
    w2check(y.rc == "0" && str(s, "y", 0) == "123456792" && str(s, "y", 1) == "5" &&
                typeOf(s, "y") == "str9",
            "tostring y after append stores Stata's float value \"123456792\", then \"5\", str9 "
            "(got rc " + y.rc + " " + y.msg + ", `" + str(s, "y", 0) + "', " + typeOf(s, "y") + ")");
    auto q = runLine("tostring q, replace", s);
    w2check(q.rc == "0" && dtypeOf(s, "q") == "f64" &&
                q.text.find("q cannot be converted reversibly; no replace") != std::string::npos,
            "tostring q (a renamed float master) after append skips with the note, q stays "
            "numeric (got rc " + q.rc + " " + q.msg + ", " + dtypeOf(s, "q") + ")");
    auto qf = runLine("tostring q, replace force", s);
    w2check(qf.rc == "0" && str(s, "q", 0) == ".1000000015" && typeOf(s, "q") == "str11",
            "tostring q, force after append stores \".1000000015\", str11 (got rc " + qf.rc + " " +
                qf.msg + ", `" + str(s, "q", 0) + "', " + typeOf(s, "q") + ")");
    auto z = runLine("tostring z, replace", s);
    w2check(z.rc == "0" && str(s, "z", 0) == ".5" && str(s, "z", 1) == ".1",
            "tostring on a float-exact master promoted to double converts: \".5\", \".1\" (got rc " +
                z.rc + " " + z.msg + ", `" + str(s, "z", 0) + "', `" + str(s, "z", 1) + "')");
  }
  {
    // s6b, the review's mixed column. Master `gen x = 0.07*100`: Stata holds
    // float 7, PIET 7.000000000000001; the using double 0.1 is exact. (The
    // review's other example, 0.1*30, is exactly 3.0 in IEEE double, so it is
    // float-exact and never discriminated.) w2f_residuals.log:1856
    // f_tostring_x: after the widening StataBE holds 7 and 0.1 and converts,
    // str2 "7" ".1". append now rounds the master row to float 7 and claims
    // double, so tostring converts the same. Seeded RED: the round-4 code left
    // x unclaimed and refused it by name, r(198) "x has no recorded storage
    // type". A row irreversible under both readings (`gen w = 1/3`, float
    // .3333333433 widened) still skips with the note.
    piet::SessionState s(subdir(wd, "s6b_mixed"));
    const std::string usingPath = wd + "/s6b_mixed_using.dta";
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate double x = 0.1", s);
    (void)piet::dispatch("generate double w = 0.1", s);
    (void)piet::dispatch("save \"" + usingPath + "\", replace", s);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate x = 0.07*100", s);
    (void)piet::dispatch("generate w = 1/3", s);
    auto ap = runLine("append using \"" + usingPath + "\"", s);
    w2check(ap.rc == "0" && typeOf(s, "x") == "double" && typeOf(s, "w") == "double" &&
                num(s, "x", 0) == 7.0 && num(s, "x", 1) == 0.1,
            "append widens the float masters x and w to double over float-rounded rows: x[1] 7, "
            "x[2] 0.1 (got rc " + ap.rc + " " + ap.msg + ", " + typeOf(s, "x") + ", " +
                typeOf(s, "w") + ")");
    auto x = runLine("tostring x, replace", s);
    w2check(x.rc == "0" && str(s, "x", 0) == "7" && str(s, "x", 1) == ".1" &&
                typeOf(s, "x") == "str2",
            "w2f_residuals f_tostring_x shape: tostring on the appended float+double column "
            "converts: \"7\", \".1\", str2 (got rc " + x.rc + " " + x.msg + ", `" + str(s, "x", 0) +
                "', `" + str(s, "x", 1) + "', " + typeOf(s, "x") + ")");
    auto w = runLine("tostring w, replace", s);
    w2check(w.rc == "0" && dtypeOf(s, "w") == "f64" &&
                w.text.find("w cannot be converted reversibly; no replace") != std::string::npos,
            "tostring on a column with a row irreversible under both readings skips with the "
            "note, rc 0 (got rc " + w.rc + " " + w.msg + ", " + dtypeOf(s, "w") + ")");
  }
  {
    // w2f_residuals.do section F (golden :1824/:1856/:1898), the datasets as
    // that file builds them. The master is used in memory here: PIET's `save`
    // writes a float claim that is not float-exact as double or long (the
    // .dta writer's lossless rule), so a save/use round trip of the master
    // does not hold Stata's float values (BUG-709, OPEN). The append and
    // tostring that follow are what this pins.
    const std::string usingPath = wd + "/w2f_using.dta";
    auto build = [&](piet::SessionState& s) {
      (void)piet::dispatch("clear", s);
      (void)piet::dispatch("set obs 2", s);
      (void)piet::dispatch("generate double x = 0.1", s);
      (void)piet::dispatch("generate long y = 123456789", s);
      (void)piet::dispatch("generate id = _n+2", s);
      (void)piet::dispatch("save \"" + usingPath + "\", replace", s);
      (void)piet::dispatch("clear", s);
      (void)piet::dispatch("set obs 2", s);
      (void)piet::dispatch("generate x = 0.07*100", s);
      (void)piet::dispatch("generate y = 123456789", s);
      (void)piet::dispatch("generate id = _n", s);
      return runLine("append using \"" + usingPath + "\"", s);
    };
    auto macroText = [&](piet::SessionState& s, const std::string& cmd) {
      (void)piet::dispatch(cmd, s);
      std::string t = runLine("display \"[`t']\"", s).text;
      t.erase(std::remove(t.begin(), t.end(), '\n'), t.end());
      return t;
    };
    piet::SessionState s(subdir(wd, "w2f"));
    auto ap = build(s);
    const std::string tx = macroText(s, "local t : type x");
    const std::string ty = macroText(s, "local t : type y");
    w2check(ap.rc == "0" && tx == "[double]" && ty == "[double]" && num(s, "x", 0) == 7.0 &&
                num(s, "x", 2) == 0.1 && num(s, "y", 0) == 123456792.0 &&
                num(s, "y", 2) == 123456789.0,
            "w2f_residuals f_append_types_ctl: rc 0, `: type` x double, y double, x[1] 7, x[3] "
            "0.1, y[1] 123456792, y[3] 123456789 (got rc " + ap.rc + " " + ap.msg + ", " + tx +
                " " + ty + ", " + std::to_string(num(s, "x", 0)) + ", " +
                std::to_string(num(s, "y", 0)) + ")");
    auto x = runLine("tostring x, replace", s);
    w2check(x.rc == "0" && typeOf(s, "x") == "str2" && str(s, "x", 0) == "7" &&
                str(s, "x", 2) == ".1",
            "w2f_residuals f_tostring_x: rc 0, str2, \"7\", \".1\" (got rc " + x.rc + " " + x.msg +
                ", " + typeOf(s, "x") + ", `" + str(s, "x", 0) + "', `" + str(s, "x", 2) + "')");
    piet::SessionState s2(subdir(wd, "w2f_y"));
    auto ap2 = build(s2);
    auto y = runLine("tostring y, replace", s2);
    w2check(ap2.rc == "0" && y.rc == "0" && typeOf(s2, "y") == "str9" &&
                str(s2, "y", 0) == "123456792" && str(s2, "y", 2) == "123456789",
            "w2f_residuals f_tostring_y: rc 0, str9, \"123456792\", \"123456789\" (got rc " + y.rc +
                " " + y.msg + ", " + typeOf(s2, "y") + ", `" + str(s2, "y", 0) + "', `" +
                str(s2, "y", 2) + "')");
  }
  {
    // usedisplayformat with no recorded display format: Stata's default for the
    // storage type (long %12.0g, double %10.0g; w2b_dataio.log:204 describes
    // `generate double big` as %10.0g). Before, %9.0g rendered "1.23e+08" /
    // "1.23e+07" and tostring skipped with the note.
    piet::SessionState s(subdir(wd, "s9"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate long L = 123456789", s);
    (void)piet::dispatch("generate double D = 12345678", s);
    auto l = runLine("tostring L, replace usedisplayformat", s);
    w2check(l.rc == "0" && str(s, "L", 0) == "123456789" && typeOf(s, "L") == "str9",
            "tostring on a long under usedisplayformat renders %12.0g: \"123456789\", str9 (got "
            "rc " + l.rc + " " + l.msg + ", `" + str(s, "L", 0) + "', text `" + l.text + "')");
    auto d = runLine("tostring D, replace usedisplayformat", s);
    w2check(d.rc == "0" && str(s, "D", 0) == "12345678" && typeOf(s, "D") == "str8",
            "tostring on a double under usedisplayformat renders %10.0g: \"12345678\", str8 (got "
            "rc " + d.rc + " " + d.msg + ", `" + str(s, "D", 0) + "', text `" + d.text + "')");
  }
  {
    // usedisplayformat follows the format the variable was CREATED with, not
    // its current type (exprfns_contract_oracle.log:646/736 "b1 was byte now
    // float" -> b1 float %8.0g; :655/737 int -> long keeps %8.0g; :735 a float
    // widened to double keeps %9.0g). Seeded RED: the round-4 code took the
    // default from the current type -- a byte holding a fraction got %9.0g,
    // a long holding one %10.0g, an append-widened float %10.0g.
    piet::SessionState s(subdir(wd, "s10"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate byte b = 1", s);
    (void)piet::dispatch("replace b = 123.4375 in 1", s);
    (void)piet::dispatch("generate long l = 1", s);
    (void)piet::dispatch("replace l = 12345678.5 in 1", s);
    auto b = runLine("tostring b, replace usedisplayformat", s);
    w2check(b.rc == "0" && dtypeOf(s, "b") == "f64" &&
                b.text.find("b cannot be converted reversibly; no replace") != std::string::npos,
            "a byte promoted to float keeps %8.0g: 123.4375 renders \"123.438\" (%9.0g would "
            "give \"123.4375\" and convert), tostring b, usedisplayformat skips with the note "
            "(got rc " + b.rc + " " + b.msg + ", " +
                dtypeOf(s, "b") + ", `" + str(s, "b", 0) + "')");
    auto l = runLine("tostring l, replace usedisplayformat", s);
    w2check(l.rc == "0" && str(s, "l", 0) == "12345678.5" && str(s, "l", 1) == "1" &&
                typeOf(s, "l") == "str10",
            "a long promoted to double keeps %12.0g: tostring l, usedisplayformat stores "
            "\"12345678.5\", str10 (got rc " + l.rc + " " + l.msg + ", `" + str(s, "l", 0) +
                "', text `" + l.text + "')");
    const std::string usingPath = wd + "/s10_using.dta";
    piet::SessionState a(subdir(wd, "s10a"));
    (void)piet::dispatch("clear", a);
    (void)piet::dispatch("set obs 1", a);
    (void)piet::dispatch("generate double x = 0.5", a);
    (void)piet::dispatch("save \"" + usingPath + "\", replace", a);
    (void)piet::dispatch("clear", a);
    (void)piet::dispatch("set obs 1", a);
    (void)piet::dispatch("generate x = 12345678", a);
    auto ap = runLine("append using \"" + usingPath + "\"", a);
    auto fit = a.varMeta.find("x");
    const std::string xfmt = fit != a.varMeta.end() && fit->second.format.has_value()
                                 ? *fit->second.format
                                 : "<none>";
    w2check(ap.rc == "0" && typeOf(a, "x") == "double" && xfmt == "%9.0g",
            "append widening a float master to double records its creation format %9.0g (got "
            "rc " + ap.rc + " " + ap.msg + ", " + typeOf(a, "x") + ", " + xfmt + ")");
    auto x = runLine("tostring x, replace usedisplayformat", a);
    w2check(x.rc == "0" && dtypeOf(a, "x") == "f64" &&
                x.text.find("x cannot be converted reversibly; no replace") != std::string::npos,
            "an append-promoted double renders %9.0g: 12345678 -> \"1.23e+07\", tostring x, "
            "usedisplayformat skips with the note (got rc " + x.rc + " " + x.msg + ", " +
                dtypeOf(a, "x") + ", `" + str(a, "x", 0) + "')");
  }
  {
    // A widening `replace` keeps the display format the variable had
    // (storage::recordReplaceType): exprfns_contract_oracle.log:645-646
    // "variable b1 was byte now float" -> describe :736 `b1 float %8.0g`;
    // :654-655 "variable i1 was int now long" -> :737 `i1 long %8.0g`. Seeded RED: before, the widened
    // type's default leaked out, `: format` %9.0g for both. A long widened to
    // double (w2e_float_default.log:859) and a `bysort: replace` widening
    // (the same recordReplaceType call in egen.cpp) follow the same rule; an
    // explicit `format` is left as it is.
    piet::SessionState s(subdir(wd, "s11"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate byte b1 = 1", s);
    (void)piet::dispatch("generate int i1 = 2", s);
    (void)piet::dispatch("generate long l2 = 1", s);
    (void)piet::dispatch("generate byte g = _n", s);
    (void)piet::dispatch("generate byte bb = 1", s);
    (void)piet::dispatch("generate byte bf = 1", s);
    (void)piet::dispatch("format bf %6.2f", s);
    auto fmtOf = [&](const std::string& v) {
      (void)piet::dispatch("local t : format " + v, s);
      std::string t = runLine("display \"[`t']\"", s).text;
      t.erase(std::remove(t.begin(), t.end(), '\n'), t.end());
      return t;
    };
    auto rb = runLine("replace b1 = 1.5 in 1", s);
    auto ri = runLine("replace i1 = 123456789 in 1", s);
    auto rl = runLine("replace l2 = 12345678.5 in 1", s);
    auto rg = runLine("bysort g: replace bb = 2.5", s);
    auto rf = runLine("replace bf = 1.5 in 1", s);
    const std::string fb = fmtOf("b1"), fi = fmtOf("i1"), fl2 = fmtOf("l2"),
                      fbb = fmtOf("bb"), fbf = fmtOf("bf");
    w2check(rb.rc == "0" && typeOf(s, "b1") == "float" && fb == "[%8.0g]",
            "exprfns_contract_oracle.log:646/736: byte b1 widened to float keeps `: format` "
            "%8.0g (got rc " + rb.rc + " " + rb.msg + ", " + typeOf(s, "b1") + ", " + fb + ")");
    w2check(ri.rc == "0" && typeOf(s, "i1") == "long" && fi == "[%8.0g]",
            "exprfns_contract_oracle.log:655/737: int i1 widened to long keeps `: format` "
            "%8.0g (got rc " + ri.rc + " " + ri.msg + ", " + typeOf(s, "i1") + ", " + fi + ")");
    w2check(rl.rc == "0" && typeOf(s, "l2") == "double" && fl2 == "[%12.0g]",
            "a long widened to double keeps %12.0g (got rc " + rl.rc + " " + rl.msg + ", " +
                typeOf(s, "l2") + ", " + fl2 + ")");
    w2check(rg.rc == "0" && typeOf(s, "bb") == "float" && fbb == "[%8.0g]",
            "bysort g: replace widening a byte to float keeps %8.0g (got rc " + rg.rc + " " +
                rg.msg + ", " + typeOf(s, "bb") + ", " + fbb + ")");
    w2check(rf.rc == "0" && typeOf(s, "bf") == "float" && fbf == "[%6.2f]",
            "an explicit format survives a widening replace (got rc " + rf.rc + " " + rf.msg +
                ", " + typeOf(s, "bf") + ", " + fbf + ")");
    std::string db, di;
    for (const auto& blk : piet::dispatch("describe", s)) {
      if (!blk.is_object() || blk.value("type", std::string()) != "describe" ||
          !blk.contains("variables"))
        continue;
      for (const auto& v : blk.at("variables")) {
        const std::string nm = v.value("name", std::string());
        const std::string row = v.value("dtype", std::string()) + " " +
                                v.value("format", std::string());
        if (nm == "b1") db = row;
        if (nm == "i1") di = row;
      }
    }
    w2check(db == "float %8.0g" && di == "long %8.0g",
            "describe :736 `b1 float %8.0g`, :737 `i1 long %8.0g` (got `" + db + "', `" + di +
                "')");
  }
  {
    // destring corners (StataBE black-box). (a) a float result is never
    // demoted to long: `destring, float` of integers above int's range stays
    // float (before: long). (b) `percent` under `float` stores float(7/100)
    // (before: the double 0.07). (c) `percent` alone takes the ignore()
    // wording of the skip note: "contains characters not specified in
    // ignore()" (before: "contains nonnumeric characters").
    piet::SessionState s(subdir(wd, "d5"));
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 2", s);
    (void)piet::dispatch("generate str6 big = cond(_n == 1, \"40000\", \"7\")", s);
    (void)piet::dispatch("generate str3 p = cond(_n == 1, \"7%\", \"50%\")", s);
    (void)piet::dispatch("generate str3 q = cond(_n == 1, \"x%\", \"5%\")", s);
    auto b = runLine("destring big, replace float", s);
    w2check(b.rc == "0" && near(num(s, "big", 0), 40000) && typeOf(s, "big") == "float",
            "destring, float of an integer above int's range stays float (got rc " + b.rc + " " +
                b.msg + ", " + typeOf(s, "big") + ")");
    auto p = runLine("destring p, replace percent float", s);
    w2check(p.rc == "0" &&
                num(s, "p", 0) == static_cast<double>(static_cast<float>(0.07)) &&
                typeOf(s, "p") == "float",
            "destring, percent float stores float(7/100) (got rc " + p.rc + " " + p.msg + ", " +
                std::to_string(num(s, "p", 0)) + ", " + typeOf(s, "p") + ")");
    auto q = runLine("destring q, replace percent", s);
    w2check(q.rc == "0" && dtypeOf(s, "q") == "factor" &&
                q.text.find("q: contains characters not specified in ignore(); no replace") !=
                    std::string::npos,
            "destring, percent on a nonnumeric value takes the ignore() wording (got rc " + q.rc +
                ", text `" + q.text + "')");
  }

  {
    // `set type double` (StataBE 19.5, measured 2026-09-27): undeclared gen and
    // egen make doubles, tostring id stores "123456789" and x ".1", and the
    // setting survives `clear all`. Before, PIET ignored it and read id as
    // float: "123456792" at rc 0. `set type` is BUG-471 T2's
    // (session.defaultStorageType, kept across `clear all`); this block still
    // restores `set type float` before it ends.
    piet::SessionState s(subdir(wd, "s7"));
    (void)piet::dispatch("clear", s);
    auto st = runLine("set type double", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate id = 123456789", s);
    (void)piet::dispatch("generate x = 0.1", s);
    (void)piet::dispatch("egen e = mean(x)", s);
    (void)piet::dispatch("generate s2 = \"abc\"", s);
    w2check(st.rc == "0" && typeOf(s, "id") == "double" && typeOf(s, "x") == "double" &&
                typeOf(s, "e") == "double" && typeOf(s, "s2") != "double",
            "under `set type double` gen/egen record double, a string gen does not (got rc " +
                st.rc + " " + st.msg + ", " + typeOf(s, "id") + ", " + typeOf(s, "x") + ", " +
                typeOf(s, "e") + ", " + typeOf(s, "s2") + ")");
    auto i = runLine("tostring id, replace", s);
    w2check(i.rc == "0" && str(s, "id", 0) == "123456789" && typeOf(s, "id") == "str9",
            "under `set type double` tostring id stores \"123456789\", str9 (got rc " + i.rc + " " +
                i.msg + ", `" + str(s, "id", 0) + "', " + typeOf(s, "id") + ")");
    auto x = runLine("tostring x, replace", s);
    w2check(x.rc == "0" && str(s, "x", 0) == ".1" && typeOf(s, "x") == "str2",
            "under `set type double` tostring x (0.1) converts: \".1\", str2 (got rc " + x.rc + " " +
                x.msg + ", `" + str(s, "x", 0) + "')");
    // The review's usedisplayformat shape: no recorded display format, so the
    // double's default %10.0g, not %9.0g ("1.23e+07", a skip). (With the
    // review's 123456789 both formats render "1.235e+08" and skip.)
    (void)piet::dispatch("generate id2 = 12345678", s);
    auto u = runLine("tostring id2, replace usedisplayformat", s);
    w2check(u.rc == "0" && str(s, "id2", 0) == "12345678" && typeOf(s, "id2") == "str8",
            "under `set type double` tostring id2, usedisplayformat stores \"12345678\", str8 "
            "(got rc " + u.rc + " " + u.msg + ", `" + str(s, "id2", 0) + "', text `" + u.text +
                "')");
    (void)piet::dispatch("clear all", s);
    (void)piet::dispatch("set obs 1", s);
    (void)piet::dispatch("generate y = 0.1", s);
    w2check(typeOf(s, "y") == "double",
            "`set type double` survives `clear all` (got " + typeOf(s, "y") + ")");
    auto up = runLine("set type DOUBLE", s);
    auto in = runLine("set type int", s);
    auto bare = runLine("set type", s);
    // rc 198 is w2e_float_default.log settype_bad_rc :1314; the message is
    // StataBE's own wording for int (:1309).
    w2check(up.rc == "198" && in.rc == "198" && bare.rc == "198" &&
                in.msg.find("-set type- may not be set to int; float and double are allowed") !=
                    std::string::npos,
            "`set type DOUBLE`, `set type int` and bare `set type` are r(198) (got " + up.rc + ", " +
                in.rc + " " + in.msg + ", " + bare.rc + ")");
    auto fl = runLine("set type float", s);
    (void)piet::dispatch("generate f = 0.1", s);
    auto f = runLine("tostring f, replace", s);
    w2check(fl.rc == "0" && typeOf(s, "f") == "<none>" && dtypeOf(s, "f") == "f64" &&
                f.text.find("f cannot be converted reversibly; no replace") != std::string::npos,
            "after `set type float` gen is float again and tostring skips 0.1 (got rc " + fl.rc +
                ", " + typeOf(s, "f") + ", " + f.rc + ")");
    (void)piet::dispatch("set type float", s);
  }

  fs::remove_all(wd, ec);
}
