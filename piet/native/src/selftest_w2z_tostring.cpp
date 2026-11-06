// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2Z `tostring` -- selftest arms for goal tostring-ado-parity (BUG-916..919).
//
// Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2b_dataio.do:29-40 / .log:76-96 -- preserve,
//     `tostring big, replace format(%20.0f)` ("big was double now str11"),
//     restore, preserve, `tostring big, replace` ("big cannot be converted
//     reversibly; no replace", rc 0), `display big[1]` -> 1.235e+10 (big is the
//     double 12345678902; display's default is %9.0g).
//   w2b_dataio.log:104-113 -- `tostring x, generate(xs) format(%9.2f)` skips,
//     so `display ... xs[1]` is `xs not found`, r(111).
//
// BUG-916: restore (and the `use` stash cache, and the resident `use` of a
// virtual save) re-adopted the preserved frame without retiring the resident
// computed-column cache. The cache is keyed by name and row count only, so a
// column that tostring/destring/generate committed inside the preserved block
// kept answering for the restored variable of the same name: `display big[1]`
// printed the converted text 12345678902 (since a3e8fc55 serves a cached
// string subscript verbatim), and a later tostring read its source values from
// the stale column and could convert where Stata skips.
// BUG-917: the absent one-index subscript was `matrix xs not found` at rc 1.
//
// BUG-918: a missing value under a left-justified format() / string() format
// lost the field's trailing pad. BUG-919: the tostring.ado option surface --
// `ge()` was refused, generate() dropped the variable label, and a format
// `display` cannot use was accepted.
//
// Every arm fails on main ac797486 (bin/main): arms 1, 2, 4 and 4b print
// 12345678902 / hello, arm 3 converts big to "1.23457e+10", arm 5 answers rc 1,
// arm 6 rc 198, arms 7, 7b and 7c an empty label, arm 8 an unpadded `.`, arm 9 rc 0.
//
// BUG-928 (goal strconv-generate-column-order, arms 10-13): generate() puts the
// new variable right after its own source (`move NEW v`, `move v NEW`
// as measured), oracle w2ab_strconv_order (StataBE 19.5 capture,
// w2ab_strconv_order.log:101 so_t1_gen, :152 so_t3_gen_two, :268 so_t7_replace,
// :289 so_t8_range, :390 so_t12_chain, :633 so_p1_roundtrip).
// On bin/main 11df6141 the new column is appended last: arm 10 reads
// `a b c a_s` / `a b c a_s b_s`, arm 11 `a b` for `ds a-b`, arm 12 the same
// appended order on both planes, arm 13 `a b c a_s b_s` after save/use.
#include "selftest_w2.h"

#include <cstdio>
#include <cstdlib>
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

#ifdef _WIN32
// Same shims as selftest.cpp: the Windows CRT has no setenv/unsetenv.
int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

struct LineResult {
  std::string rc = "0";
  std::string msg;
  std::string text;  // concatenated display-block text
  bool allObjects = true;
  size_t errBlocks = 0;
};

LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  LineResult r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) {
      r.allObjects = false;
      continue;
    }
    const std::string type = b.value("type", std::string());
    if (type == "display") r.text += b.value("text", std::string()) + "\n";
    if (type != "error") continue;
    ++r.errBlocks;
    r.rc = piet::eval::coarseRc(b);
    r.msg = b.value("message", std::string());
  }
  return r;
}

