// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `ebmat` (goal eb-order-canonical-ematrix, eb-order SPEC T-P3 + T-P1,
// 2026-10-15) -- one coefficient source and one canonical e(b)/e(V). Every
// reader of e(b)/e(V) (`matrix b = e(b)`, `matrix list`, `: colnames e(b)`,
// el(), colsof(), `_b[]`/`_se[]` in both layers) answers from the live e()'s own
// block, in Stata's order (`_cons` last, never `(intercept)`), with the fit's
// full covariance; a later posting replaces it, and a live e() without an e(b)
// (estpost, pca) or no e() at all refuses with Stata's rc.
// Oracles: tests/eval/golden/aer_w2/w2g_ebmat.log (case aer_w2:w2g_ebmat) and
// internal capture w2v_estpost_ematrix.log
// (the w2v_estpost_ematrix capture, StataNow 19.5). Each check names the PREC
// arm it rests on. Every check below fails on main 615271bf (bin/main records:
// `: colnames e(b)` r(111), V[1,2] 0, `_b[x1]` after estpost the regress
// coefficient at rc 0, test after estpost rc 0, `_b[x1]` with no e() `.` at rc
// 0, erepost written over the engine's `(intercept)`-first order, `matrix drop`
// a silent rc-1 no-op, `input` storing doubles).
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gEbmatChecks = 0;
void ebCheck(bool cond, const std::string& label) {
  ++gEbmatChecks;
  w2check(cond, label);
}

// The w2g_ebmat / w2v_estpost_ematrix fixture (eb-order fixture.csv, 20 rows),
// typed as both do-files type it: `input double y x1 x2 ...` makes y double and
// the rest float.
const char* kFixture =
    "clear\n"
    "input double y x1 x2 g id yb\n"
    "4.6612 2.0271 1.4473 2 1 0\n"
    "3.2857 1.231 0.855 3 1 1\n"
    "1.2851 -1.0755 0.1427 1 1 0\n"
    "3.3677 -1.3669 -1.4133 2 1 1\n"
    "4.0168 0.9302 -1.1047 3 2 1\n"
    "4.379 2.5971 -0.1354 1 2 1\n"
    "4.2264 1.3382 1.1798 2 2 1\n"
    "1.4462 -0.8557 1.1633 3 2 0\n"
    "0.8513 -0.624 1.7998 1 3 1\n"
    "4.3434 1.8403 1.7309 2 3 0\n"
    "4.5986 3.0735 1.1301 3 3 1\n"
    "3.7891 1.4155 -0.7789 1 3 1\n"
    "3.8416 -0.5582 -1.1216 2 4 0\n"
    "3.2475 0.1903 -0.7956 3 4 1\n"
    "4.3595 2.7111 0.1867 1 4 0\n"
    "6.3316 3.4576 0.3045 2 4 1\n"
    "3.1369 1.4827 1.4791 3 5 0\n"
    "1.1121 -0.1738 2.0992 1 5 0\n"
    "3.55 1.0613 2.0141 2 5 1\n"
    "5.401 3.5251 0.2051 3 5 0\n"
    "end\n";

std::vector<std::string> displays(piet::SessionState& session, const std::string& src) {
  std::vector<std::string> out;
  try {
    auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object()) {
        out.push_back("ARRAY-BLOCK");  // defect class 1: never an array pushed as a block
        continue;
      }
      if (b.value("type", std::string()) == "display") out.push_back(b.value("text", std::string()));
    }
  } catch (const std::exception& e) {
    out.push_back(std::string("EXC:") + e.what());
  }
  return out;
}

std::string lineStarting(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines) {
    if (l.compare(0, prefix.size(), prefix) == 0) return l;
  }
  return std::string("<no " + prefix + " line>");
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

// The %21x field between the line's first two `|` after the tag.
bool fieldNear(const std::string& line, const char* oracleHex) {
  const std::size_t a = line.find('|');
  const std::size_t b = a == std::string::npos ? a : line.find('|', a + 1);
  if (b == std::string::npos) return false;
  const double got = fromStataHex(line.substr(a + 1, b - a - 1));
  const double want = fromStataHex(oracleHex);
  if (std::isnan(want) || std::isnan(got)) return false;
  return std::fabs(got - want) <= 1e-9 * std::fabs(want);
}

}  // namespace

