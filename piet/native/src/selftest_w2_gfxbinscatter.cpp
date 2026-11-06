// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `gfxbinscatter` (goal gfx-binscatter, 2026-10-20) -- binscatter
// (Stepner 7.02) as StataBE 19.5 runs it: the sample (if/in, aw/fw), several y,
// by() with pooled bins, controls()/absorb() residualization, the bin points,
// the fit lines in e(y#_coefs), e(byvalues), e(graphcmd), savedata's f.csv and
// f.do, r(602) without replace, reportreg's table, the rc-0 refusals that post
// nothing, and the previous e() cleared.
// Oracle: tests/eval/golden/aer_w2/w2k_binscatter.log (case aer_w2:w2k_binscatter,
// StataNow 19.5 with binscatter 7.02); each check cites its log lines. Every
// check fails on main f849cf7e, where binscatter takes two variables and no
// `if`, refuses savedata/by/controls/absorb/linetype/reportreg/medians/discrete
// with r(198), leaves the previous estimation in e() and `matrix M = e(absent)`
// is r(111).
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gBsChecks = 0;
void bsCheck(bool cond, const std::string& label) {
  ++gBsChecks;
  w2check(cond, label);
}

// w2k_binscatter.do:143-158, the oracle's closed-form fixture (120 rows, ties
// in x on purpose).
const char* kFixture =
    "clear\n"
    "set obs 120\n"
    "generate int id = _n\n"
    "generate byte g = mod(_n-1, 4) + 1\n"
    "generate byte grp = mod(_n, 2)\n"
    "generate double x = floor(mod(_n*37, 101)/4)/2 + grp\n"
    "generate double z = mod(_n*13, 17)/3\n"
    "generate double y = 1 + 0.5*x + 0.3*z + g/2 + mod(_n*7, 11)/5 - grp*x/4\n"
    "generate double y2 = 2 - 0.25*x + z/2 + mod(_n*5, 7)/3\n"
    "generate byte xd = mod(_n*7, 6)\n"
    "generate byte w = 1 + mod(_n, 3)\n"
    "generate int t = _n\n"
    "tsset t\n";

struct Run {
  std::vector<std::string> displays;
  std::vector<std::string> errors;  // the message of every error block
  bool arrayBlock = false;
};

Run run(piet::SessionState& session, const std::string& src) {
  Run r;
  try {
    auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object()) {
        r.arrayBlock = true;  // defect class 1: never an array pushed as a block
        continue;
      }
      if (b.value("type", std::string()) == "display") r.displays.push_back(b.value("text", std::string()));
      if (b.value("type", std::string()) == "error") r.errors.push_back(b.value("message", std::string()));
    }
  } catch (const std::exception& e) {
    r.displays.push_back(std::string("EXC:") + e.what());
  }
  return r;
}

std::string lineStarting(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines)
    if (l.compare(0, prefix.size(), prefix) == 0) return l;
  return "<no " + prefix + " line>";
}

bool hasLine(const std::vector<std::string>& lines, const std::string& text) {
  for (const auto& l : lines)
    if (l == text) return true;
  return false;
}

std::vector<std::string> fileLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  for (std::string l; std::getline(f, l);) out.push_back(l);
  return out;
}

// Stata's %21x text -> double (missing -> NaN).
double fromStataHex(const std::string& s) {
  const std::size_t xpos = s.find('X');
  if (s.size() < 5 || xpos == std::string::npos) return std::nan("");
  const long e = std::strtol(s.substr(xpos + 1).c_str(), nullptr, 16);
  if (e == 0x3ff) return std::nan("");
  const std::string mant = s.substr(1, xpos - 1);
  const double m = std::strtod(("0x" + mant + "p0").c_str(), nullptr);
  if (m == 0.0) return 0.0;
  return (s[0] == '-' ? -1.0 : 1.0) * std::ldexp(m, static_cast<int>(e));
}

// Every %21x field of `line` (TAG|a|b|...) within 1e-9 of the oracle's.
bool hexFieldsNear(const std::string& line, const std::vector<const char*>& oracle) {
  std::vector<std::string> f;
  std::stringstream ss(line);
  for (std::string p; std::getline(ss, p, '|');) f.push_back(p);
  if (f.size() < oracle.size() + 1) return false;
  for (std::size_t i = 0; i < oracle.size(); ++i) {
    const double got = fromStataHex(f[i + 1]);
    const double want = fromStataHex(oracle[i]);
    if (std::isnan(want) != std::isnan(got)) return false;
    if (!std::isnan(want) && std::fabs(got - want) > 1e-9 * std::fabs(want)) return false;
  }
  return true;
}