std::string displayText(const std::string& expr, piet::SessionState& session) {
  LineResult r = runLine("display " + expr, session);
  if (r.rc != "0") return "<rc " + r.rc + ": " + r.msg + ">";
  if (!r.text.empty() && r.text.back() == '\n') r.text.pop_back();
  return r.text;
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

// The dataset's column order as `ds` posts it in r(varlist) (the oracle's
// order field, w2ab_strconv_order.do `quietly ds` / `r(varlist)`).
std::string dsOrder(piet::SessionState& session, const std::string& args = "") {
  LineResult d = runLine("quietly ds" + args, session);
  if (d.rc != "0") return "<ds rc " + d.rc + ": " + d.msg + ">";
  return displayText("\"`r(varlist)'\"", session);
}

std::string subdir(const std::string& wd, const std::string& name) {
  const std::string d = wd + "/" + name;
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// w2b_dataio.do:6-10, the columns these arms read.
void setupBig(piet::SessionState& s) {
  (void)piet::dispatch("clear", s);
  (void)piet::dispatch("set obs 12", s);
  (void)piet::dispatch("generate x = mod(_n*7,11) + _n/100", s);
  (void)piet::dispatch("generate double big = 12345678901 + _n*_n", s);
}

}  // namespace

void testW2zTostring() {
  std::printf("[selftest] --- W2Z tostring: tostring.ado parity (BUG-916..919) ---\n");
  const std::string wd = (fs::temp_directory_path() / "piet_selftest_w2z_tostring").u8string();
  std::error_code ec;
  fs::remove_all(wd, ec);
  fs::create_directories(wd, ec);

  // (1) The golden's own sequence, w2b_dataio.do:29-40 in one session.
  {
    piet::SessionState s(subdir(wd, "a1"));
    setupBig(s);
    (void)runLine("preserve", s);
    auto c = runLine("tostring big, replace format(%20.0f)", s);
    (void)runLine("restore", s);
    (void)runLine("preserve", s);
    auto r = runLine("tostring big, replace", s);
    const std::string shown = displayText("big[1]", s);
    w2check(c.rc == "0" && r.rc == "0" &&
                r.text.find("big cannot be converted reversibly; no replace") != std::string::npos &&
                dtypeOf(s, "big") == "f64" && shown == "1.235e+10",
            "w2b_dataio tostring_nofmt_ctl after the format() arm's preserve/restore: note at rc 0, "
            "big double, display big[1] 1.235e+10 (log:90-96; got rc " + r.rc + ", dtype " +
                dtypeOf(s, "big") + ", display `" + shown + "')");
    (void)runLine("restore", s);
  }
  // (2) The same restore shape through generate: a string column committed
  // under the preserved block's name must not answer for the restored double.
  {
    piet::SessionState s(subdir(wd, "a2"));
    setupBig(s);
    (void)runLine("preserve", s);
    (void)runLine("drop big", s);
    (void)runLine("generate str5 big = \"hello\"", s);
    (void)runLine("restore", s);
    const std::string shown = displayText("big[1]", s);
    w2check(dtypeOf(s, "big") == "f64" && shown == "1.235e+10",
            "restore after `generate str5 big` under preserve: big[1] is the restored double "
            "1.235e+10 (got dtype " + dtypeOf(s, "big") + ", display `" + shown + "')");
  }
  // (3) tostring's own source read: destring's cached values from inside the
  // preserved block must not stand in for the restored column.
  {
    piet::SessionState s(subdir(wd, "a3"));
    setupBig(s);
    (void)runLine("preserve", s);
    (void)runLine("tostring big, replace force", s);
    (void)runLine("destring big, replace", s);
    (void)runLine("restore", s);
    auto r = runLine("tostring big, replace", s);
    w2check(r.rc == "0" &&
                r.text.find("big cannot be converted reversibly; no replace") != std::string::npos &&
                dtypeOf(s, "big") == "f64" && num(s, "big", 0) == 12345678902.0,
            "tostring after preserve/tostring force/destring/restore reads the restored big: note, "
            "big stays double 12345678902 (got rc " + r.rc + ", dtype " + dtypeOf(s, "big") +
                ", text `" + r.text + "')");
  }
  // (4) The `use` stash cache re-adopts a frame the same way restore does.
  {
    piet::SessionState s(subdir(wd, "a4"));
    setupBig(s);
    const std::string file = wd + "/a4/base.dta";
    (void)runLine("save \"" + file + "\", replace", s);
    (void)runLine("use \"" + file + "\", clear", s);
    (void)runLine("tostring big, replace format(%20.0f)", s);
    (void)runLine("use \"" + file + "\", clear", s);
    const std::string shown = displayText("big[1]", s);
    w2check(dtypeOf(s, "big") == "f64" && shown == "1.235e+10",
            "use of a file after tostring of its big: big[1] is the file's double 1.235e+10 (got "
            "dtype " + dtypeOf(s, "big") + ", display `" + shown + "')");
  }
  // (4b) The resident `use` of a virtual save (tempfile) re-adopts a frame
  // through loadFromVirtual, which has no load() either.
  {
    piet::SessionState s(subdir(wd, "a4b"));
    setupBig(s);
    (void)runLine("tempfile t", s);
    (void)runLine("save `t'", s);
    (void)runLine("tostring big, replace format(%20.0f)", s);
    (void)runLine("use `t', clear", s);
    const std::string shown = displayText("big[1]", s);
    w2check(dtypeOf(s, "big") == "f64" && shown == "1.235e+10",
            "use of a tempfile after tostring of its big: big[1] is the saved double 1.235e+10 "
            "(got dtype " + dtypeOf(s, "big") + ", display `" + shown + "')");
  }
  // (5) w2b_dataio tostring_gen_fmt: the skipped generate() leaves no xs, and
  // `display ... xs[1]` is `xs not found`, r(111), one error block.
  {
    piet::SessionState s(subdir(wd, "a5"));
    setupBig(s);
    auto t = runLine("tostring x, generate(xs) format(%9.2f)", s);
    auto r = runLine("display \"PREC|\" xs[1] \"|\"", s);
    w2check(t.rc == "0" && dtypeOf(s, "xs") == "<absent>" && r.rc == "111" &&
                r.msg == "xs not found" && r.errBlocks == 1 && r.allObjects &&
                r.text == "PREC|xs not found\n",
            "w2b_dataio tostring_gen_fmt: display of the absent xs[1] is `xs not found` r(111) "
            "(log:110-113; got rc " + r.rc + " `" + r.msg + "', shown `" + r.text + "')");
  }
  // (6)-(9): the w2z_tostring fixture's vb (byte 1..5, . in 6, labelled) and
  // vfi (2*_n), w2z_tostring.do:1-12.
  auto setupW2z = [&](piet::SessionState& s) {
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 6", s);
    (void)piet::dispatch("generate byte vb = _n", s);
    (void)piet::dispatch("replace vb = . in 6", s);
    (void)piet::dispatch("label variable vb \"Byte source\"", s);
    (void)piet::dispatch("generate vfi = 2*_n", s);
  };
  // (6) BUG-919 tz_ge_abbrev: `ge()` is the ado's Generate(string) -> rc 0, str2 "12".
  {
    piet::SessionState s(subdir(wd, "a6"));
    setupW2z(s);
    auto t = runLine("tostring vfi, ge(vfi_s)", s);
    const std::string shown = displayText("\"[\" vfi_s[6] \"]\"", s);
    w2check(t.rc == "0" && dtypeOf(s, "vfi_s") == "factor" && shown == "[12]",
            "w2z_tostring tz_ge_abbrev: `tostring vfi, ge(vfi_s)` rc 0, vfi_s[6] [12] (got rc " +
                t.rc + ", dtype " + dtypeOf(s, "vfi_s") + ", display `" + shown + "')");
  }
  // (7) BUG-919 tz_gen_varlabel: generate() copies the variable label (_crcslbl).
  {
    piet::SessionState s(subdir(wd, "a7"));
    setupW2z(s);
    auto t = runLine("tostring vb, gen(vb_l)", s);
    (void)runLine("local lbl : variable label vb_l", s);
    const std::string shown = displayText("\"[`lbl']\"", s);
    w2check(t.rc == "0" && shown == "[Byte source]",
            "w2z_tostring tz_gen_varlabel: gen(vb_l) carries vb's label [Byte source] (got rc " +
                t.rc + ", label `" + shown + "')");
  }
  // (7c) BUG-919 review round: an unlabelled source labels the new variable
  // with the source's NAME (the variable name is the fallback label),
  // in a varlist and under force alike; destring copies the source's label as
  // is ([D] destring, generate()), so destring's unlabelled source stays unlabelled.
  {
    piet::SessionState s(subdir(wd, "a7c"));
    setupW2z(s);
    (void)piet::dispatch("generate double vd = _n/3", s);
    auto t = runLine("tostring vfi vb, gen(vfi_s vb_s)", s);
    auto f = runLine("tostring vd, gen(vd_s) force", s);
    auto d = runLine("destring vfi_s, gen(vfi_n)", s);
    (void)runLine("local l1 : variable label vfi_s", s);
    (void)runLine("local l2 : variable label vb_s", s);
    (void)runLine("local l3 : variable label vd_s", s);
    (void)runLine("local l4 : variable label vfi_n", s);
    const std::string shown = displayText("\"[`l1'][`l2'][`l3'][`l4']\"", s);
    w2check(t.rc == "0" && f.rc == "0" && d.rc == "0" &&
                shown == "[vfi][Byte source][vd][vfi]",
            "tostring gen() of an unlabelled source labels it with the source name "
            "[vfi][Byte source][vd][vfi] (got rc " + t.rc + "/" + f.rc + "/" +
                d.rc + ", labels `" + shown + "')");
  }
  // (7b) BUG-919 sibling: destring's generate() labels NEWVAR with the
  // source's label too ([D] destring: labels are copied to the new variables).
  {
    piet::SessionState s(subdir(wd, "a7b"));
    setupW2z(s);
    (void)runLine("tostring vfi, gen(vfi_s)", s);
    (void)runLine("label variable vfi_s \"String twin\"", s);
    auto t = runLine("destring vfi_s, gen(vfi_n)", s);
    (void)runLine("local lbl : variable label vfi_n", s);
    const std::string shown = displayText("\"[`lbl']\"", s);
    w2check(t.rc == "0" && shown == "[String twin]",
            "destring gen(vfi_n) carries vfi_s's label [String twin] ([D] destring; got rc " +
                t.rc + ", label `" + shown + "')");
  }
  // (8) BUG-918 tz_fmt_left: format(%-9.0g) pads the missing row like any other,
  // and string() of a missing under a left-justified format does the same.
  {
    piet::SessionState s(subdir(wd, "a8"));
    setupW2z(s);
    auto t = runLine("tostring vb, replace format(%-9.0g)", s);
    const std::string shown = displayText("\"[\" vb[1] \"]\" \"[\" vb[6] \"]\"", s);
    const std::string fn = displayText("\"[\" string(.a, \"%-6.2f\") \"][\" string(., \"%9.0g\") \"]\"", s);
    w2check(t.rc == "0" && shown == "[1        ][.        ]" && fn == "[.a    ][.]",
            "w2z_tostring tz_fmt_left: format(%-9.0g) stores [1        ][.        ], string(.a, "
            "\"%-6.2f\") is [.a    ] and string(., \"%9.0g\") [.] (got rc " + t.rc + ", `" + shown +
                "', `" + fn + "')");
  }
  // (9) BUG-919 tz_fmt_invalid: format(%5.2z) is r(198) and vb stays numeric.
  {
    piet::SessionState s(subdir(wd, "a9"));
    setupW2z(s);
    auto t = runLine("tostring vb, replace format(%5.2z)", s);
    w2check(t.rc == "198" && dtypeOf(s, "vb") == "f64",
            "w2z_tostring tz_fmt_invalid: format(%5.2z) r(198), vb unchanged (got rc " + t.rc +
                ", dtype " + dtypeOf(s, "vb") + ")");
  }
  // (10)-(13) BUG-928: the w2ab_strconv_order fixture's numeric a b c
  // (w2ab_strconv_order.do: set obs 3, a = _n, b = 10*_n, c = 100*_n).
  auto setupAbc = [&](piet::SessionState& s) {
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 3", s);
    (void)piet::dispatch("generate a = _n", s);
    (void)piet::dispatch("generate b = 10*_n", s);
    (void)piet::dispatch("generate c = 100*_n", s);
  };
  // (10) so_t1_gen / so_t3_gen_two / so_t7_replace: one and two generate()
  // variables each land right after their source; replace keeps the position.
  {
    piet::SessionState s(subdir(wd, "a10"));
    setupAbc(s);
    auto t1 = runLine("tostring a, gen(a_s)", s);
    const std::string one = dsOrder(s);
    (void)runLine("drop a_s", s);
    auto t3 = runLine("tostring a b, generate(a_s b_s)", s);
    const std::string two = dsOrder(s);
    const std::string vals = displayText("\"[\" a_s[1] \"][\" b_s[3] \"]\"", s);
    piet::SessionState r(subdir(wd, "a10r"));
    setupAbc(r);
    auto t7 = runLine("tostring a, replace", r);
    const std::string rep = dsOrder(r);
    w2check(t1.rc == "0" && t3.rc == "0" && t7.rc == "0" && one == "a a_s b c" &&
                two == "a a_s b b_s c" && vals == "[1][30]" && rep == "a b c" &&
                dtypeOf(r, "a") == "factor",
            "w2ab_strconv_order so_t1_gen/so_t3_gen_two/so_t7_replace: gen(a_s) -> `a a_s b c`, "
            "gen(a_s b_s) -> `a a_s b b_s c` ([1][30]), replace keeps `a b c` ("
            "got `" + one + "' / `" + two + "' " + vals + " / `" + rep + "')");
  }
  // (11) so_t8_range: a varlist range read after generate() holds the new column.
  {
    piet::SessionState s(subdir(wd, "a11"));
    setupAbc(s);
    (void)runLine("tostring a, gen(a_s)", s);
    const std::string range = dsOrder(s, " a-b");
    w2check(range == "a a_s b",
            "w2ab_strconv_order so_t8_range: `ds a-b` after tostring a, gen(a_s) is `a a_s b` "
            "(got `" + range + "')");
  }
  // (12) Both planes answer the same, and the resident computed-column memo
  // still serves the chained consumer after the placement: tostring gen, then
  // destring gen of the new column, then a generate from it (so_t12_chain).
  {
    std::string order[2], vals[2], types[2], labels[2];
    for (int plane = 0; plane < 2; ++plane) {
      if (plane == 1) setenv("PIET_NO_RESIDENT_SETCOL", "1", 1);
      piet::SessionState s(subdir(wd, plane == 0 ? "a12res" : "a12sl"));
      setupAbc(s);
      (void)runLine("label variable a \"Source a\"", s);
      (void)runLine("tostring a b, gen(a_s b_s)", s);
      (void)runLine("destring a_s, gen(a_n)", s);
      (void)runLine("generate a2 = a_s + \"x\"", s);
      if (plane == 1) unsetenv("PIET_NO_RESIDENT_SETCOL");
      order[plane] = dsOrder(s);
      vals[plane] = displayText("\"[\" a_s[3] \"][\" b_s[2] \"][\" a2[1] \"]\" a_n[3]", s);
      (void)runLine("local t1 : type a_s", s);
      (void)runLine("local t2 : type b_s", s);
      (void)runLine("local t3 : type a_n", s);
      types[plane] = displayText("\"`t1' `t2' `t3'\"", s);
      (void)runLine("local l1 : variable label a_s", s);
      (void)runLine("local l2 : variable label a_n", s);
      labels[plane] = displayText("\"[`l1'][`l2']\"", s);
    }
    w2check(order[0] == "a a_s a_n b b_s c a2" && order[0] == order[1] &&
                vals[0] == "[3][20][1x]3" && vals[0] == vals[1] &&
                types[0] == "str1 str2 byte" && types[0] == types[1] &&
                labels[0] == "[Source a][Source a]" && labels[0] == labels[1],
            "BUG-928 plane parity + chain: resident and PIET_NO_RESIDENT_SETCOL=1 both give "
            "`a a_s a_n b b_s c a2`, [3][20][1x]3, str1 str2 byte, [Source a][Source a] (got `" +
                order[0] + "' " + vals[0] + " " + types[0] + " " + labels[0] + " | `" + order[1] +
                "' " + vals[1] + " " + types[1] + " " + labels[1] + ")");
  }
  // (13) so_p1_roundtrip: the placement is in the saved file.
  {
    piet::SessionState s(subdir(wd, "a13"));
    setupAbc(s);
    (void)runLine("tostring a b, gen(a_s b_s)", s);
    (void)runLine("tempfile tf", s);
    auto sv = runLine("save `tf'", s);
    (void)runLine("clear", s);
    auto u = runLine("use `tf'", s);
    const std::string back = dsOrder(s);
    w2check(sv.rc == "0" && u.rc == "0" && back == "a a_s b b_s c",
            "w2ab_strconv_order so_p1_roundtrip: save/use of a tempfile keeps `a a_s b b_s c` "
            "(got rc " + sv.rc + "/" + u.rc + ", `" + back + "')");
  }
}