void testW2Ebmat() {
  std::printf(
      "[selftest] --- W2 ebmat: one coefficient source, one canonical e(b)/e(V) "
      "(w2g_ebmat.log, w2v_estpost_ematrix.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2ebmat").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // (1) T-P1 after `regress y x1 x2`: the canonical stripe, el/colsof over it,
  // the full e(V), matrix list at rc 0.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"C|\" _rc \"|`c'|\"\n"
                                   "display \"S|\" colsof(e(b)) \"|\"\n"
                                   "display \"EL|\" %21x el(e(b),1,1) \"|\"\n"
                                   "matrix V = e(V)\n"
                                   "display \"V12|\" %21x V[1,2] \"|\"\n"
                                   "display \"VSE|\" (abs(V[1,1] - _se[x1]^2) < 1e-15) \"|\"\n"
                                   "capture matrix list e(b)\n"
                                   "display \"LB|\" _rc \"|\"\n"
                                   "local r : rownames V\n"
                                   "display \"VR|`r'|\"\n");
    ebCheck(lineStarting(L, "C|") == "C|0|x1 x2 _cons|",
            "W2 ebmat (1) e1_colnames_eb: `: colnames e(b)` after regress is `x1 x2 _cons` at rc 0 "
            "(main r(111)) -- got `" + lineStarting(L, "C|") + "`");
    ebCheck(lineStarting(L, "S|") == "S|3|",
            "W2 ebmat (1) e1_colsof_eb: colsof(e(b)) 3 -- got `" + lineStarting(L, "S|") + "`");
    ebCheck(fieldNear(lineStarting(L, "EL|"), "+1.a1a9c9edad936X-001"),
            "W2 ebmat (1) e1_el_eb_11: el(e(b),1,1) is _b[x1] (+1.a1a9c9edad936X-001), not the "
            "engine's (intercept) -- got `" + lineStarting(L, "EL|") + "`");
    ebCheck(fieldNear(lineStarting(L, "V12|"), "-1.f7d0ccaeed4b1X-00a"),
            "W2 ebmat (1) e1_V12: e(V)[1,2] is the fit's covariance -1.f7d0ccaeed4b1X-00a, never the "
            "diagonal's 0 (main 0, FU-945) -- got `" + lineStarting(L, "V12|") + "`");
    ebCheck(lineStarting(L, "VSE|") == "VSE|1|",
            "W2 ebmat (1) control (green on main too): e(V)[1,1] is _se[x1]^2, one source for both "
            "-- got `" +
                lineStarting(L, "VSE|") + "`");
    ebCheck(lineStarting(L, "LB|") == "LB|0|" && lineStarting(L, "VR|") == "VR|x1 x2 _cons|",
            "W2 ebmat (1) e1_list_eb / e1_list_V: `matrix list e(b)` rc 0, e(V) rows `x1 x2 _cons` -- "
            "got `" + lineStarting(L, "LB|") + "` `" + lineStarting(L, "VR|") + "`");
  }

  // (2) A later fit replaces e(b) (e3a), and a logit's e(b) is readable at once
  // (e4l, FU-460).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "quietly regress y x1\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"A|\" _rc \"|`c'|\"\n"
                                   "quietly logit yb x1 x2\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"L|\" _rc \"|`c'|\"\n"
                                   "display \"LEL|\" %21x el(e(b),1,1) \"|\"\n");
    ebCheck(lineStarting(L, "A|") == "A|0|x1 _cons|",
            "W2 ebmat (2) e3a_colnames_eb: after a second regress e(b) is `x1 _cons` -- got `" +
                lineStarting(L, "A|") + "`");
    ebCheck(lineStarting(L, "L|") == "L|0|x1 x2 _cons|",
            "W2 ebmat (2) e4l_colnames_eb (FU-460): `: colnames e(b)` right after logit is "
            "`x1 x2 _cons` (main r(111)) -- got `" + lineStarting(L, "L|") + "`");
    ebCheck(fieldNear(lineStarting(L, "LEL|"), "+1.31858fc410d2dX-003"),
            "W2 ebmat (2) e4l_el_eb_11: el(e(b),1,1) after logit +1.31858fc410d2dX-003 -- got `" +
                lineStarting(L, "LEL|") + "`");
  }

  // (3) estpost after a fit posts no e(b) (matches estpost's behaviour, identified from its ado): `_b[]` is
  // r(111), `matrix b = e(b)` a 0x0 matrix, `: colnames e(b)` r(111), el()
  // r(109), test r(321), and the posting's own e() is live.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "quietly estpost summarize x1 x2\n"
                                   "display \"CMD|`e(cmd)'|`e(subcmd)'|\"\n"
                                   "capture local w = _b[x1]\n"
                                   "display \"B|\" _rc \"|\"\n"
                                   "capture display _b[x1]\n"
                                   "display \"DB|\" _rc \"|\"\n"
                                   "matrix b = e(b)\n"
                                   "display \"BS|\" rowsof(b) \"|\" colsof(b) \"|\"\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"C|\" _rc \"|\"\n"
                                   "capture local v = el(e(b),1,1)\n"
                                   "display \"EL|\" _rc \"|\"\n"
                                   "capture matrix list e(b)\n"
                                   "display \"LB|\" _rc \"|\"\n"
                                   "capture test x1\n"
                                   "display \"T|\" _rc \"|\"\n"
                                   "capture testparm x1\n"
                                   "display \"TP|\" _rc \"|\"\n");
    ebCheck(lineStarting(L, "CMD|") == "CMD|estpost|summarize|",
            "W2 ebmat (3) ep13_ecmd: e(cmd)/e(subcmd) `estpost|summarize` -- got `" +
                lineStarting(L, "CMD|") + "`");
    ebCheck(lineStarting(L, "B|") == "B|111|" && lineStarting(L, "DB|") == "DB|111|",
            "W2 ebmat (3) ep13_local_bx1 / ep13_disp_bx1: `_b[x1]` after estpost is r(111) in both "
            "layers, never the regress coefficient at rc 0 (main) -- got `" + lineStarting(L, "B|") +
                "` `" + lineStarting(L, "DB|") + "`");
    ebCheck(lineStarting(L, "BS|") == "BS|0|0|",
            "W2 ebmat (3) ep13_b_shape: `matrix b = e(b)` after estpost is 0x0 (main 1x2) -- got `" +
                lineStarting(L, "BS|") + "`");
    ebCheck(lineStarting(L, "C|") == "C|111|" && lineStarting(L, "EL|") == "EL|109|" &&
                lineStarting(L, "LB|") == "LB|111|",
            "W2 ebmat (3) ep13_colnames_eb 111, ep13_el_eb_11 109, ep13_list_eb 111 -- got `" +
                lineStarting(L, "C|") + "` `" + lineStarting(L, "EL|") + "` `" +
                lineStarting(L, "LB|") + "`");
    ebCheck(lineStarting(L, "T|") == "T|321|" && lineStarting(L, "TP|") == "TP|301|",
            "W2 ebmat (3) ep14_test 321, ep14_testparm 301 (main: test rc 0 on the old fit) -- got `" +
                lineStarting(L, "T|") + "` `" + lineStarting(L, "TP|") + "`");
  }

  // (4) erepost: on an estpost posting M becomes e(b) under M's names
  // (ep14e_*, ep14e2_*); on a fit, M is positional over the canonical order
  // (w2g_ebmat e8_bx1 / e8_bcons).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "quietly estpost summarize x1 x2\n"
                                   "matrix M = (7, 9)\n"
                                   "erepost b = M\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"E|\" _rc \"|`c'|\" _b[c1] \"|\"\n"
                                   "matrix M2 = (7, 9)\n"
                                   "matrix colnames M2 = x1 x2\n"
                                   "erepost b = M2\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"E2|\" _rc \"|`c'|\" _b[x2] \"|\"\n"
                                   "quietly regress y x1 x2\n"
                                   "matrix N = (1,2,3)\n"
                                   "matrix colnames N = x1 x2 _cons\n"
                                   "erepost b = N\n"
                                   "display \"R|\" _b[x1] \"|\" _b[_cons] \"|\"\n");
    ebCheck(lineStarting(L, "E|") == "E|0|c1 c2|7|",
            "W2 ebmat (4) ep14e_colnames_eb / ep14e_local_bc1: erepost on estpost posts M as e(b) "
            "(main r(111)) -- got `" + lineStarting(L, "E|") + "`");
    ebCheck(lineStarting(L, "E2|") == "E2|0|x1 x2|9|",
            "W2 ebmat (4) ep14e2_colnames_eb / ep14e2_local_bx2: a second erepost takes M2's names -- "
            "got `" + lineStarting(L, "E2|") + "`");
    ebCheck(lineStarting(L, "R|") == "R|1|3|",
            "W2 ebmat (4) e8_bx1 / e8_bcons: erepost over regress is positional in e(b)'s order "
            "(_b[x1] 1, _b[_cons] 3; main 2 and 1) -- got `" + lineStarting(L, "R|") + "`");
  }

  // (5) pca (no e(b), w2g_ebmat E15c): `matrix b = e(b)` is 0x0 at rc 0 and
  // `_b[x1]` r(111) in the macro layer; then no e() at all (ereturn clear):
  // `_b[x1]` and `_b[x1]*2` are r(111) too (ep15f_local_bx1,
  // ep15f_disp_bx1_times2; FU-949), `matrix b = e(b)` r(301).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "quietly pca x1 x2\n"
                                   "capture matrix b = e(b)\n"
                                   "display \"PB|\" _rc \"|\" colsof(b) \"|\"\n"
                                   "capture local w = _b[x1]\n"
                                   "display \"PL|\" _rc \"|\"\n"
                                   "ereturn clear\n"
                                   "capture local w = _b[x1]\n"
                                   "display \"FL|\" _rc \"|\"\n"
                                   "capture display _b[x1]*2\n"
                                   "display \"FD|\" _rc \"|\"\n"
                                   "capture scalar s = _b[x1]\n"
                                   "display \"FS|\" _rc \"|\"\n"
                                   "capture generate z = _b[x1]\n"
                                   "display \"FG|\" _rc \"|\"\n"
                                   "capture matrix drop b\n"
                                   "capture matrix b = e(b)\n"
                                   "display \"FB|\" _rc \"|\"\n");
    ebCheck(lineStarting(L, "PB|") == "PB|0|0|",
            "W2 ebmat (5) e15c_matrix_b / ep15c_b_shape: `matrix b = e(b)` after pca is rc 0 and 0x0 "
            "-- got `" + lineStarting(L, "PB|") + "`");
    ebCheck(lineStarting(L, "PL|") == "PL|111|",
            "W2 ebmat (5) e15c_local_bx1: `local w = _b[x1]` after pca is r(111) (main `.` at rc 0) "
            "-- got `" + lineStarting(L, "PL|") + "`");
    ebCheck(lineStarting(L, "FL|") == "FL|111|" && lineStarting(L, "FD|") == "FD|111|" &&
                lineStarting(L, "FS|") == "FS|111|" && lineStarting(L, "FG|") == "FG|111|",
            "W2 ebmat (5) ep15f_local_bx1 / ep15f_disp_bx1_times2 (FU-949): with no e() `_b[x1]` is "
            "r(111) in the macro layer, an expression, scalar and generate (main `.` at rc 0, gen "
            "rc 1) -- got `" + lineStarting(L, "FL|") + "` `" + lineStarting(L, "FD|") + "` `" +
                lineStarting(L, "FS|") + "` `" + lineStarting(L, "FG|") + "`");
    ebCheck(lineStarting(L, "FB|") == "FB|301|",
            "W2 ebmat (5) ep15f_matrix_b: `matrix b = e(b)` with no e() is r(301) -- got `" +
                lineStarting(L, "FB|") + "`");
  }

  // (6) `matrix drop` (ep15f_b_shape: the dropped matrix is gone, r(111) on
  // confirm; main: a silent rc-1 no-op that kept it).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s,
                            "matrix Z = (1,2)\n"
                            "matrix Y = (3)\n"
                            "capture matrix drop Z\n"
                            "display \"D|\" _rc \"|\"\n"
                            "capture confirm matrix Z\n"
                            "display \"CZ|\" _rc \"|\"\n"
                            "capture confirm matrix Y\n"
                            "display \"CY|\" _rc \"|\"\n"
                            "capture matrix drop nosuch\n"
                            "display \"DN|\" _rc \"|\"\n"
                            "matrix drop _all\n"
                            "capture confirm matrix Y\n"
                            "display \"CA|\" _rc \"|\"\n");
    ebCheck(lineStarting(L, "D|") == "D|0|" && lineStarting(L, "CZ|") == "CZ|111|" &&
                lineStarting(L, "CY|") == "CY|0|",
            "W2 ebmat (6) matrix drop Z removes Z only -- got `" + lineStarting(L, "D|") + "` `" +
                lineStarting(L, "CZ|") + "` `" + lineStarting(L, "CY|") + "`");
    ebCheck(lineStarting(L, "DN|") == "DN|111|" && lineStarting(L, "CA|") == "CA|111|",
            "W2 ebmat (6) matrix drop of an absent name is r(111); matrix drop _all clears -- got `" +
                lineStarting(L, "DN|") + "` `" + lineStarting(L, "CA|") + "`");
  }

  // (7) poisson's e(V) is the fit's own (FU-859; w2v_estpost_ematrix.log
  // eb_po_rob_V_shape `x c1 _cons`, nonzero symmetric off-diagonals).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly poisson g x1 x2, vce(robust)\n"
                                   "matrix V = e(V)\n"
                                   "local r : rownames V\n"
                                   "display \"PV|`r'|\" (V[1,2]!=0) \"|\" (V[1,3]!=0) \"|\" (V[2,3]!=0) "
                                   "\"|\" (V[1,2]==V[2,1]) \"|\" (abs(V[2,2] - _se[x2]^2) < 1e-15) \"|\"\n");
    ebCheck(lineStarting(L, "PV|") == "PV|x1 x2 _cons|1|1|1|1|1|",
            "W2 ebmat (7) eb_po_rob_V_shape: poisson e(V) rows `x1 x2 _cons`, real off-diagonals, "
            "symmetric, diagonal = _se^2 (main: (intercept) first and zero off-diagonals) -- got `" +
                lineStarting(L, "PV|") + "`");
  }

  // (8) `input double y x1 x2` types y alone; untyped columns are float
  // (FU-944; w2g_ebmat.log data_sum x1 +1.63a1620ffffffX+004, the float data's
  // sum; main summed the doubles, +1.63a161e4f7660X+004). Read through a local
  // as data_sum does: summarize's own r(sum) is one ulp from Stata's
  // (+1.63a1621000000X+004, inside rtol 1e-9), and the local's text is what the
  // oracle printed.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly summarize x1\n"
                                   "local sx = r(sum)\n"
                                   "display \"SX|\" %21x `sx' \"|\"\n"
                                   "local ty : type y\n"
                                   "local tx : type x1\n"
                                   "display \"TY|`ty'|`tx'|\"\n");
    ebCheck(lineStarting(L, "SX|") == "SX|+1.63a1620ffffffX+004|",
            "W2 ebmat (8) data_sum: x1 holds float values -- got `" + lineStarting(L, "SX|") + "`");
    ebCheck(lineStarting(L, "TY|") == "TY|double|float|",
            "W2 ebmat (8) input_double_types: `input double y x1` types y double, x1 float -- got `" +
                lineStarting(L, "TY|") + "`");
  }

  // (9) colsof()/rowsof()/el() over e(b) on the row surfaces (assert, generate,
  // count if) take the same value as in `display` (w2g_ebmat.log:305
  // e1_colsof_eb 3; main: rc 1 "function colsof() is not supported").
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 x2\n"
                                   "capture assert colsof(e(b)) == 3 & rowsof(e(V)) == 3\n"
                                   "display \"RA|\" _rc \"|\"\n"
                                   "capture generate k = colsof(e(b))\n"
                                   "display \"RG|\" _rc \"|\" k[1] \"|\"\n"
                                   "capture count if el(e(b),1,3) == _b[_cons]\n"
                                   "display \"RC|\" _rc \"|\" r(N) \"|\"\n");
    ebCheck(lineStarting(L, "RA|") == "RA|0|" && lineStarting(L, "RG|") == "RG|0|3|" &&
                lineStarting(L, "RC|") == "RC|0|20|",
            "W2 ebmat (9) e1_colsof_eb on the row surfaces: assert/generate/count if read colsof(), "
            "rowsof() and el() of e(b) -- got `" + lineStarting(L, "RA|") + "` `" +
                lineStarting(L, "RG|") + "` `" + lineStarting(L, "RC|") + "`");
  }

  // (10) The equation stripe (review round 2026-10-15). logit/probit keep the
  // depvar equation on every column of e(b) and both stripes of e(V)
  // (w2g_ebmat.log:654-656 and :724 `yb:x1 yb:x2 yb:_cons`), while `: colnames`
  // drops it (:647 e4l_colnames_eb `x1 x2 _cons`); poisson's equation is the
  // depvar too (not captured). Main: `: colfullnames e(b)`
  // r(111).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "generate yn = round(y)\n"
                                   "quietly logit yb x1 x2\n"
                                   "local f\n"
                                   "local r\n"
                                   "local c\n"
                                   "capture local f : colfullnames e(b)\n"
                                   "display \"LF|\" _rc \"|`f'|\"\n"
                                   "capture local r : rowfullnames e(V)\n"
                                   "display \"LR|\" _rc \"|`r'|\"\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"LC|\" _rc \"|`c'|\"\n"
                                   "quietly probit yb x1 x2\n"
                                   "matrix b = e(b)\n"
                                   "local f : colfullnames b\n"
                                   "display \"PF|`f'|\"\n"
                                   "quietly poisson yn x1 x2\n"
                                   "local f : colfullnames e(b)\n"
                                   "display \"OF|`f'|\"\n");
    ebCheck(lineStarting(L, "LF|") == "LF|0|yb:x1 yb:x2 yb:_cons|" &&
                lineStarting(L, "LR|") == "LR|0|yb:x1 yb:x2 yb:_cons|" &&
                lineStarting(L, "LC|") == "LC|0|x1 x2 _cons|",
            "W2 ebmat (10) logit e(b)/e(V) keep the `yb:` equation (w2g_ebmat.log:654-656), "
            "`: colnames` drops it (e4l_colnames_eb) -- got `" + lineStarting(L, "LF|") + "` `" +
                lineStarting(L, "LR|") + "` `" + lineStarting(L, "LC|") + "`");
    ebCheck(lineStarting(L, "PF|") == "PF|yb:x1 yb:x2 yb:_cons|",
            "W2 ebmat (10) probit `matrix b = e(b)` keeps the `yb:` equation (w2g_ebmat.log:724) "
            "-- got `" + lineStarting(L, "PF|") + "`");
    ebCheck(lineStarting(L, "OF|") == "OF|yn:x1 yn:x2 yn:_cons|",
            "W2 ebmat (10) poisson e(b) is in the depvar's equation -- got `" +
                lineStarting(L, "OF|") + "`");
  }

  // (11) A fit whose Stata e(b) has columns the engine's coeftable does not
  // carry (ologit's cut points, tobit's var(e.y), nbreg's lnalpha) never
  // answers a shorter e(b) at rc 0. Since BUG-1196 the fit posts Stata's whole
  // e(b)/e(V) from the engine's --emit-v covariance, so every reader answers
  // with the ancillary columns (StataBE 19.5 on this fixture: rc 0 for all four,
  // `yo:x1 yo:x2 /cut1 /cut2`, `yc:x1 yc:x2 yc:_cons /var(e.yc)`); nbreg here
  // sits at the alpha -> 0 boundary (Stata's lnalpha -64.9), where the engine
  // fits the Poisson limit and sends no covariance, so every matrix reader stays
  // the named rc-198 refusal. `_b[x1]`, read by name, answers throughout. Before
  // the refusal, main answered `matrix b = e(b)` at rc 0 with 2 or 3 columns.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "generate yo = 1 + (y > 2.5) + (y > 4)\n"
                                   "generate yc = max(0, y - 3)\n"
                                   "generate yn = round(y)\n"
                                   "foreach m in ologit tobit nbreg {\n"
                                   "  if \"`m'\" == \"ologit\" quietly ologit yo x1 x2\n"
                                   "  if \"`m'\" == \"tobit\" quietly tobit yc x1 x2, ll(0)\n"
                                   "  if \"`m'\" == \"nbreg\" quietly nbreg yn x1 x2\n"
                                   "  capture matrix b = e(b)\n"
                                   "  local r1 = _rc\n"
                                   "  capture local c : colnames e(b)\n"
                                   "  local r2 = _rc\n"
                                   "  capture local k = colsof(e(b))\n"
                                   "  local r3 = _rc\n"
                                   "  capture matrix list e(b)\n"
                                   "  local r4 = _rc\n"
                                   "  capture local w = _b[x1]\n"
                                   "  display \"Q|`m'|`r1'|`r2'|`r3'|`r4'|\" _rc \"|\"\n"
                                   "  local f\n"
                                   "  capture local f : colfullnames e(b)\n"
                                   "  display \"N|`m'|`f'|\"\n"
                                   "}\n");
    const std::pair<const char*, const char*> kWant[] = {
        {"ologit", "|0|0|0|0|0|yo:x1 yo:x2 /cut1 /cut2"},
        {"tobit", "|0|0|0|0|0|yc:x1 yc:x2 yc:_cons /var(e.yc)"},
        {"nbreg", "|198|198|198|198|0|"}};
    for (const auto& [m, w] : kWant) {
      const auto after = [&L](const std::string& prefix) {
        const std::string line = lineStarting(L, prefix);
        return line.size() >= prefix.size() ? line.substr(prefix.size()) : std::string("<none>");
      };
      const std::string got = "|" + after(std::string("Q|") + m + "|") +
                              after(std::string("N|") + m + "|");
      ebCheck(got == std::string(w) + "|",
              std::string("W2 ebmat (11) ") + m +
                  ": e(b) readers answer Stata's whole stripe, ancillary columns included, or "
                  "refuse r(198) -- never a shorter matrix; `_b[x1]` answers -- got `" + got + "`");
    }
  }

  // ROUND 3 (internal note eb-order-rediagnosis/DIAGNOSIS.md
  // sections 3 and 7): the stripe classification is a CLOSED table. The census
  // panel (DIAGNOSIS p1/census.do) carries every family below.
  const std::string kPanel =
      "clear\n"
      "set obs 200\n"
      "generate id = ceil(_n/4)\n"
      "generate t = mod(_n-1,4)+1\n"
      "generate gvar = cond(mod(id,3)==0, 0, cond(mod(id,3)==1, 3, 4))\n"
      "generate x1 = mod(_n*7919,101)/50 - 1\n"
      "generate x2 = mod(_n*104729,97)/48 - 1\n"
      "generate y = 1 + 0.5*x1 - 0.3*x2 + mod(_n*15485863,83)/41 - 1 + (gvar>0 & t>=gvar)*0.8\n"
      "generate yb = y > 1.3\n"
      "generate yn = floor(exp(0.3*x1)+mod(_n*7,13)/4)\n"
      "generate gvarm = cond(gvar==0, ., gvar)\n"
      "generate treat = gvar > 0\n"
      "xtset id t\n";
  // The three e(b) readers of a refusal, as `R|<tag>|<matrix b>|<colfullnames>|
  // <colsof>|<matrix list>|`.
  const auto readers = [](const std::string& tag) {
    return "capture matrix drop b\n"
           "capture matrix b = e(b)\n"
           "local r1 = _rc\n"
           "capture local f : colfullnames e(b)\n"
           "local r2 = _rc\n"
           "capture local k = colsof(e(b))\n"
           "local r3 = _rc\n"
           "capture matrix list e(b)\n"
           "display \"R|" + tag + "|`r1'|`r2'|`r3'|\" _rc \"|\"\n";
  };

  // (12) The closed default. csdid's Stata e(b) under the default agg(attgt)
  // has one column per ATT(g,t) cell (csdid_kitchensink_oracle.log:61-87, :107
  // `ATTGT_K=42`), drdid's is `ATET:r1vs0.<treat>` (drdid's naming, identified from its ado), and
  // did_imputation and mean have no record: every e(b) reader is the named
  // r(198). 0a8c2d54 answered csdid's one `ATT` column at rc 0 (`C2|0|1`), and
  // mean's `y x1` (census R|mean b0|k2).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, kPanel +
                                   "quietly csdid y x1, ivar(id) time(t) gvar(gvar)\n" +
                                   readers("csdid") +
                                   "quietly did_imputation y id t gvarm, horizons(0 1)\n" +
                                   readers("didimp") +
                                   "quietly mean y x1\n" + readers("mean") +
                                   "preserve\n"
                                   "keep if t==2 | t==3\n"
                                   "quietly drdid y x1, ivar(id) time(t) treatment(treat) dripw\n" +
                                   readers("drdid") + "restore\n");
    // csdid-ematrix-post / drdid-ematrix-post: csdid and drdid now POST Stata's
    // e(b)/e(V) (the engine's attgt cells `g#:t_#_#`+`wgt:w#_#`; `ATET:r1vs0.treat`),
    // so every reader answers at rc 0 (w2ag_csdid_drdid.log:114 ag_def_names,
    // ag_def_mlist `0|0|`, :1810 ag_drdid). mean posts its varlist stripe and full
    // covariance too (BUG-1541; StataBE 19.5 `mean x x2` colnames `x x2`, the census's
    // `y x1`; scratch-laneA2/agents/wml/x2.log). did_imputation keeps the refusal.
    for (const char* tag : {"csdid", "drdid", "mean"}) {
      const std::string want = std::string("R|") + tag + "|0|0|0|0|";
      ebCheck(lineStarting(L, std::string("R|") + tag + "|") == want,
              std::string("W2 ebmat (12) ") + tag +
                  ": the posted e(b) answers every reader at rc 0 (w2ag_csdid_drdid.log "
                  "ag_def_mlist, ag_drdid; mean BUG-1541) -- got `" +
                  lineStarting(L, std::string("R|") + tag + "|") + "`");
    }
    for (const char* tag : {"didimp"}) {
      const std::string want = std::string("R|") + tag + "|198|198|198|198|";
      ebCheck(lineStarting(L, std::string("R|") + tag + "|") == want,
              std::string("W2 ebmat (12) ") + tag +
                  ": an e(cmd) whose Stata stripe this version cannot build refuses every e(b) "
                  "reader r(198) (DIAGNOSIS section 3; csdid_kitchensink_oracle.log:107 "
                  "ATTGT_K=42) -- got `" + lineStarting(L, std::string("R|") + tag + "|") + "`");
    }
  }

  // (13) The conditional fe fits keep the depvar equation and have no `_cons`:
  // xtlogit, fe fits clogit (equation named for the depvar), and
  // xtpoisson, fe names its equation for the depvar too. 0a8c2d54 refused both r(198).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, kPanel +
                                   "quietly xtlogit yb x1 x2, fe\n"
                                   "local f\n"
                                   "capture local f : colfullnames e(b)\n"
                                   "display \"XL|\" _rc \"|`f'|\" colsof(e(b)) \"|\"\n"
                                   "quietly xtpoisson yn x1 x2, fe\n"
                                   "local f\n"
                                   "capture local f : colfullnames e(b)\n"
                                   "display \"XP|\" _rc \"|`f'|\" colsof(e(b)) \"|\"\n");
    ebCheck(lineStarting(L, "XL|") == "XL|0|yb:x1 yb:x2|2|" &&
                lineStarting(L, "XP|") == "XP|0|yn:x1 yn:x2|2|",
            "W2 ebmat (13) xtlogit/xtpoisson fe: e(b) is `yb:x1 yb:x2` / `yn:x1 yn:x2` "
            "-- got `" + lineStarting(L, "XL|") + "` `" +
                lineStarting(L, "XP|") + "`");
  }

  // (14) G1: an omitted (collinear) regressor. The engine keeps it at b 0 / se 0
  // under its bare name where Stata names it `o.x` (w2u_binary_ml_edges.log:215
  // bm_l_dz0_names `x1 o.dz0 _cons`). The fitted stripe now retains the
  // norm-selected omission, and e(V) has its exact zero row/column.
  // Controls: logit with `asis` keeps dz0 (w2u_binary_ml_edges.log:312
  // bm_l_dz0_asis_names `x1 dz0 _cons`), and a reposted 0 is not an omission.
  {
    piet::SessionState s(workdir);
    const auto L = displays(
        s, std::string(kFixture) +
               "generate x3c = 2*x1\n"
               "quietly regress y x1 x2 x3c\n"
               "capture matrix b = e(b)\n"
               "local r1 = _rc\n"
               "capture matrix V = e(V)\n"
               "local r2 = _rc\n"
               "capture local c : colnames e(b)\n"
               "display \"O|`r1'|`r2'|\" _rc \"|`c'|\" colsof(e(b)) \"|\" "
               "el(e(b),1,1) \"|\" el(e(V),1,1) \"|\" el(e(V),1,2) \"|\"\n"
               "estimates store Omitted\nquietly regress y x1 x2\nestimates restore Omitted\n"
               "matrix restored_b=e(b)\nmatrix restored_V=e(V)\n"
               "quietly regress y x1 x2\n"
               "matrix Z = (0, 2, 3)\n"
               // e(b)'s names: an unnamed Z is r(507) in Stata (BUG-1494).
               "matrix colnames Z = x1 x2 _cons\n"
               "erepost b = Z\n"
               "local c\n"
               "capture local c : colnames e(b)\n"
               "display \"OE|\" _rc \"|`c'|\" el(e(b),1,1) \"|\"\n"
               "clear\n"
               "set obs 200\n"
               "generate double x1 = mod(_n*7,23)/4 - 2.75 + mod(_n,3)/8\n"
               "generate double x2 = mod(_n*11,17)/8 + mod(_n*5,9)/16\n"
               "generate byte c = mod(_n*3,4)\n"
               "generate double eps = (mod(_n*37,41)/41 + mod(_n*53,59)/59 + "
               "mod(_n*29,31)/31 - 1.5)*2.2\n"
               "generate byte y = (0.9*x1 - 0.5*x2 + 0.3*c + eps + 0.4 > 0)\n"
               "generate byte dz0 = (mod(_n,9)==0 & y==0)\n"
               "quietly logit y x1 dz0, asis\n"
               "local c\n"
               "capture local c : colnames e(b)\n"
               "display \"OA|\" _rc \"|`c'|\"\n");
    ebCheck(lineStarting(L, "O|") == "O|0|0|0|o.x1 x2 x3c _cons|4|0|0|0|",
            "W2 ebmat (14) G1: collinear x3c=2*x1 retains the frozen canonical "
            "o.x1 x2 x3c _cons stripe with exact zero omitted coefficient/covariance "
            "-- got `" + lineStarting(L, "O|") + "`");
    const auto* ob = s.eval.findMatrix("b");
    const auto* ov = s.eval.findMatrix("V");
    const auto* rb = s.eval.findMatrix("restored_b");
    const auto* rv = s.eval.findMatrix("restored_V");
    ebCheck(ob && ov && rb && rv && ob->colnames == rb->colnames && ob->data == rb->data &&
                ov->colnames == rv->colnames && ov->data == rv->data,
            "omitted canonical b/V stripe survives estimates store/restore exactly");
    ebCheck(lineStarting(L, "OE|") == "OE|0|x1 x2 _cons|0|" &&
                lineStarting(L, "OA|") == "OA|0|x1 dz0 _cons|",
            "W2 ebmat (14) G1 controls: a reposted 0 and an `asis` regressor still answer "
            "(w2u_binary_ml_edges.log:312) -- got `" + lineStarting(L, "OE|") + "` `" +
                lineStarting(L, "OA|") + "`");
  }

  // (14b) G1 survives `erepost b = M` on a fit: the repost nulls every se, but
  // the omitted column is still omitted -- Stata's e(b) keeps its `o.` name
  // (w2v_estripe_census.log:274 esc_regress_omit `o.x1 x2 x3c _cons`) after
  // a second repost too, while the supplied coefficient values are preserved.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "generate x3c = 2*x1\n"
                                   "quietly regress y x1 x2 x3c\n"
                                   "matrix Z = (1, 2, 3, 4)\n"
                                   // e(b)'s names, `o.` included: an unnamed Z (or
                                   // the bare x1) is r(507) in Stata (BUG-1494).
                                   "matrix colnames Z = o.x1 x2 x3c _cons\n"
                                   "erepost b = Z\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "local r1 = _rc\n"
                                   "capture matrix b = e(b)\n"
                                   "local r2 = _rc\n"
                                   "matrix Z = (5, 6, 7, 8)\n"
                                   "matrix colnames Z = o.x1 x2 x3c _cons\n"
                                   "erepost b = Z\n"
                                   "capture local c : colnames e(b)\n"
                                   "display \"OR|`r1'|`r2'|\" _rc \"|`c'|\" "
                                   "el(e(b),1,1) \"|\" el(e(b),1,4) \"|\"\n");
    ebCheck(lineStarting(L, "OR|") == "OR|0|0|0|o.x1 x2 x3c _cons|5|8|",
            "W2 ebmat (14b) erepost keeps the omitted canonical name and supplied b values "
            "(w2v_estripe_census.log:274 Stata stripe) -- got `" +
                lineStarting(L, "OR|") + "`");
  }

  // (14c) ME08/C7/EB-BASE: a factor term's base level is a column of e(b) and
  // e(V) (b 0, a zero row/column), recorded once at the fit (eb_stripe):
  // w2g_ebmat.log:452 e2_colnames_eb `x1 1b.g 2.g 3.g _cons`, e2_colsof_eb 5,
  // e2_el_eb_12 0; `_b[1b.g]` and `_b[1.g]` are 0 (w2f_residuals.log
  // e_b_0bc/e_b_0c). Before: every name/operand reader refused r(198).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly regress y x1 i.g\n"
                                   "local c\n"
                                   "capture local c : colnames e(b)\n"
                                   "local r1 = _rc\n"
                                   "matrix V = e(V)\n"
                                   "display \"FB|`r1'|`c'|\" colsof(e(b)) \"|\" el(e(b),1,2) \"|\" "
                                   "_b[1b.g] \"|\" _b[1.g] \"|\" el(V,2,2) \"|\" rowsof(V) \"|\"\n");
    ebCheck(lineStarting(L, "FB|") == "FB|0|x1 1b.g 2.g 3.g _cons|5|0|0|0|0|5|",
            "W2 ebmat (14c) base levels: e(b) `x1 1b.g 2.g 3.g _cons` with a zero base column, "
            "_b[1b.g] = _b[1.g] = 0, a zero e(V) row (w2g_ebmat.log:452) -- got `" +
                lineStarting(L, "FB|") + "`");
  }

  // (15) rdrobust posts Stata's 1x3 e(b) (w2g_ebmat.log:2302-2312 e15d_list_eb
  // `e(b)[1,3]` Conventional Bias-corre~d Robust, rc 0): as rdrobust posts it (identified from its ado),
  // b = (tau_cl, tau_bc, tau_bc), V diagonal (V_cl, V_cl, V_rb). Every reader
  // answers at rc 0 and `_b[x1]` stays r(111). Before: every reader r(198).
  {
    piet::SessionState s(workdir);
    const auto L = displays(
        s, kPanel + "quietly rdrobust y x1, c(0.5)\n" + readers("rd") +
               "display \"RB|`: colfullnames e(b)'|`: rownames e(b)'|\" "
               "(el(e(b),1,2) == el(e(b),1,3)) \"|\" (el(e(V),1,1) == el(e(V),2,2)) \"|\" "
               "(el(e(V),1,2) == 0) \"|\" (abs(sqrt(el(e(V),3,3)) - _se[Robust]) < 1e-15) \"|\"\n"
               "capture display _b[x1]\n"
               "display \"RX|\" _rc \"|\"\n");
    ebCheck(lineStarting(L, "R|rd|") == "R|rd|0|0|0|0|",
            "W2 ebmat (15) rdrobust: every e(b) reader answers the posted 1x3 at rc 0 "
            "(w2g_ebmat.log:2312) -- got `" + lineStarting(L, "R|rd|") + "`");
    ebCheck(lineStarting(L, "RB|") == "RB|Conventional Bias-corrected Robust|y1|1|1|1|1|" &&
                lineStarting(L, "RX|") == "RX|111|",
            "W2 ebmat (15) rdrobust: e(b) is y1:Conventional Bias-corrected Robust with "
            "b_bc = b_rb and V diagonal (V_cl, V_cl, V_rb); _b[x1] is r(111) -- got `" +
                lineStarting(L, "RB|") + "` `" + lineStarting(L, "RX|") + "`");
  }

  // (15b) e15d_eststo2/e15d_esttab2 (w2g_ebmat.log:2477-2513): rddensity
  // clears e() and posts scalars without `ereturn post` (rddensity's behaviour, identified from its ado):
  // no e(cmd)/e(N); `estimates store` and eststo
  // (`_est hold` on an unposted e()) are r(301), so esttab of the
  // name is r(111). Before: e(cmd) rddensity, stored at rc 0.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, kPanel +
                                   "quietly regress y x1 x2\n"
                                   "quietly rddensity x1, c(0.1)\n"
                                   "display \"DC|`e(cmd)'|\" e(N) \"|\"\n"
                                   "capture eststo RD2\n"
                                   "local r1 = _rc\n"
                                   "capture estimates store RD3\n"
                                   "local r2 = _rc\n"
                                   "capture esttab RD2\n"
                                   "local r3 = _rc\n"
                                   "capture matrix list e(b)\n"
                                   "display \"DS|`r1'|`r2'|`r3'|\" _rc \"|\"\n");
    ebCheck(lineStarting(L, "DC|") == "DC||.|" && lineStarting(L, "DS|") == "DS|301|301|111|111|",
            "W2 ebmat (15b) rddensity: no e(cmd)/e(N); eststo and estimates store r(301), "
            "esttab r(111), matrix list e(b) r(111) -- got `" + lineStarting(L, "DC|") + "` `" +
                lineStarting(L, "DS|") + "`");
  }

  // (15c) e10_nlcom/e10_rb (w2g_ebmat.log:1311-1338): an element of e(b) is a
  // parameter of nlcom (Stata differentiates with respect to it), so
  // `el(e(b),1,1)/el(e(b),1,2)` is `_b[x1]/_b[x2]` -- same estimate and se.
  // Before: r(1) "bare identifier 'b'".
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, kPanel +
                                   "quietly regress y x1 x2\n"
                                   "quietly nlcom _b[x1]/_b[x2]\n"
                                   "scalar nl_b0 = el(r(b),1,1)\n"
                                   "scalar nl_v0 = el(r(V),1,1)\n"
                                   "capture nlcom el(e(b),1,1)/e(b)[1,2]\n"
                                   "display \"NL|\" _rc \"|\" (el(r(b),1,1) == nl_b0) \"|\" "
                                   "(abs(el(r(V),1,1) - nl_v0) <= 1e-12*abs(nl_v0)) \"|\"\n");
    ebCheck(lineStarting(L, "NL|") == "NL|0|1|1|",
            "W2 ebmat (15c) nlcom el(e(b),1,k) / e(b)[1,k] is _b[<column k>] -- got `" +
                lineStarting(L, "NL|") + "`");
  }

  // (16) glm's ML fit posts its one equation under the depvar
  // (w2v_estripe_census.log:1562 esc_glm_ml `yb:x1 yb:x2 yb:_cons` for b and V;
  // aer_estim2_glm_oracle.log:230-238 `price:mpg ... price:_cons`), `: colnames`
  // drops the equation and `_b[x1]` still reads the coefficient. 3654a96c posted
  // the bare `x1 x2 _cons` (FU-1017).
  {
    piet::SessionState s(workdir);
    const auto L = displays(s, std::string(kFixture) +
                                   "quietly glm yb x1 x2, family(binomial) link(logit)\n"
                                   "local f\n"
                                   "local c\n"
                                   "local v\n"
                                   "capture local f : colfullnames e(b)\n"
                                   "local r1 = _rc\n"
                                   "capture local c : colnames e(b)\n"
                                   "capture local v : rowfullnames e(V)\n"
                                   "display \"G|`r1'|`f'|`c'|`v'|\" colsof(e(b)) \"|\" "
                                   "(_b[x1] == el(e(b),1,1)) \"|\"\n");
    ebCheck(lineStarting(L, "G|") ==
                "G|0|yb:x1 yb:x2 yb:_cons|x1 x2 _cons|yb:x1 yb:x2 yb:_cons|3|1|",
            "W2 ebmat (16) glm: e(b)/e(V) are `yb:x1 yb:x2 yb:_cons` (w2v_estripe_census.log:1562 "
            "esc_glm_ml), `: colnames e(b)` `x1 x2 _cons`, _b[x1] = el(e(b),1,1) -- got `" +
                lineStarting(L, "G|") + "`");
  }

  // The frozen coefplot-family factorial fixture has empty and collinear
  // cells. Its fitted stripe must survive both estimates store and changed data.
  {
    piet::SessionState s(workdir);
    const auto L = displays(s,
        "clear\nset obs 40\n"
        "generate g = mod(_n-1,4)+1\n"
        "generate d = mod(_n,2)\n"
        "generate x1 = mod(_n*7,11)\n"
        "generate x2 = mod(_n*3,5)+.5*mod(_n,2)\n"
        "generate ev_0 = mod(_n,5)==2\n"
        "generate ev_m2 = mod(_n,5)==0\n"
        "generate y = 1.5*x1-.8*x2+.7*g+mod(_n*5,7)/3+.4*ev_0-.3*ev_m2\n"
        "quietly regress y x1 i.d##i.g\n"
        "matrix Bfull = e(b)\nmatrix Vfull = e(V)\n"
        "display \"F|\" colsof(e(b)) \"|\" rowsof(e(V)) \"|\" el(e(V),8,8) \"|\"\n"
        "estimates store dg\ndrop d g\nquietly regress y x1\nestimates restore dg\n"
        "matrix Brestored = e(b)\nmatrix Vrestored = e(V)\n"
        "coefplot dg, keep(1.d#*.g) omitted baselevels generate(cp_) replace nodraw\n"
        "count if !missing(cp_plot)\ndisplay \"CP|\" r(N) \"|\"\n");
    ebCheck(lineStarting(L, "F|") == "F|16|16|0|",
            "two-factor e(b)/e(V) retain all base/omitted cells and zero covariance rows; got `" +
                lineStarting(L, "F|") + "`");
    ebCheck(lineStarting(L, "CP|") == "CP|4|",
            "frozen cf_bls_dg keeps four 1.d#*.g cells even after d/g are dropped; got `" +
                lineStarting(L, "CP|") + "`");
    const auto* b0 = s.eval.findMatrix("Bfull");
    const auto* b1 = s.eval.findMatrix("Brestored");
    const auto* v0 = s.eval.findMatrix("Vfull");
    const auto* v1 = s.eval.findMatrix("Vrestored");
    ebCheck(b0 && b1 && v0 && v1 && b0->colnames == b1->colnames &&
                b0->data == b1->data && v0->colnames == v1->colnames && v0->data == v1->data,
            "stored factorial e(b)/e(V) preserve the fitted stripe and full covariance exactly");
  }

  // The exact dyadic w2l_vce_shape_det frame: previously IV-ML and the
  // underlying clogit fit computed these covariances but matrix reads refused.
  {
    piet::SessionState s(workdir);
    (void)displays(s,
        "clear\nset obs 180\ngenerate id=ceil(_n/6)\ngenerate t=mod(_n-1,6)+1\n"
        "generate x1=mod(_n*7,11)+mod(_n,5)/8\n"
        "generate z1=mod(_n*5,13)+x1/2+mod(_n*17,19)/16\n"
        "generate x2=.5*z1+mod(_n*3,5)+mod(_n*13,7)/8+mod(_n*23,29)/32\n"
        "generate y=1+2*x1-x2+id/4+mod(_n*11,17)/4+"
        "(mod(_n*37,41)/64+mod(_n*53,59)/64+mod(_n*29,31)/64-1)*3\n"
        "generate byte yb=(y+(mod(_n*43,47)/64+mod(_n*61,67)/64+mod(_n*19,23)/64-1.5)*3>8)\n"
        "generate id2=mod(_n,7)+1\n"
        "generate cnt=int((mod(_n*7,11)+mod(_n*13,17)/2+mod(id,4))/4)\n"
        "xtset id t\n");
    struct Fit { const char* cmd; size_t k; double se11; };
    for (const Fit& fit : {
             Fit{"ivprobit yb x1 (x2=z1), vce(cluster id)", 8, .0917955778630873},
             Fit{"ivtobit y x1 (x2=z1), ll(0) vce(cluster id)", 9, .0956912009646633},
             Fit{"xtlogit yb x1 x2, fe cluster()", 2, .6861715223056473}}) {
      (void)displays(s, std::string(fit.cmd) + "\nmatrix MLb=e(b)\nmatrix MLV=e(V)\n");
      const auto* b = s.eval.findMatrix("MLb");
      const auto* V = s.eval.findMatrix("MLV");
      bool good = b && V && b->colnames.size() == fit.k && V->data.size() == fit.k;
      if (good) {
        good = std::fabs(std::sqrt(V->data[0][0]) - fit.se11) < 1e-6 * fit.se11 &&
               std::fabs(V->data[0][1]) > 1e-12;
        for (size_t i = 0; i < fit.k; ++i)
          for (size_t j = 0; j < fit.k; ++j)
            good = good && std::fabs(V->data[i][j] - V->data[j][i]) < 1e-12;
      }
      ebCheck(good, std::string("actual full ML covariance and canonical stripe: ") + fit.cmd +
                        "; fixed w2l se11 pin at iterative-MLE precision, nonzero off-diagonal");
    }
    (void)displays(s,
        "ppmlhdfe cnt x1 x2, absorb(id) vce(cluster id id2)\n"
        "matrix PPV=e(V)\nmatrix PPb=e(b)\n");
    const auto* pv = s.eval.findMatrix("PPV");
    const auto* pb = s.eval.findMatrix("PPb");
    const double se = pv && pv->data.size() == 3 ? std::sqrt(pv->data[0][0]) : std::nan("");
    ebCheck(pv && pb && pb->colnames == std::vector<std::string>({"x1", "x2", "_cons"}) &&
                pv->data.size() == 3 &&
                std::fabs(se - .0077529809719547) <= 1e-9 * .0077529809719547 &&
                std::fabs(pv->data[0][2]) > 1e-10,
            "w2l ppmlhdfe_vce_c2_se: project the full standardized covariance, including "
            "recovered _cons; se[x1]=.0077529809719547, not slope-only .0077106682367076");
  }
  {
    piet::SessionState s(workdir);
    const auto L = displays(s,
        "clear\nset obs 200\n"
        "generate double x1=mod(_n*7,23)/4-2.75+mod(_n,3)/8\n"
        "generate double u=(mod(_n*17,19)/19+mod(_n*43,47)/47-1)*2\n"
        "generate double z=mod(_n*13,29)/7-2\n"
        "generate byte s=(.7*x1+.9*z+.6*u+.2>0)\n"
        "generate double yh=cond(s==1,1+.8*x1+.7*u+(mod(_n*61,67)/67-.5)*1.5,.)\n"
        "heckman yh x1, select(s=x1 z) twostep\n"
        "local cn : colfullnames e(b)\n"
        "display \"HK|`cn'|\" e(N_selected) \"|\" e(N_nonselected) \"|\" e(fconverged) \"|\"\n"
        "display \"HQ|\" (abs(_b[yh:x1]-.8260663512918692)<1e-6) \"|\" "
        "(abs(_b[s:z]-1.542845339735127)<1e-6) \"|\" "
        "(abs(e(selambda)-.1617014046553287)<1e-6) \"|\"\n");
    ebCheck(lineStarting(L, "HK|") ==
                "HK|yh:x1 yh:_cons s:x1 s:z s:_cons /mills:lambda|113|87|1|",
            "Heckman frozen bm_h_d0 canonical equations, selected counts and actual convergence; got `" +
                lineStarting(L, "HK|") + "`");
    ebCheck(lineStarting(L, "HQ|") == "HQ|1|1|1|",
            "Heckman qualified coefficients and selambda expose actual two-step fit; got `" +
                lineStarting(L, "HQ|") + "`");
  }

  std::printf("[selftest] W2 ebmat: %d checks\n", gEbmatChecks);
  fs::remove_all(workdir, ec);
}
