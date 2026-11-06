// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// T7 integration selftests (package 007): `clear all` keeps macros, and
// `import excel, firstrow` names a repeated header by its column letter.
//
// Stata's `clear all` drops the
// data, frames, labels, matrices, scalars, constraints, programs, timers and Mata,
// and never runs `macro drop`. StataBE 19.5 agrees: after
//     local L "keep" / global G "gkeep" / scalar s = 3 / clear all
// the global and the local still read back and `display scalar(s)` is r(111)
// (scratch probe ca3.do under scratch-r150/t7-int). Package 007's
// `Wave 1 cleaning.do` opens with `clear all` and then reads `$data`, set one
// level up in its settings file; PIET used to wipe every macro there.
#include "selftest_w2.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

std::string shown(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text += b.value("text", std::string());
  }
  return text;
}

std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  return rc;
}

std::string varNames(piet::SessionState& session) {
  std::string out;
  for (const auto& b : piet::dispatch("describe", session)) {
    if (!b.is_object() || !b.contains("variables")) continue;
    for (const auto& v : b.at("variables")) {
      if (!out.empty()) out += ' ';
      out += v.value("name", std::string());
    }
  }
  return out;
}

// StataBE 19.5, 007's baseline sheet (scratch-r150/t7-int/ie2.do): the second
// `source` header (column L) is the variable `L`, not an error. The same fallback
// names the second of two long headers that truncate alike.
void testExcelRepeatedHeaders(const std::string& workdir) {
  piet::SessionState session(workdir);
  // An absolute path: a relative one lands in the process cwd, not the workdir,
  // and the gate refuses to start on a dirty tree.
  const std::string book = workdir + "/t7dup.xlsx";
  (void)piet::dispatch("putexcel set \"" + book + "\", replace", session);
  (void)piet::dispatch("putexcel A1 = \"src\"", session);
  (void)piet::dispatch("putexcel B1 = \"age\"", session);
  (void)piet::dispatch("putexcel C1 = \"src\"", session);
  (void)piet::dispatch("putexcel A2 = 1", session);
  (void)piet::dispatch("putexcel B2 = 2", session);
  (void)piet::dispatch("putexcel C2 = 3", session);
  std::string rc = "0";
  for (const auto& b : piet::dispatch("import excel using \"" + book + "\", firstrow clear", session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  w2check(rc == "0", "T7/excel: a repeated header is not an error (StataBE ie2: rc 0), got rc " + rc);
  w2check(varNames(session) == "src age C",
          "T7/excel: the second `src` (column C) is named by its column letter. Got {" +
              varNames(session) + "}");
  // The raw header is cut to 32 characters BEFORE cleaning (StataBE sp.do).
  (void)piet::dispatch("putexcel set \"" + book + "\", replace", session);
  (void)piet::dispatch("putexcel A1 = \"Have you heard about this program?\"", session);
  (void)piet::dispatch("putexcel B1 = \"Would you like to provide us your email address\"", session);
  (void)piet::dispatch("putexcel A2 = 1", session);
  (void)piet::dispatch("putexcel B2 = 2", session);
  (void)piet::dispatch("import excel using \"" + book + "\", firstrow clear", session);
  w2check(varNames(session) == "Haveyouheardaboutthisprogra Wouldyouliketoprovideusyou",
          "T7/excel: a long header is cut to 32 raw characters, then cleaned (StataBE sp.do). Got {" +
              varNames(session) + "}");
}

// A STORED zip of the given parts. xl/worksheets/sheet1.xml alone is enough
// for xlsx::read, which falls back to that part when the workbook names none;
// xl/styles.xml adds number formats. Used to put cell kinds in a workbook that
// putexcel cannot write (error cells, date formats).
void writeXlsxParts(const std::string& path, const std::vector<std::pair<std::string, std::string>>& parts) {
  auto crc32 = [](const std::string& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) {
      c ^= b;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
  };
  std::string out, cd;
  auto u16 = [](std::string& o, uint16_t v) { o.push_back(char(v & 0xFF)); o.push_back(char(v >> 8)); };
  auto u32 = [](std::string& o, uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back(char((v >> (8 * k)) & 0xFF)); };
  for (const auto& [name, data] : parts) {
    const uint32_t crc = crc32(data), size = static_cast<uint32_t>(data.size());
    const uint32_t off = static_cast<uint32_t>(out.size());
    u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
    u32(out, crc); u32(out, size); u32(out, size);
    u16(out, static_cast<uint16_t>(name.size())); u16(out, 0); out += name; out += data;
    u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
    u32(cd, crc); u32(cd, size); u32(cd, size);
    u16(cd, static_cast<uint16_t>(name.size())); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
    u32(cd, 0); u32(cd, off); cd += name;
  }
  const uint32_t cdOff = static_cast<uint32_t>(out.size()), cdSize = static_cast<uint32_t>(cd.size());
  out += cd;
  const uint16_t n = static_cast<uint16_t>(parts.size());
  u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, n); u16(out, n);
  u32(out, cdSize); u32(out, cdOff); u16(out, 0);
  std::ofstream(path, std::ios::binary) << out;
}

void writeMinimalXlsx(const std::string& path, const std::string& sheetXml) {
  writeXlsxParts(path, {{"xl/worksheets/sheet1.xml", sheetXml}});
}

// StataBE: an Excel error cell (#N/A) imports as missing, not as its text.
void testExcelErrorCells(const std::string& workdir) {
  piet::SessionState session(workdir);
  const std::string book = workdir + "/t7err.xlsx";
  writeMinimalXlsx(book,
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?><worksheet xmlns=\"http://schemas."
                   "openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
                   "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>id</t></is></c>"
                   "<c r=\"B1\" t=\"inlineStr\"><is><t>v</t></is></c></row>"
                   "<row r=\"2\"><c r=\"A2\"><v>17</v></c><c r=\"B2\"><v>1</v></c></row>"
                   "<row r=\"3\"><c r=\"A3\" t=\"e\"><v>#N/A</v></c><c r=\"B3\"><v>2</v></c></row>"
                   "</sheetData></worksheet>");
  std::string rc = "0";
  for (const auto& b : piet::dispatch("import excel using \"" + book + "\", firstrow clear", session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  w2check(rc == "0", "T7/excel: a workbook with an error cell imports (rc " + rc + ")");
  std::string n;
  for (const auto& b : piet::dispatch("count if missing(id)", session))
    if (b.is_object() && b.value("type", std::string()) == "display") n += b.value("text", std::string());
  w2check(n.find('1') != std::string::npos,
          "T7/excel: the `#N/A` cell is MISSING (StataBE, 007 baseline new_id). count if missing(id) -> `" + n + "`");
  w2check(shown(session, "display id[1]") == "17", "T7/excel: the numeric cell beside it stays a number");
}


// import excel draws a date cell by its number format (StataBE
// scratch-r150/t7-007/stata/dt dt2.golden/dt3.golden, 506/506 lines; AER 007's
// survey dates, which its cleaning sorts and merges on): built-in ids the way
// Excel draws them, a custom code in Stata's own %tc/%td default, serials
// before 61 a day later, the clock truncated from the %tc value.
void testExcelDateCells(const std::string& workdir) {
  piet::SessionState session(workdir);
  const std::string book = workdir + "/t7dates.xlsx";
  const std::string styles =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><styleSheet xmlns=\"http://schemas.openxmlformats.org/"
      "spreadsheetml/2006/main\"><numFmts count=\"1\"><numFmt numFmtId=\"165\" "
      "formatCode=\"d/m/yyyy\\ hh:mm\"/></numFmts><cellXfs count=\"4\"><xf numFmtId=\"0\"/>"
      "<xf numFmtId=\"14\" applyNumberFormat=\"1\"/><xf numFmtId=\"22\" applyNumberFormat=\"1\"/>"
      "<xf numFmtId=\"165\" applyNumberFormat=\"1\"/></cellXfs></styleSheet>";
  auto cellsRow = [](int r, const char* a, const char* b, const char* c) {
    const std::string rs = std::to_string(r);
    return "<row r=\"" + rs + "\"><c r=\"A" + rs + "\" s=\"1\"><v>" + a + "</v></c><c r=\"B" + rs +
           "\" s=\"2\"><v>" + b + "</v></c><c r=\"C" + rs + "\" s=\"3\"><v>" + c + "</v></c></row>";
  };
  const std::string sheet =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/"
      "spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>d14</t></is></c>"
      "<c r=\"B1\" t=\"inlineStr\"><is><t>d22</t></is></c><c r=\"C1\" t=\"inlineStr\"><is><t>dcus</t></is>"
      "</c></row>" +
      cellsRow(2, "43593.24929861111", "43743.55625", "43593.24929861111") +
      cellsRow(3, "0", "44555.999988425923", "0.16666666666666671") + "</sheetData></worksheet>";
  writeXlsxParts(book, {{"xl/worksheets/sheet1.xml", sheet}, {"xl/styles.xml", styles}});
  (void)piet::dispatch("import excel using \"" + book + "\", firstrow allstring clear", session);
  w2check(shown(session, "display \"[\" d14[1] \"][\" d14[2] \"]\"") == "[  5/8/2019][12/31/1899]",
          "T7/exceldate: id 14 is m/d/yyyy in 10; serial 0 is 12/31/1899 (StataBE dt2 f14)");
  w2check(shown(session, "display \"[\" d22[1] \"][\" d22[2] \"]\"") == "[  10/5/2019 1:21][12/25/2021 11:59]",
          "T7/exceldate: id 22 keeps the clock, 12-hour without AM/PM, truncated (StataBE dt2 f22)");
  w2check(shown(session, "display \"[\" dcus[1] \"][\" dcus[2] \"]\"") ==
              "[08may2019 05:58:59][31dec1899 04:00:00]",
          "T7/exceldate: a custom code draws in Stata's %tc default (StataBE dt2 cdmy)");
  (void)piet::dispatch("import excel using \"" + book + "\", firstrow clear", session);
  w2check(shown(session, "display \"`: type d14'|`: format d14'|`: format d22'|`: format dcus'\"") ==
              "double|%tdnn/dd/CCYY|%tcnn/dd/ccYY_hh:MM|%tc",
          "T7/exceldate: numeric date columns are double in the id's format (StataBE dt2 num)");
  w2check(shown(session, "display %21.0g d14[1] \"|\" %21.0g dcus[1]") ==
              "  21677.2492986111065| 1872914339399.999756",
          "T7/exceldate: days -> seconds -> ms, the fraction kept (StataBE dt2 num row 1)");
}


// tex drops every blank before its text (StataBE: 007's Table_A3.tex lines start
// at `&`), and graph save adds .gph only to a name with no dot anywhere
// (StataBE uni-oracle/gs.do: "./gsub/fc" is saved as fc).
void testTexAndGraphSaveNames(const std::string& workdir) {
  piet::SessionState session(workdir);
  namespace fs = std::filesystem;
  const std::string tex = workdir + "/t7tex";
  std::error_code ec;
  fs::remove(tex + ".tex", ec);
  (void)piet::dispatch("texdoc init \"" + tex + "\", replace force", session);
  (void)piet::dispatch("tex \t\t   & (1) & (2) \\\\", session);
  (void)piet::dispatch("texdoc close", session);
  std::ifstream in(tex + ".tex");
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  w2check(body == "& (1) & (2) \\\\\n", "T7/tex: leading blanks after `tex` are dropped (StataBE Table_A3). Got `" + body + "`");
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 5", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("quietly scatter x x", session);
  fs::remove(workdir + "/fc", ec);
  fs::remove(workdir + "/fc.gph", ec);
  (void)piet::dispatch("graph save \"" + workdir + "/./fc\", replace", session);
  w2check(fs::exists(workdir + "/fc") && !fs::exists(workdir + "/fc.gph"),
          "T7/graphsave: a dotted path gets no .gph (StataBE gs.do fc)");
}


// BUG-1091: a %tc column is Stata's milliseconds after `use` and in the file
// `save` writes. StataBE reads tests/eval/golden/tc_datetime_fixture.dta's
// tc_var as 0 ... 500 (uni-oracle/tcf.do); the old reader made it days
// (500/86400000) and the writer multiplied back, so only an outside reader saw
// the 86 400 000x values (AER 007 EndDate2: 1.677e20 for 1941192180000).
void testTcDtaMilliseconds(const std::string& workdir) {
  std::string f = std::string(__FILE__);
  const auto pos = f.rfind("/piet/native/src/selftest_w2_t7_clearall.cpp");
  const std::string fixture =
      (pos == std::string::npos ? std::string("..") : f.substr(0, pos)) + "/tests/eval/golden/tc_datetime_fixture.dta";
  if (!std::filesystem::exists(fixture)) {
    std::printf("[selftest]  SKIP  T7/tcdta: %s absent\n", fixture.c_str());
    return;
  }
  piet::SessionState session(workdir);
  (void)piet::dispatch("use \"" + fixture + "\", clear", session);
  w2check(shown(session, "display %21.0g tc_var[_N]") == "                  500",
          "T7/tcdta: `use` keeps a StataBE %tc column in milliseconds (StataBE tcf.do 500)");
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 1", session);
  (void)piet::dispatch("generate double t = clock(\"7/6/2021 12:03\", \"MDYhm\")", session);
  (void)piet::dispatch("format t %tc", session);
  const std::string out = workdir + "/t7tc.dta";
  (void)piet::dispatch("save \"" + out + "\", replace", session);
  // The double as stored: the last 8 bytes of the single observation's row
  // sit right before </data> in a format-118 file.
  std::ifstream in(out, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const size_t end = bytes.find("</data>");
  double stored = 0;
  if (end != std::string::npos && end >= 8) std::memcpy(&stored, bytes.data() + end - 8, 8);
  w2check(stored == 1941192180000.0,
          "T7/tcdta: `save` writes the %tc milliseconds as they are (StataBE clock 1941192180000)");
}


// winsor2 ..., trim keeps the values AT the cutoffs (StataBE uni-oracle/wt2.do:
// x = 1..5 four times each and one 9, cuts(10 90): only the 9 goes), and a
// packed digit run's last piece may be short (StataBE dr.do/tm.do; AER 007's
// trip times "4:009AM").
void testWinsorTrimAndPackedDigits(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 20", session);
  (void)piet::dispatch("generate x = ceil(_n/4)", session);
  (void)piet::dispatch("replace x = 9 in 20", session);
  (void)piet::dispatch("winsor2 x, cuts(10 90) trim", session);
  (void)piet::dispatch("quietly summarize x_tr", session);
  w2check(shown(session, "display r(min) \"|\" r(max) \"|\" r(N)") == "1|5|19",
          "T7/winsor2: trim keeps the values at the cutoffs (StataBE wt2.do 1|5|19)");
  // Cutoffs are Stata's _pctile at ANY cut, and the result keeps the source's
  // type (StataBE uni-oracle/wq.do Q|x_c, Q|x_w, Q|xb_w); the engine answered 0
  // for a cut outside its nine stored percentiles, so cuts(0 98) zeroed x.
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 10", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("generate byte xb = _n", session);
  (void)piet::dispatch("replace x = 100 in 10", session);
  (void)piet::dispatch("winsor2 x, cuts(10 90)", session);
  (void)piet::dispatch("winsor2 x, cuts(0 98) suffix(_z)", session);
  (void)piet::dispatch("winsor2 xb, cuts(20 80)", session);
  w2check(shown(session, "display x_w[1] \"|\" x_w[10] \"|\" x_z[5] \"|\" x_z[10]") == "1.5|54.5|5|100",
          "T7/winsor2: cutoffs are _pctile at any cut; cuts(0 98) leaves x alone (StataBE wq.do)");
  w2check(shown(session, "display \"`: type xb_w'|\" xb_w[1] \"|\" xb_w[10]") == "byte|2|8",
          "T7/winsor2: the new variable keeps the source's type (StataBE wq.do Q|xb_w)");
  w2check(shown(session, "display %21.0g clock(\"4:009AM\", \"hms\") \"|\" %21.0g clock(\"1:234\", \"hms\")") ==
              "             14409000|              4984000",
          "T7/clock: a packed run's last piece may be short (StataBE tm.do/dr.do)");
  w2check(shown(session, "display date(\"2010115\", \"YMD\") \"|\" date(\"Jan152\", \"MDY\") \"|\" "
                         "clock(\"1:23456\", \"hms\")") == "18571|.|.",
          "T7/date: ...a one-digit year and leftover digits stay missing (StataBE dr.do)");
}


// merge carries the using file's value labels (StataBE uni-oracle/vm2.do:
// V|ml|ul||Yes|M-one, N|ul|both, N2|111), and save keeps an attachment to an
// undefined set (StataBE writes AER 007's Wave1.dta not_in_LF_BL -> notlf).
void testMergeValueLabels(const std::string& workdir) {
  piet::SessionState session(workdir);
  const std::string u = workdir + "/t7vmu.dta", d = workdir + "/t7ul.dta";
  for (const char* c : {"clear", "set obs 2", "generate id = _n", "generate u = _n - 1", "generate s = _n - 1",
                        "label define ul 0 \"No\" 1 \"Yes\"", "label define both 0 \"U-zero\" 1 \"U-one\"",
                        "label values u ul", "label values s both"})
    (void)piet::dispatch(c, session);
  (void)piet::dispatch("save \"" + u + "\", replace", session);
  for (const char* c : {"clear", "set obs 2", "generate id = 2*_n - 1", "generate m = mod(_n,2)",
                        "generate s = 1 - mod(_n,2)", "label define ml 0 \"Zero\" 1 \"One\"",
                        "label define both 0 \"M-zero\" 1 \"M-one\"", "label values m ml"})
    (void)piet::dispatch(c, session);
  (void)piet::dispatch("merge 1:1 id using \"" + u + "\", nogen", session);
  (void)piet::dispatch("generate t = 1", session);
  (void)piet::dispatch("label values t both", session);
  w2check(shown(session, "display \"`: value label m'|`: value label u'|`: value label s'|`: label (u) 1'|`: label (t) 1'\"") ==
              "ml|ul||Yes|M-one",
          "T7/mergelbl: brought-in variables keep their value labels; the master's sets win (StataBE vm2.do V)");
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 1", session);
  (void)piet::dispatch("generate id = 1", session);
  (void)piet::dispatch("merge 1:1 id using \"" + u + "\", nogen nolabel", session);
  w2check(shown(session, "display \"`: value label u'|`: value label s'\"") == "ul|both" &&
              rcOf(session, "label list ul") == "111",
          "T7/mergelbl: nolabel attaches but copies no definition (StataBE vm2.do N, N2)");
  (void)piet::dispatch("label values u notdefined", session);
  (void)piet::dispatch("save \"" + d + "\", replace", session);
  (void)piet::dispatch("use \"" + d + "\", clear", session);
  w2check(shown(session, "display \"`: value label u'\"") == "notdefined",
          "T7/savelbl: an attachment to an undefined set survives save (StataBE Wave1.dta notlf)");
}


// A standalone `a#b` cell holding a factor's base level is the one omitted
// when it duplicates a main effect, and an esttab label names an interaction
// factor-first (StataBE uni-oracle/bc3.do; AER 007's TA17 and joint test).
void testBaseCellOmitAndLabelOrder(const std::string& workdir) {
  piet::SessionState session(workdir);
  for (const char* c : {"clear", "set obs 200", "generate y = mod(_n*7,11) + 0.1*_n + mod(_n*_n,13)/5",
                        "generate t = mod(_n*13,7) < 3", "generate h = mod(_n*5,11) < 5",
                        "generate e = mod(_n*3,13) < 6", "generate x = mod(_n*3,7)", "generate g = mod(_n,5)",
                        "label variable t \"Treat\"", "label variable x \"Age\""})
    (void)piet::dispatch(c, session);
  (void)piet::dispatch("quietly reghdfe y t##i.h t#i.e, absorb(g)", session);
  w2check(shown(session, "display %12.0g _b[1.t] \"|\" %12.0g _b[1.t#1.e] \"|\" %12.0g _b[1.t#0.e]") ==
              "  .055273259|-.0588964813|           0",
          "T7/basecell: reghdfe omits 1.t#0.e, the base-level cell, not 1.t#1.e (StataBE bc3.do)");
  (void)piet::dispatch("quietly regress y c.x##i.t", session);
  (void)piet::dispatch("eststo m1", session);
  const std::string out = workdir + "/t7bc3.txt";
  (void)piet::dispatch("esttab m1 using \"" + out + "\", label keep(1.t#c.x) replace plain nonumbers nomtitles noobs",
                       session);
  std::ifstream in(out);
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  w2check(body.find("Treat=1 # Age") != std::string::npos,
          "T7/esttab: an interaction label is factor-first, `Treat=1 # Age` (StataBE bc3.do). Got `" + body + "`");
}


// r(sd) is the correctly rounded sd (StataBE uni-oracle/sm.do SM1/SM2), and
// esttab formats a cell's 16-digit macro text (StataBE et2.do: estadd's
// 0.715/5.2 prints 0.138; AER 007's Table_A9_Panel_A).
void testExactSdAndEstoutMacroRounding(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 997", session);
  (void)piet::dispatch("generate double x = mod(_n*7919, 1009)/7 + 0.1*_n", session);
  (void)piet::dispatch("quietly summarize x", session);
  (void)piet::dispatch("generate double z = (x - r(mean))/r(sd)", session);
  w2check(shown(session, "display %21x r(mean) \"|\" %21x r(sd)") ==
              "+1.e82b979e83ebdX+006|+1.943b553de0432X+005",
          "T7/sd: r(sd) is the correctly rounded value (StataBE sm.do SM1)");
  (void)piet::dispatch("quietly summarize z", session);
  w2check(shown(session, "display %21x r(mean)") == "+1.a04f15d3526c6X-035",
          "T7/sd: ...so a standardized mean carries Stata's last bits (StataBE sm.do SM2)");
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 30", session);
  (void)piet::dispatch("generate y = mod(_n*7,11)", session);
  (void)piet::dispatch("generate x = mod(_n*3,5)", session);
  (void)piet::dispatch("quietly regress y x", session);
  (void)piet::dispatch("eststo m1", session);
  (void)piet::dispatch("estadd scalar bc = 0.715/5.2", session);
  const std::string out = workdir + "/t7et2.txt";
  (void)piet::dispatch("esttab m1 using \"" + out + "\", scalars(\"bc BC\") b(%9.3f) replace plain noobs nonumbers nomtitles",
                       session);
  std::ifstream in(out);
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  w2check(body.find("BC                  0.138") != std::string::npos,
          "T7/esttab: a scalar is formatted from its macro text, 0.715/5.2 -> 0.138 (StataBE et2.do). Got `" + body + "`");
}


// StataBE (bq.do): a plain string that ends in a backtick is two characters, not
// the opening of a compound quote. 007's Wave 1 cleaning: replace hh_les18="1" if hh_les18== "1`"
void testBacktickInPlainString(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 3", session);
  (void)piet::dispatch("generate str4 h = \"1`\" in 1", session);
  (void)piet::dispatch("replace h = \"2\" in 2", session);
  (void)piet::dispatch("replace h = \"1`\" in 3", session);
  w2check(rcOf(session, "replace h=\"1\" if h== \"1`\"") == "0",
          "T7/backtick: `replace ... if h == \"1`\"` runs (StataBE bq.do rc 0)");
  std::string n;
  for (const auto& b : piet::dispatch("count if h == \"1\"", session))
    if (b.is_object() && b.value("type", std::string()) == "display") n += b.value("text", std::string());
  w2check(n.find('2') != std::string::npos, "T7/backtick: both `1`` cells were replaced (StataBE: 2). Got `" + n + "`");
}


// winsor2.ado: `trim` without suffix() names the new variable <var>_tr.
void testWinsor2TrimSuffix(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 20", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("winsor2 x, cuts(0 95) trim", session);
  w2check(varNames(session) == "x x_tr", "T7/winsor2: `trim` names the new variable x_tr (winsor2.ado). Got {" +
                                             varNames(session) + "}");
  (void)piet::dispatch("winsor2 x, cuts(5 95)", session);
  w2check(varNames(session) == "x x_tr x_w", "T7/winsor2: without trim the suffix stays _w. Got {" +
                                                 varNames(session) + "}");
}


// mvencode: `if` limits the recode AND the override check to the sample
// (StataBE mv.do, scratch-r150/uni-oracle).
void testMvencodeIf(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 6", session);
  (void)piet::dispatch("generate c = mod(_n,2)", session);
  (void)piet::dispatch("generate a = cond(_n<=4, ., _n)", session);
  (void)piet::dispatch("generate b = cond(_n==1, 0, .)", session);
  w2check(rcOf(session, "mvencode a b if c == 1, mv(0) override") == "0",
          "T7/mvencode: `if` is accepted (StataBE mv.do rc 0)");
  w2check(shown(session, "display a[1] \"|\" a[2] \"|\" a[3] \"|\" a[4]") == "0|.|0|.",
          "T7/mvencode: only the `if` rows are recoded (StataBE A|0|.|0|.)");
  w2check(rcOf(session, "mvencode b if c == 0, mv(0)") == "0",
          "T7/mvencode: the override check looks only inside the `if` sample (StataBE rc 0)");
}


// A string cell holding `"` read through var[1] (StataBE sq2.do; 007's Wave 3
// labels every variable from row 1 of a Qualtrics export). The pre-fix build
// refused it as "invalid syntax".
void testQuotedStringSubscript(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 2", session);
  (void)piet::dispatch("generate str60 s = \"For \" + char(34) + \"A uni\" + char(34) + \" is it 'ok'?\"", session);
  (void)piet::dispatch("generate str5 t = \"plain\"", session);
  w2check(shown(session, "display strlen(s[1])") == "23", "T7/strsub: strlen(s[1]) of a quote-bearing cell (StataBE 23)");
  w2check(shown(session, "display strpos(s[1], \"uni\")") == "8", "T7/strsub: strpos(s[1], \"uni\") (StataBE 8)");
  (void)piet::dispatch("local value = s[1]", session);
  (void)piet::dispatch("local vname = strtoname(`\"`value'\"')", session);
  (void)piet::dispatch("label var t `\"`vname'\"'", session);
  w2check(shown(session, "display `\"`: variable label t'\"'") == "For__A_uni__is_it__ok__",
          "T7/strsub: 007's label loop on a quote-bearing cell (StataBE For__A_uni__is_it__ok__)");
}


// Variable abbreviation in keep/drop (StataBE ab.do, ab2.do; 007's Wave 3 keeps
// `number_people_clicked` meaning number_people_clicked_w3).
void testKeepDropAbbreviation(const std::string& workdir) {
  piet::SessionState session(workdir);
  auto fresh = [&] {
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("generate number_people_clicked_w3 = 1", session);
    (void)piet::dispatch("generate number_friends_clicked_w3 = 2", session);
    (void)piet::dispatch("generate other = 3", session);
  };
  fresh();
  w2check(rcOf(session, "keep number_people_clicked number_friends_clicked other") == "0",
          "T7/abbrev: keep resolves unique abbreviations (StataBE rc 0)");
  w2check(varNames(session) == "number_people_clicked_w3 number_friends_clicked_w3 other",
          "T7/abbrev: ...and keeps the full-named variables. Got {" + varNames(session) + "}");
  fresh();
  w2check(rcOf(session, "drop numb") == "111", "T7/abbrev: an ambiguous abbreviation is r(111) (StataBE)");
  w2check(rcOf(session, "drop number_f") == "0" && varNames(session) == "number_people_clicked_w3 other",
          "T7/abbrev: drop resolves a unique abbreviation (StataBE). Got {" + varNames(session) + "}");
  // rename's old name (StataBE rn.do).
  fresh();
  w2check(rcOf(session, "rename number_people_clicked ppl") == "0" &&
              varNames(session) == "ppl number_friends_clicked_w3 other",
          "T7/abbrev: rename resolves an abbreviated old name (StataBE rn.do). Got {" + varNames(session) + "}");
  w2check(rcOf(session, "rename numb q") == "0" && varNames(session) == "ppl q other",
          "T7/abbrev: ...and `numb` is then unique (StataBE rn.do rc 0). Got {" + varNames(session) + "}");
}


// estadd scalar of a division by zero stores missing (StataBE es.do: rc 0, e(bc) = .).
void testEstaddMissing(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 10", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("generate y = 2*_n + mod(_n,3)", session);
  (void)piet::dispatch("quietly regress y x", session);
  w2check(rcOf(session, "estadd scalar bc = 0.098/0.000") == "0",
          "T7/estadd: a scalar that divides by zero is stored, rc 0 (StataBE es.do)");
  w2check(shown(session, "display e(bc)") == ".", "T7/estadd: ...as missing (StataBE e(bc) = .). Got `" +
                                                      shown(session, "display e(bc)") + "`");
  w2check(rcOf(session, "estadd scalar bad = nosuchthing + 1") != "0",
          "T7/estadd: an expression that fails to evaluate is still refused");
}


// graph export .jpg writes a baseline JFIF of the requested size (Stata writes
// jpg; 007's figures use it). The pixels were checked against the png raster by
// decoding with macOS sips (mean |diff| 0.27/255, scratch-r150/jpg/cmp.py).
void testGraphExportJpg(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 20", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("generate y = mod(_n*7, 11)", session);
  (void)piet::dispatch("twoway scatter y x", session);
  const std::string path = workdir + "/t7.jpg";
  w2check(rcOf(session, "graph export \"" + path + "\", replace width(320)") == "0",
          "T7/jpg: graph export .jpg is accepted");
  std::ifstream f(path, std::ios::binary);
  const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t sof = b.find("\xFF\xC0");
  const bool header = b.size() > 20 && b.compare(0, 2, "\xFF\xD8") == 0 && b.compare(6, 4, "JFIF") == 0;
  const bool tail = b.size() > 2 && b.compare(b.size() - 2, 2, "\xFF\xD9") == 0;
  int w = 0;
  if (sof != std::string::npos && sof + 9 < b.size())
    w = (static_cast<unsigned char>(b[sof + 7]) << 8) | static_cast<unsigned char>(b[sof + 8]);
  w2check(header && tail && w == 320,
          "T7/jpg: the file is a baseline JFIF (SOI, JFIF, SOF0 width 320, EOI); width read " + std::to_string(w));
}


// r(table) with an omitted regressor (StataBE scratch-r150/rtab/r2.do; 007 FA3
// `o.miss_age_PAP`): a regressor constant in the sample is Stata's `o.c` column,
// b 0, se/p missing, df and crit filled. A collinear PAIR stays refused: which of
// the two Stata omits depends on order (r1.do: reghdfe keeps x, omits z).
void testRTableOmitted(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 60", session);
  (void)piet::dispatch("generate id = ceil(_n/6)", session);
  (void)piet::dispatch("generate g = mod(_n,5)", session);
  (void)piet::dispatch("generate x = mod(_n*7,13)", session);
  (void)piet::dispatch("generate c = 0", session);
  (void)piet::dispatch("generate y = 1 + 0.5*x + mod(_n*3,7)", session);
  for (const char* fit : {"reghdfe y x c, absorb(g) vce(cluster id)", "regress y x c"}) {
    (void)piet::dispatch(std::string("quietly ") + fit, session);
    (void)piet::dispatch("matrix T = r(table)", session);
    const std::string tag = std::string("T7/rtable [") + fit + "]: ";
    w2check(shown(session, "display \"`: colnames T'\"") == "x o.c _cons",
            tag + "colnames x o.c _cons (StataBE r2.do). Got `" + shown(session, "display \"`: colnames T'\"") + "`");
    w2check(shown(session, "display T[1,2] \"|\" T[2,2] \"|\" T[4,2]") == "0|.|.",
            tag + "the o.c column is b 0, se and p missing (StataBE)");
  }
  (void)piet::dispatch("quietly reghdfe y x c, absorb(g) vce(cluster id)", session);
  (void)piet::dispatch("matrix T = r(table)", session);
  w2check(shown(session, "display T[7,2] \"|\" %9.6f T[8,2]") == "9| 2.262157",
          "T7/rtable: df and crit are filled on the omitted column (StataBE 9 | 2.262157)");
  // A collinear pair: whichever member the engine omits, r(table) names it as
  // e(b) does (`o.`, se missing). WHICH member is BUG-1067 (reghdfe omits z,
  // selftest_w2_c1067.cpp); regress omits x (StataBE r1.do `o.x z _cons`).
  (void)piet::dispatch("generate z = 2*x", session);
  (void)piet::dispatch("quietly regress y x z", session);
  (void)piet::dispatch("matrix T2 = r(table)", session);
  (void)piet::dispatch("matrix E2 = e(b)", session);
  w2check(shown(session, "display \"`: colnames T2'\"") == "o.x z _cons" &&
              shown(session, "display \"`: colnames E2'\"") == "o.x z _cons",
          "T7/rtable: regress on a collinear pair: r(table) and e(b) both `o.x z _cons` (StataBE r1.do)");
  w2check(shown(session, "display T2[1,1] \"|\" T2[2,1]") == "0|.",
          "T7/rtable: ...the o.x column is b 0, se missing (StataBE)");
}


// reghdfe ..., nocons with absorbed effects (StataBE scratch-r150/rtab/nc.do):
// the fit is unchanged and only `_cons` leaves e(b) and r(table).
void testReghdfeNocons(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 60", session);
  (void)piet::dispatch("generate id = ceil(_n/6)", session);
  (void)piet::dispatch("generate g = mod(_n,5)", session);
  (void)piet::dispatch("generate x = mod(_n*7,13)", session);
  (void)piet::dispatch("generate y = 1 + 0.5*x + mod(_n*3,7)", session);
  w2check(rcOf(session, "quietly reghdfe y x, absorb(g) vce(cluster id) nocons") == "0",
          "T7/nocons: reghdfe ..., absorb() nocons runs (StataBE nc.do)");
  w2check(shown(session, "display %12.8f _b[x] \"|\" %12.8f _se[x]") == "  0.52736886|  0.09208737",
          "T7/nocons: b and se equal the fit with a constant (StataBE nc.do)");
  (void)piet::dispatch("matrix E = e(b)", session);
  (void)piet::dispatch("matrix T = r(table)", session);
  w2check(shown(session, "display \"`: colnames E'|`: colnames T'\"") == "x|x",
          "T7/nocons: e(b) and r(table) carry no _cons (StataBE `x`)");
}


// Abbreviations in every plain varlist (StataBE scratch-r150/uni-oracle/ab3.do;
// 007's Stats_for_paper.do `tab edu_category` -> edu_category_BL).
void testPlainVarlistAbbreviation(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 4", session);
  (void)piet::dispatch("generate edu_category_BL = mod(_n,3)", session);
  (void)piet::dispatch("generate age_BL = _n", session);
  (void)piet::dispatch("generate agex = 2", session);
  w2check(rcOf(session, "quietly tab edu_category") == "0" && shown(session, "display r(N) \"|\" r(r)") == "4|3",
          "T7/abbrev: tab resolves an abbreviation (StataBE ab3.do R1 0|4|3)");
  w2check(rcOf(session, "quietly summarize age_B") == "0" && shown(session, "display r(N) \"|\" r(mean)") == "4|2.5",
          "T7/abbrev: summarize resolves an abbreviation (StataBE R2 0|4|2.5)");
  w2check(rcOf(session, "summarize ag") == "111", "T7/abbrev: summarize of an ambiguous abbreviation is r(111) (StataBE R3)");
  w2check(rcOf(session, "quietly tab edu_c*") == "0" && shown(session, "display r(r)") == "3",
          "T7/abbrev: tab expands a wildcard (StataBE R4 0|3)");
  w2check(rcOf(session, "list edu_cat in 1, noobs") == "0", "T7/abbrev: list resolves an abbreviation (StataBE R5)");
  // replace's target (StataBE rp.do: R|0|0, A|111).
  (void)piet::dispatch("replace edu_category_BL = . in 2", session);
  w2check(rcOf(session, "replace edu_category = 0 if edu_category==.") == "0" &&
              shown(session, "display edu_category_BL[2]") == "0",
          "T7/abbrev: replace resolves an abbreviated target (StataBE rp.do R|0|0)");
  (void)piet::dispatch("generate edu_x = 1", session);
  w2check(rcOf(session, "replace edu = 5") == "111",
          "T7/abbrev: replace with an ambiguous target is r(111) (StataBE rp.do A|111)");
}


// Abbreviations in an estimation varlist (StataBE scratch-r150/uni-oracle/fa.do;
// 007's Alt_main_results_alternate_controls.do `$controls_alt ...
// miss_unemployed` -> miss_unemployed_BL).
void testFitVarlistAbbreviation(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 40", session);
  (void)piet::dispatch("generate y = mod(_n*7,11) + 0.1*_n", session);
  (void)piet::dispatch("generate miss_unemployed_BL = mod(_n,3)==0", session);
  (void)piet::dispatch("generate edu_category_BL = mod(_n,4)", session);
  (void)piet::dispatch("generate g = mod(_n,5)", session);
  (void)piet::dispatch("generate t = _n", session);
  (void)piet::dispatch("tsset t", session);
  w2check(rcOf(session, "quietly regress y miss_unemployed") == "0" &&
              rcOf(session, "display _b[miss_unemployed_BL]") == "0",
          "T7/fitabbrev: regress resolves an abbreviated regressor (StataBE fa.do F1)");
  (void)piet::dispatch("quietly regress y i.edu_category", session);
  (void)piet::dispatch("matrix E = e(b)", session);
  w2check(shown(session, "display \"`: colnames E'\"") ==
              "0b.edu_category_BL 1.edu_category_BL 2.edu_category_BL 3.edu_category_BL _cons",
          "T7/fitabbrev: i.<abbrev> fits the full name (StataBE fa.do F2)");
  w2check(rcOf(session, "quietly regress y c.miss_unemployed#i.edu_cat") == "0",
          "T7/fitabbrev: each # atom resolves its name (StataBE fa.do F3 rc 0)");
  (void)piet::dispatch("quietly regress y L.miss_unemployed", session);
  (void)piet::dispatch("matrix E = e(b)", session);
  w2check(shown(session, "display \"`: colnames E'\"") == "L.miss_unemployed_BL _cons",
          "T7/fitabbrev: L.<abbrev> keeps its operator (StataBE fa.do F4)");
  w2check(rcOf(session, "quietly reghdfe y miss_unemployed i.edu_category, absorb(g) vce(cluster g)") == "0" &&
              rcOf(session, "display _b[miss_unemployed_BL]") == "0",
          "T7/fitabbrev: reghdfe resolves abbreviations (StataBE fa.do F5)");
  (void)piet::dispatch("quietly regress miss_unemp y", session);
  w2check(shown(session, "display e(depvar)") == "miss_unemployed_BL",
          "T7/fitabbrev: the depvar resolves too (StataBE fa.do F6)");
  (void)piet::dispatch("generate edu_x = 1", session);
  w2check(rcOf(session, "regress y edu") == "111" && rcOf(session, "regress y i.edu") == "111",
          "T7/fitabbrev: an ambiguous abbreviation is r(111), plain or after i. (StataBE fa.do F7/F8)");
}


// Abbreviated variables inside coefficient names resolve against the model's
// variables (StataBE scratch-r150/uni-oracle/hy.do, hz.do; 007's HTE tables:
// `test 1.treatment + 1.treatment#1.husb_influence_kids = 0` after a fit on
// husb_influence_kids_original_BL).
void testCoefNameAbbreviation(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 80", session);
  (void)piet::dispatch("generate y = mod(_n*7,11) + 0.1*_n", session);
  (void)piet::dispatch("generate treatment = mod(_n,2)", session);
  (void)piet::dispatch("generate husb_influence_kids_original_BL = mod(floor(_n/3),2)", session);
  (void)piet::dispatch("generate g = mod(_n,5)", session);
  (void)piet::dispatch("generate husb_x = 1", session);
  (void)piet::dispatch("quietly reghdfe y treatment##i.husb_influence_kids_original_BL, absorb(g) vce(cluster g)",
                       session);
  w2check(rcOf(session, "quietly test 1.treatment + 1.treatment#1.husb_influence_kids = 0") == "0" &&
              shown(session, "display %12.0g r(F) \"|\" %12.0g r(p)") == " 1.68993e-06| .9990250193",
          "T7/coefabbrev: test resolves an abbreviated interaction (StataBE hy.do Y1)");
  w2check(rcOf(session, "quietly lincom 1.treatment + 1.treatment#1.husb") == "0" &&
              shown(session, "display %12.0g r(estimate) \"|\" %12.0g r(se)") == " .0017413912| 1.339557702",
          "T7/coefabbrev: lincom resolves against the model, not the data (StataBE hy.do Y3/Y6)");
  w2check(shown(session, "display %12.0g _b[1.treatment#1.husb_influence_kids]") == "  .311490367",
          "T7/coefabbrev: _b[] resolves an abbreviated interaction (StataBE hy.do Y4)");
  (void)piet::dispatch("generate weight_a = mod(_n*3,7)", session);
  (void)piet::dispatch("generate weight_b = mod(_n*5,9)", session);
  (void)piet::dispatch("quietly reghdfe y treatment weight_a weight_b, absorb(g) vce(cluster g)", session);
  w2check(rcOf(session, "display _b[weight]") == "111" && rcOf(session, "lincom weight") == "111",
          "T7/coefabbrev: two model candidates is r(111) for _b[] and lincom (StataBE hz.do Z5/Z6)");
  w2check(rcOf(session, "lincom nosuch") == "111",
          "T7/coefabbrev: lincom of a name not in the model is r(111) (StataBE ln.do L1)");
}


// esttab ... using "a path with blanks.tex" writes THAT file (007's robustness
// tables: "Working and ability to spend_..._`c(current_date)'.tex").
void testEsttabQuotedUsing(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 20", session);
  (void)piet::dispatch("generate x = _n", session);
  (void)piet::dispatch("generate y = 2*_n + mod(_n,3)", session);
  (void)piet::dispatch("quietly regress y x", session);
  (void)piet::dispatch("eststo m1", session);
  const std::string path = workdir + "/Working and ability 28 Oct.tex";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  w2check(rcOf(session, "esttab m1 using \"" + path + "\", replace se") == "0",
          "T7/esttab: using a quoted path with blanks is rc 0");
  w2check(std::filesystem::exists(path), "T7/esttab: ...and writes the file under its full name");
}

}  // namespace

void testW2T7ClearAll() {
  std::printf("[selftest] --- T7 clear all keeps global and local macros ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_t7clearall").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("local L \"keep\"", session);
    (void)piet::dispatch("global G \"gkeep\"", session);
    (void)piet::dispatch("scalar s = 3", session);
    (void)piet::dispatch("clear all", session);
    w2check(shown(session, "display \"[$G]\"") == "[gkeep]",
            "T7/clearall: a GLOBAL survives `clear all` (clear.ado never drops macros; StataBE ca3)");
    w2check(shown(session, "display \"[`L']\"") == "[keep]",
            "T7/clearall: a LOCAL survives `clear all` (StataBE ca3)");
    w2check(rcOf(session, "display scalar(s)") != "0",
            "T7/clearall: a scalar does NOT survive `clear all` (StataBE ca3: r(111))");
  }
  testExcelRepeatedHeaders(workdir);
  testExcelErrorCells(workdir);
  testExcelDateCells(workdir);
  testTexAndGraphSaveNames(workdir);
  testTcDtaMilliseconds(workdir);
  testWinsorTrimAndPackedDigits(workdir);
  testMergeValueLabels(workdir);
  testBaseCellOmitAndLabelOrder(workdir);
  testExactSdAndEstoutMacroRounding(workdir);
  testBacktickInPlainString(workdir);
  testWinsor2TrimSuffix(workdir);
  testMvencodeIf(workdir);
  testQuotedStringSubscript(workdir);
  testKeepDropAbbreviation(workdir);
  testEstaddMissing(workdir);
  testGraphExportJpg(workdir);
  testRTableOmitted(workdir);
  testReghdfeNocons(workdir);
  testPlainVarlistAbbreviation(workdir);
  testFitVarlistAbbreviation(workdir);
  testCoefNameAbbreviation(workdir);
  testEsttabQuotedUsing(workdir);
  fs::remove_all(workdir, ec);
}