std::string joinLines(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& l : v) s += l + "\n";
  return s;
}

}  // namespace

void testW2GfxBinscatter() {
  std::printf(
      "[selftest] --- W2 gfxbinscatter: binscatter 7.02 samples, bins, fits, files, e() "
      "(w2k_binscatter.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2gfxbs").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto session = [&]() {
    auto s = std::make_unique<piet::SessionState>(workdir);
    s->cwd = workdir;  // savedata()'s relative names land here, never in the process cwd
    return s;
  };

  // (1) BS_ctl0 (log 225-260): nquantiles(5) savedata() -- e(N) 120, e(cmd)
  // empty, e(y1_coefs) `x _cons` with row `=`, the CSV at %10.0g, the .do and
  // its graph command at the macro layer's 16 digits, the two file notes.
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "capture erase bs_ctl0.csv\n"
                              "capture erase bs_ctl0.do\n"
                              "binscatter y x, nquantiles(5) savedata(bs_ctl0) nodraw\n"
                              "display \"N|\" e(N) \"|`e(cmd)'|\"\n"
                              "matrix M = e(y1_coefs)\n"
                              "local rn : rowfullnames M\n"
                              "local cn : colnames M\n"
                              "display \"NM|\" rowsof(M) \"|\" colsof(M) \"|`rn'|`cn'|\"\n"
                              "display \"C1|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n");
    bsCheck(!R.arrayBlock, "W2 gfxbinscatter (1) every block is an object (no array pushed as a block)");
    bsCheck(lineStarting(R.displays, "N|") == "N|120||",
            "W2 gfxbinscatter (1) BS_ctl0 (log 238): e(N) 120, e(cmd) empty -- got `" +
                lineStarting(R.displays, "N|") + "`");
    bsCheck(lineStarting(R.displays, "NM|") == "NM|1|2|=|x _cons|",
            "W2 gfxbinscatter (1) BS_ctl0_c1_names (log 248): 1 x 2, row `=`, columns `x _cons` -- got `" +
                lineStarting(R.displays, "NM|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "C1|"), {"+1.67ce762c0391dX-002", "+1.0b9f0cb5e3dd6X+002"}),
            "W2 gfxbinscatter (1) BS_ctl0_c1_r1 (log 249): slope/constant -- got `" +
                lineStarting(R.displays, "C1|") + "`");
    const auto csv = fileLines(workdir + "/bs_ctl0.csv");
    bsCheck(joinLines(csv) == "x,y\n1.5208333,4.1125\n4.2857143,6.1258929\n7,7.190625\n9.5208333,7.446875\n"
                              "11.85,7.8925\n",
            "W2 gfxbinscatter (1) BS_ctl0_csv_L1-L6 (log 252-258): the CSV text -- got\n" + joinLines(csv));
    const auto dof = fileLines(workdir + "/bs_ctl0.do");
    bsCheck(dof.size() == 3 && dof[0] == "insheet using bs_ctl0.csv" && dof[1].empty() &&
                dof[2] == "twoway (scatter y x, mcolor(navy) lcolor(maroon)) (function "
                          "0*x^2+.3513735260356883*x+4.181582620265052, range(1.520833333333333 11.85) "
                          "lcolor(maroon)), graphregion(fcolor(white))  xtitle(x) ytitle(y) legend(off order()) nodraw",
            "W2 gfxbinscatter (1) BS_ctl0_do_L1-L3 (log 261-263): the .do text -- got\n" + joinLines(dof));
    bsCheck(hasLine(R.displays, "(file bs_ctl0.csv written containing saved data)") &&
                hasLine(R.displays, "(file bs_ctl0.do written containing commands to process saved data)") &&
                !hasLine(R.displays, "(file bs_ctl0.csv not found)"),
            "W2 gfxbinscatter (1) log 226-227: the two file notes, no `not found` note without replace");
    const std::string* gc = s->eval.eStrings.find("graphcmd");
    bsCheck(gc != nullptr &&
                *gc == "twoway (scatteri 4.1125 1.520833333333333 6.125892857142857 4.285714285714286 7.190625 7 "
                       "7.446874999999999 9.520833333333334 7.8925 11.85, mcolor(navy) lcolor(maroon)) (function "
                       "0*x^2+.3513735260356883*x+4.181582620265052, range(1.520833333333333 11.85) lcolor(maroon)), "
                       "graphregion(fcolor(white))  xtitle(x) ytitle(y) legend(off order()) nodraw",
            "W2 gfxbinscatter (1) BS_gc_one_g1-g2 (log 1571-1573): e(graphcmd) text, 345 characters -- got `" +
                (gc != nullptr ? *gc : std::string("<none>")) + "`");

    // (2) BS_sd_noreplace (log 1195-1217): the same savedata() without
    // replace is r(602) before anything runs; the CSV is left as it was.
    const Run R2 = run(*s, "capture noisily binscatter y x, nquantiles(10) savedata(bs_ctl0) nodraw\n"
                           "display \"RC|\" _rc \"|\"\n");
    bsCheck(lineStarting(R2.displays, "RC|") == "RC|602|",
            "W2 gfxbinscatter (2) BS_sd_noreplace (log 1207): r(602) -- got `" + lineStarting(R2.displays, "RC|") + "`");
    bsCheck(fileLines(workdir + "/bs_ctl0.csv").size() == 6,
            "W2 gfxbinscatter (2) BS_sd_noreplace_csv_nlines (log 1217): the 5-bin CSV is untouched");
  }

  // (3) BS_by (log 596-648): by(grp) -- pooled bins, one fit row per group with
  // equation by#, e(byvalues), the _by# CSV header and the .do's label lines.
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "binscatter y x, nquantiles(5) by(grp) savedata(bs_by) replace nodraw\n"
                              "matrix M = e(y1_coefs)\n"
                              "local rn : rowfullnames M\n"
                              "display \"BYN|`rn'|\"\n"
                              "local re : roweq M\n"
                              "local ce : coleq M\n"
                              "display \"BYE|`re'|`ce'|\"\n"
                              "display \"B1|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "display \"B2|\" %21x M[2,1] \"|\" %21x M[2,2] \"|\"\n"
                              "matrix V = e(byvalues)\n"
                              "local vr : rownames V\n"
                              "local vc : colnames V\n"
                              "display \"BV|`vr'|`vc'|\" V[1,1] \"|\" V[2,1] \"|\"\n");
    bsCheck(lineStarting(R.displays, "BYN|") == "BYN|by1:= by2:=|",
            "W2 gfxbinscatter (3) BS_by_c1_names (log 621): rows `=` in equations by1 by2 -- got `" +
                lineStarting(R.displays, "BYN|") + "`");
    bsCheck(lineStarting(R.displays, "BYE|") == "BYE|by1 by2|_ _|",
            "W2 gfxbinscatter (3) BS_by_c1_names (log 621): `: roweq` is by1 by2, `: coleq` `_ _` -- got `" +
                lineStarting(R.displays, "BYE|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "B1|"), {"+1.e45b308d08c60X-002", "+1.1ce5a288f7c6cX+002"}) &&
                hexFieldsNear(lineStarting(R.displays, "B2|"), {"+1.1975e25788d9bX-002", "+1.d6110a3bebf0fX+001"}),
            "W2 gfxbinscatter (3) BS_by_c1_r1/r2 (log 622-623): per-group fits -- got `" +
                lineStarting(R.displays, "B1|") + "` `" + lineStarting(R.displays, "B2|") + "`");
    bsCheck(lineStarting(R.displays, "BV|") == "BV|by1 by2|grp|0|1|",
            "W2 gfxbinscatter (3) BS_by_bv (log 626-628): e(byvalues) 0 \\ 1, rows by1 by2, column grp -- got `" +
                lineStarting(R.displays, "BV|") + "`");
    const auto csv = fileLines(workdir + "/bs_by.csv");
    bsCheck(csv.size() == 6 && csv[0] == "x_by1,x_by2,y_by1,y_by2" && csv[1] == "1.3461538,1.7272727,4.75,3.3590909",
            "W2 gfxbinscatter (3) BS_by_csv_L1-L2 (log 631-632): by columns -- got\n" + joinLines(csv));
    const auto dof = fileLines(workdir + "/bs_by.do");
    bsCheck(dof.size() == 8 && dof[2] == "label variable x_by1 \"x; grp==0\"" &&
                dof[5] == "label variable y_by2 \"y; grp==1\"" && dof[6].empty(),
            "W2 gfxbinscatter (3) BS_by_do (log 642-648): label lines then a blank line -- got\n" + joinLines(dof));
    bsCheck(hasLine(R.displays, "(file bs_by.csv not found)"),
            "W2 gfxbinscatter (3) log 597: `replace` with no file prints `(file bs_by.csv not found)`");
  }

  // (4) The ado's three rc-0 refusals post nothing (log 973-980 BS_lt_bad,
  // 1084-1091 BS_reportreg_none, 1101-1108 BS_disc_nq); pweights are r(101)
  // (log 2514 BS_pw); discrete over N/2 values is r(134) (log 2562 BS_disc_134).
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "quietly regress y z\n"
                              "capture noisily binscatter y x, nquantiles(5) linetype(foo) nodraw\n"
                              "display \"LT|\" _rc \"|`e(cmd)'|\"\n"
                              "capture noisily binscatter y x, nquantiles(5) reportreg line(none) nodraw\n"
                              "display \"RR|\" _rc \"|`e(cmd)'|\"\n"
                              "capture noisily binscatter y xd, nquantiles(5) discrete nodraw\n"
                              "display \"DQ|\" _rc \"|`e(cmd)'|\"\n"
                              "capture noisily binscatter y x [pw=w], nquantiles(5) nodraw\n"
                              "display \"PW|\" _rc \"|\"\n"
                              "capture noisily binscatter x y, discrete nodraw\n"
                              "display \"D134|\" _rc \"|\"\n");
    bsCheck(lineStarting(R.displays, "LT|") == "LT|0|regress|" &&
                hasLine(R.displays, "linetype() must either be connect, lfit, qfit, or none"),
            "W2 gfxbinscatter (4) BS_lt_bad (log 974, 980): message, rc 0, e() kept -- got `" +
                lineStarting(R.displays, "LT|") + "`");
    bsCheck(lineStarting(R.displays, "RR|") == "RR|0|regress|",
            "W2 gfxbinscatter (4) BS_reportreg_none (log 1085, 1091) -- got `" + lineStarting(R.displays, "RR|") + "`");
    bsCheck(lineStarting(R.displays, "DQ|") == "DQ|0|regress|",
            "W2 gfxbinscatter (4) BS_disc_nq (log 1102, 1108) -- got `" + lineStarting(R.displays, "DQ|") + "`");
    bsCheck(lineStarting(R.displays, "PW|") == "PW|101|",
            "W2 gfxbinscatter (4) BS_pw (log 2514): pweights not allowed, r(101) -- got `" + lineStarting(R.displays, "PW|") + "`");
    bsCheck(lineStarting(R.displays, "D134|") == "D134|134|",
            "W2 gfxbinscatter (4) BS_disc_134 (log 2562): discrete over N/2 values, r(134) -- got `" +
                lineStarting(R.displays, "D134|") + "`");
  }

  // (5) BS_state (log 1361-1369): binscatter replaces e() -- `_b[z]` of the
  // regress before it is r(111) -- and line(none) posts no e(y1_coefs), so
  // `matrix M = e(y1_coefs)` is Stata's 1 x 1 missing r1/c1 (log 863-864).
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "quietly regress y z\n"
                              "binscatter y x, nquantiles(5) line(none) nodraw\n"
                              "capture local bz = _b[z]\n"
                              "display \"ST|\" _rc \"|\" e(N) \"|`e(cmd)'|\"\n"
                              "capture matrix M = e(y1_coefs)\n"
                              "display \"MR|\" _rc \"|\" rowsof(M) \"|\" colsof(M) \"|\" M[1,1] \"|\"\n"
                              "local rn : rownames M\n"
                              "local cn : colnames M\n"
                              "display \"MN|`rn'|`cn'|\"\n");
    bsCheck(lineStarting(R.displays, "ST|") == "ST|111|120||",
            "W2 gfxbinscatter (5) BS_state (log 1369): `_b[z]` r(111), e(N) 120, e(cmd) empty -- got `" +
                lineStarting(R.displays, "ST|") + "`");
    bsCheck(lineStarting(R.displays, "MR|") == "MR|0|1|1|.|" && lineStarting(R.displays, "MN|") == "MN|r1|c1|",
            "W2 gfxbinscatter (5) BS_line_none_c1 (log 863-864): 1 x 1 missing, r1/c1, rc 0 -- got `" +
                lineStarting(R.displays, "MR|") + "` `" + lineStarting(R.displays, "MN|") + "`");
  }

  // (6) Residualization, weights, several y, the sample (log 427 BS_controls,
  // 505 BS_controls_fv, 544 BS_absorb, 1346 BS_aw, 1632 BS_gc_multiy c2,
  // 1292 BS_multiy_if e(N) 90, 2528 BS_in e(N) 60).
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "binscatter y x, nquantiles(5) controls(z) nodraw\n"
                              "matrix M = e(y1_coefs)\n"
                              "display \"CT|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "binscatter y x, nquantiles(5) controls(i.g z) nodraw\n"
                              "matrix M = e(y1_coefs)\n"
                              "display \"FV|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "binscatter y x, nquantiles(5) absorb(g) nodraw\n"
                              "matrix M = e(y1_coefs)\n"
                              "display \"AB|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "binscatter y x [aw=w], nquantiles(5) nodraw\n"
                              "matrix M = e(y1_coefs)\n"
                              "display \"AW|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "binscatter y y2 x, nquantiles(5) nodraw\n"
                              "matrix M = e(y2_coefs)\n"
                              "display \"Y2|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n"
                              "binscatter y y2 x if g <= 3, nquantiles(5) line(qfit) nodraw\n"
                              "display \"IF|\" e(N) \"|\"\n"
                              "binscatter y x in 1/60, nquantiles(5) nodraw\n"
                              "display \"IN|\" e(N) \"|\"\n");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "CT|"), {"+1.608530279b341X-002", "+1.0e9e882aef299X+002"}),
            "W2 gfxbinscatter (6) BS_controls_c1_r1 (log 427): residualized on z, mean added back -- got `" +
                lineStarting(R.displays, "CT|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "FV|"), {"+1.7820511e99a76X-002", "+1.04e8095d7cd00X+002"}),
            "W2 gfxbinscatter (6) BS_controls_fv_c1_r1 (log 505): factor-variable controls -- got `" +
                lineStarting(R.displays, "FV|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "AB|"), {"+1.7f81e82ce71eeX-002", "+1.01de8c9279454X+002"}),
            "W2 gfxbinscatter (6) BS_absorb_c1_r1 (log 544): areg residuals -- got `" +
                lineStarting(R.displays, "AB|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "AW|"), {"+1.66160d9d4b57bX-002", "+1.0affee87c3f86X+002"}),
            "W2 gfxbinscatter (6) BS_aw_c1_r1 (log 1346): [aw=w] -- got `" + lineStarting(R.displays, "AW|") + "`");
    bsCheck(hexFieldsNear(lineStarting(R.displays, "Y2|"), {"-1.cf01aed7cba43X-003", "+1.0c0dd01a875caX+002"}),
            "W2 gfxbinscatter (6) BS_gc_multiy_c2_r1 (log 1632): the second y -- got `" +
                lineStarting(R.displays, "Y2|") + "`");
    bsCheck(lineStarting(R.displays, "IF|") == "IF|90|" && lineStarting(R.displays, "IN|") == "IN|60|",
            "W2 gfxbinscatter (6) BS_multiy_if (log 1292) / BS_in (log 2528): the if and in samples -- got `" +
                lineStarting(R.displays, "IF|") + "` `" + lineStarting(R.displays, "IN|") + "`");
  }

  // (7) BS_reportreg (log 991-1014): reportreg prints regress's table, its
  // full-width rule first.
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) + "binscatter y x, nquantiles(5) reportreg nodraw\n");
    bsCheck(hasLine(R.displays, std::string(255, '-')) &&
                hasLine(R.displays, "      Source |       SS           df       MS      Number of obs   =       120") &&
                hasLine(R.displays, "       Model |  197.191555         1  197.191555   Prob > F        =    0.0000") &&
                hasLine(R.displays, "    Residual |  247.985815       118   2.1015747   R-squared       =    0.4430") &&
                hasLine(R.displays, "       Total |   445.17737       119   3.7409863   Root MSE        =    1.4497") &&
                hasLine(R.displays, "           x |   .3513735   .0362742     9.69   0.000     .2795408    .4232062") &&
                hasLine(R.displays, "       _cons |   4.181583   .2730218    15.32   0.000     3.640925     4.72224"),
            "W2 gfxbinscatter (7) BS_reportreg (log 992-1013): the regress table -- got\n" + joinLines(R.displays));
    const Run R2 = run(*s, "binscatter y x, reportreg nq(10) controls(z) nodraw\n");
    bsCheck(hasLine(R2.displays, "    __000003 | Coefficient  Std. err.      t    P>|t|     [95% conf. interval]") &&
                hasLine(R2.displays, "    __000002 |   .3442581   .0349514     9.85   0.000     .2750447    .4134714"),
            "W2 gfxbinscatter (7) BS_reportreg_ctl (log 1049-1051): the residualized tempvar names -- got\n" +
                joinLines(R2.displays));
  }

  // (8) The 077 pipeline (log 1401-1503): if, by(), factor controls, line(none),
  // savedata() replace, then `clear`, `do f.do`, `save` -- and genxq() (log
  // 1377-1391 BS_genxq: 24 rows in bin 1, 20 in bin 5, bin 1 ends at 2.5, bin
  // 2 starts at 3: fastxtile's ties-go-low rule).
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "binscatter y x, nquantiles(5) genxq(bsq) nodraw\n"
                              "quietly count if bsq == 1\n"
                              "local n1 = r(N)\n"
                              "quietly count if bsq == 5\n"
                              "local n5 = r(N)\n"
                              "quietly summarize x if bsq == 1\n"
                              "local mx1 = r(max)\n"
                              "quietly summarize x if bsq == 2\n"
                              "display \"GQ|`n1'|`n5'|`mx1'|\" r(min) \"|\"\n"
                              "binscatter y x if id <= 100, by(grp) controls(i.g) n(5) line(none) "
                              "savedata(bs_077) replace nodraw\n"
                              "display \"P77|\" e(N) \"|\"\n"
                              "clear\n"
                              "do bs_077\n"
                              "local l1 : variable label x_by1\n"
                              "display \"D77|\" _N \"|\" c(k) \"|`l1'|\"\n"
                              "display \"O77|\" %21x x_by1[1] \"|\" %21x y_by1[1] \"|\"\n");
    bsCheck(lineStarting(R.displays, "GQ|") == "GQ|24|20|2.5|3|",
            "W2 gfxbinscatter (8) BS_genxq (log 1391) -- got `" + lineStarting(R.displays, "GQ|") + "`");
    bsCheck(lineStarting(R.displays, "P77|") == "P77|100|" &&
                lineStarting(R.displays, "D77|") == "D77|5|4|x; grp==0|",
            "W2 gfxbinscatter (8) BS_077 / BS_077_do / BS_077_labels (log 1416, 1472, 1494): the saved .do "
            "rebuilds the 5 x 4 data -- got `" +
                lineStarting(R.displays, "P77|") + "` `" + lineStarting(R.displays, "D77|") + "`");
    bsCheck(lineStarting(R.displays, "O77|") == "O77|+1.9ef9da0000000X+000|+1.f5b3d00000000X+001|",
            "W2 gfxbinscatter (8) BS_077_data_o1 (log 1477): insheet reads the .csv's non-integer columns as "
            "float -- got `" + lineStarting(R.displays, "O77|") + "`");
  }

  // (9) Review round 3 (no oracle arm captures these; the rules come from the ado
  // and from help insheet). (a) The 1x1-missing answer for an absent e() name
  // is binscatter's own e() only: after `regress, vce(robust)` PIET cannot
  // tell e(V_modelbased) (a matrix Stata posts) from an absent name, so it
  // refuses with r(111) or answers the 2x2, never a 1x1 missing at rc 0.
  // (b) Under `set type double` the controls() residuals are doubles (the
  // ado's predict/gen take c(type), identified from binscatter's ado): the slope is the
  // regression of the double residuals, not of their float roundings
  // (BS_controls_c1_r1, log 427, is the float one). (c) insheet keeps doubles
  // under `set type double`; `nodouble` asks for floats (help insheet).
  {
    auto s = session();
    {
      std::ofstream f(workdir + "/bs_t9.csv");
      f << "x,y\n1.6209999,3\n4.0116666,4\n";
    }
    const Run R = run(*s, std::string(kFixture) +
                              "quietly regress y x, vce(robust)\n"
                              "capture matrix VMB = e(V_modelbased)\n"
                              "local vrc = _rc\n"
                              "local vr = .\n"
                              "if `vrc' == 0 local vr = rowsof(VMB)\n"
                              "display \"VMB|`vrc'|`vr'|\"\n"
                              "set type double\n"
                              "quietly regress y z\n"
                              "predict double ry, residuals\n"
                              "summarize y, meanonly\n"
                              "replace ry = ry + r(mean)\n"
                              "quietly regress x z\n"
                              "predict double rx, residuals\n"
                              "summarize x, meanonly\n"
                              "replace rx = rx + r(mean)\n"
                              "quietly regress ry rx\n"
                              "scalar bd = _b[rx]\n"
                              "binscatter y x, controls(z) nquantiles(10) nodraw\n"
                              "matrix E = e(y1_coefs)\n"
                              "display \"DBL|\" (abs(E[1,1] - scalar(bd)) <= 1e-13*abs(scalar(bd))) \"|\"\n"
                              "insheet using bs_t9.csv, clear\n"
                              "display \"INS|\" %21x x[1] \"|\"\n"
                              "insheet using bs_t9.csv, clear nodouble\n"
                              "display \"INSF|\" %21x x[1] \"|\"\n"
                              "set type float\n");
    const std::string vmb = lineStarting(R.displays, "VMB|");
    bsCheck(vmb == "VMB|111|.|" || vmb == "VMB|0|2|",
            "W2 gfxbinscatter (9a) e(V_modelbased) after regress, vce(robust) is a refusal or the 2x2, never a "
            "1x1 missing -- got `" + vmb + "`");
    bsCheck(lineStarting(R.displays, "DBL|") == "DBL|1|",
            "W2 gfxbinscatter (9b) set type double: controls() residuals stay double -- got `" +
                lineStarting(R.displays, "DBL|") + "`\n" + joinLines(R.displays));
    bsCheck(lineStarting(R.displays, "INS|") == "INS|+1.9ef9d97551bbbX+000|" &&
                lineStarting(R.displays, "INSF|") == "INSF|+1.9ef9da0000000X+000|",
            "W2 gfxbinscatter (9c) set type double: insheet keeps doubles, nodouble stores floats -- got `" +
                lineStarting(R.displays, "INS|") + "` `" + lineStarting(R.displays, "INSF|") + "`");
  }

  // (10) An engine error keeps what binscatter printed before it (no oracle
  // arm captures it; same binary). `discrete controls(z)` prints the warning
  // of binscatter's ado and then exits r(134); main printed the
  // error alone.
  {
    auto s = session();
    const Run R = run(*s, std::string(kFixture) +
                              "capture noisily binscatter y xd, discrete controls(z) nodraw\n"
                              "display \"DC|\" _rc \"|\"\n");
    const std::string warn =
        "warning: discrete is specified in combination with controls() or absorb(). note that binning takes "
        "places after residualization, so the residualized x-variable may contain many more unique values.";
    std::size_t iw = R.displays.size(), irc = R.displays.size();
    for (std::size_t i = 0; i < R.displays.size(); ++i) {
      if (R.displays[i] == warn && iw == R.displays.size()) iw = i;
      if (R.displays[i] == "DC|134|") irc = i;
    }
    bsCheck(iw < irc && irc < R.displays.size() && !R.arrayBlock,
            "W2 gfxbinscatter (10) discrete controls(z): the ado's warning, then r(134) -- got\n" +
                joinLines(R.displays));
  }

  // (11) FU-1088 (no oracle arm; same binary): by(varname), absorb(varname)
  // and CONTROLs(varlist numeric ts fv) (binscatter's syntax line, identified from its ado) resolve their
  // names as Stata's `syntax` does, through the by/bysort key rule
  // (handlers::resolveByKeyNames): a unique prefix stands for its variable, a
  // wildcard expands, an exact name wins over a longer one, several prefix
  // matches are r(111). A factor term keeps its operator. Main answered
  // by(gr) r(111) and controls(z*) rc 1.
  {
    auto s = session();
    auto gcNow = [&]() {
      const std::string* gc = s->eval.eStrings.find("graphcmd");
      return gc != nullptr ? *gc : std::string("<none>");
    };
    const char* pinCoefs =
        "matrix M = e(y1_coefs)\n"
        "local rn : rowfullnames M\n"
        "local cn : colnames M\n"
        "display \"CO|`rn'|`cn'|\" %21x M[1,1] \"|\" %21x M[1,2] \"|\"\n";
    const Run A = run(*s, std::string(kFixture) + "generate byte gnum = g\n" +
                              "binscatter y x, nquantiles(5) by(grp) nodraw\n"
                              "matrix V = e(byvalues)\n"
                              "local vc : colnames V\n"
                              "display \"BVA|`vc'|\" V[1,1] \"|\" V[2,1] \"|\"\n");
    const std::string gcFull = gcNow();
    const Run B = run(*s, "capture noisily binscatter y x, nquantiles(5) by(gr) nodraw\n"
                          "display \"RCB|\" _rc \"|\"\n"
                          "matrix V = e(byvalues)\n"
                          "local vc : colnames V\n"
                          "display \"BVA|`vc'|\" V[1,1] \"|\" V[2,1] \"|\"\n");
    bsCheck(lineStarting(B.displays, "RCB|") == "RCB|0|" &&
                lineStarting(B.displays, "BVA|") == lineStarting(A.displays, "BVA|") &&
                lineStarting(A.displays, "BVA|") == "BVA|grp|0|1|" && gcNow() == gcFull && !B.arrayBlock,
            "W2 gfxbinscatter (11a) by(gr) is by(grp): e(byvalues) and e(graphcmd) equal -- got `" +
                lineStarting(B.displays, "RCB|") + "` `" + lineStarting(B.displays, "BVA|") + "`\n" +
                joinLines(B.errors));

    const Run C1 = run(*s, std::string("binscatter y x, nquantiles(5) controls(z) nodraw\n") + pinCoefs);
    const std::string gcZ = gcNow();
    const Run C2 = run(*s, std::string("capture noisily binscatter y x, nquantiles(5) controls(z*) nodraw\n"
                                       "display \"RCC|\" _rc \"|\"\n") +
                               pinCoefs);
    bsCheck(lineStarting(C2.displays, "RCC|") == "RCC|0|" &&
                lineStarting(C2.displays, "CO|") == lineStarting(C1.displays, "CO|") &&
                lineStarting(C1.displays, "CO|").find("<no") == std::string::npos && gcNow() == gcZ,
            "W2 gfxbinscatter (11b) controls(z*) is controls(z) (z is the only z* variable) -- got `" +
                lineStarting(C2.displays, "RCC|") + "` `" + lineStarting(C2.displays, "CO|") + "` vs `" +
                lineStarting(C1.displays, "CO|") + "`\n" + joinLines(C2.errors));

    const Run F1 = run(*s, std::string("binscatter y x, nquantiles(5) controls(i.g z) nodraw\n") + pinCoefs);
    const Run F2 = run(*s, std::string("capture noisily binscatter y x, nquantiles(5) controls(i.gn c.z) nodraw\n"
                                       "display \"RCF|\" _rc \"|\"\n") +
                               pinCoefs);
    bsCheck(lineStarting(F1.displays, "CO|").find("<no") == std::string::npos &&
                lineStarting(F1.displays, "CO|") != lineStarting(C1.displays, "CO|") &&
                lineStarting(F2.displays, "RCF|") == "RCF|0|" &&
                lineStarting(F2.displays, "CO|") == lineStarting(F1.displays, "CO|"),
            "W2 gfxbinscatter (11c) controls(i.g z) still serves; i.gn resolves to i.gnum with its operator kept -- "
            "got `" + lineStarting(F1.displays, "CO|") + "` `" + lineStarting(F2.displays, "RCF|") + "` `" +
                lineStarting(F2.displays, "CO|") + "`\n" + joinLines(F2.errors));

    const Run E = run(*s, "capture noisily binscatter y x, nquantiles(5) by(g) nodraw\n"
                          "display \"RCE|\" _rc \"|\"\n"
                          "matrix V = e(byvalues)\n"
                          "local vc : colnames V\n"
                          "display \"BVE|`vc'|\" rowsof(V) \"|\"\n");
    bsCheck(lineStarting(E.displays, "RCE|") == "RCE|0|" && lineStarting(E.displays, "BVE|") == "BVE|g|4|",
            "W2 gfxbinscatter (11d) by(g) with g, gnum and grp present is g (an exact name wins) -- got `" +
                lineStarting(E.displays, "RCE|") + "` `" + lineStarting(E.displays, "BVE|") + "`");

    const Run G = run(*s, "generate double zz1 = z\n"
                          "generate double zz2 = z\n"
                          "capture noisily binscatter y x, nquantiles(5) by(gr) controls(zz) nodraw\n"
                          "display \"RCG|\" _rc \"|\"\n"
                          "capture noisily binscatter y x, nquantiles(5) absorb(zz) nodraw\n"
                          "display \"RCH|\" _rc \"|\"\n");
    bool ambMsg = false;
    for (const auto& e : G.errors)
      if (e == "zz ambiguous abbreviation") ambMsg = true;
    bsCheck(lineStarting(G.displays, "RCG|") == "RCG|111|" && lineStarting(G.displays, "RCH|") == "RCH|111|" &&
                ambMsg,
            "W2 gfxbinscatter (11e) controls(zz)/absorb(zz) with zz1 and zz2: r(111) `zz ambiguous abbreviation` -- "
            "got `" + lineStarting(G.displays, "RCG|") + "` `" + lineStarting(G.displays, "RCH|") + "`\n" +
                joinLines(G.errors));

    // (11f) A parenthesised group with a space is one term: `c.(z zsq)` is
    // `c.z c.zsq` and `i.(gn)` is `i.gnum` (fvexpand). The first split the
    // group at its space and refused `zsq)` with r(111).
    const Run P1 = run(*s, std::string("generate double zsq = z * z\n"
                                       "binscatter y x, nquantiles(5) controls(i.g z zsq) nodraw\n") +
                               pinCoefs);
    const Run P2 = run(*s, std::string("capture noisily binscatter y x, nquantiles(5) controls(i.(gn) c.(z zsq)) "
                                       "nodraw\n"
                                       "display \"RCP|\" _rc \"|\"\n") +
                               pinCoefs);
    bsCheck(lineStarting(P1.displays, "CO|").find("<no") == std::string::npos &&
                lineStarting(P1.displays, "CO|") != lineStarting(F1.displays, "CO|") &&
                lineStarting(P2.displays, "RCP|") == "RCP|0|" &&
                lineStarting(P2.displays, "CO|") == lineStarting(P1.displays, "CO|"),
            "W2 gfxbinscatter (11f) controls(i.(gn) c.(z zsq)) is controls(i.g z zsq) -- got `" +
                lineStarting(P1.displays, "CO|") + "` `" + lineStarting(P2.displays, "RCP|") + "` `" +
                lineStarting(P2.displays, "CO|") + "`\n" + joinLines(P2.errors));
  }

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2 gfxbinscatter: %d checks\n", gBsChecks);
}
