// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane MATRIX (matsort matselrc): checks against
// tests/eval/golden/aer_w2/w2ax_t1_community.log (arms matsort_* / matselrc_*, StataBE 19.5)
// and the lane's own StataBE probe mp1 (scratch lane-zipmat/mp1.log, 2026-10-28). Every check
// names the arm whose record it mirrors; the fixtures repeat the oracle do-file's `matrix`
// lines. Matrices are read back from the session's store (data, row and column stripes) and
// rc through eval::coarseRc, so a check sees what a do-file's `_rc` and `: rownames` see.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gT1MatrixChecks = 0;
void matCheck(bool cond, const std::string& label) {
  ++gT1MatrixChecks;
  w2check(cond, "T1 Matrix " + label);
}

// rc of one dispatched line: the last error block's coarse rc, "0" when none.
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (b.is_object() && b.value("type", std::string()) == "error")
        rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC:") + e.what();
  }
  return rc;
}

const piet::eval::EvalMatrix* mat(piet::SessionState& s, const std::string& name) {
  for (const auto& p : s.eval.matrices)
    if (p.first == name) return &p.second;
  return nullptr;
}

std::string joined(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? " " : "") + v[i];
  return out;
}

// `: rownames` (the part after any `eq:`), as the oracle's display line prints it.
std::string rowNames(piet::SessionState& s, const std::string& name) {
  const piet::eval::EvalMatrix* m = mat(s, name);
  if (m == nullptr) return "<absent>";
  std::vector<std::string> out;
  for (const auto& n : m->rownames) {
    const size_t c = n.find(':');
    out.push_back(c == std::string::npos ? n : n.substr(c + 1));
  }
  return joined(out);
}

double cell(piet::SessionState& s, const std::string& name, size_t r, size_t c) {
  const piet::eval::EvalMatrix* m = mat(s, name);
  if (m == nullptr || r >= m->data.size() || c >= m->data[r].size()) return -999.0;
  return m->data[r][c];
}

std::string column(piet::SessionState& s, const std::string& name, size_t c) {
  const piet::eval::EvalMatrix* m = mat(s, name);
  if (m == nullptr) return "<absent>";
  std::string out;
  for (size_t r = 0; r < m->data.size(); ++r) {
    const double v = m->data[r][c];
    out += (r ? " " : "") + (std::isnan(v) ? std::string(".") : std::to_string(static_cast<long long>(v)));
  }
  return out;
}

void run(piet::SessionState& s, std::initializer_list<const char*> lines) {
  for (const char* l : lines) {
    try {
      piet::dispatch(l, s);
    } catch (const std::exception&) {
    }
  }
}

void matsortArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  run(s, {"matrix t1A = (3, 10 \\ 1, 20 \\ 2, 30 \\ 5, 5)", "matrix rownames t1A = ra rb rc rd",
          "matrix colnames t1A = ca cb"});
  // matsort_down: PREC|matsort_down|0|5|3|2|1|5|rd ra rc rb|ca cb|
  {
    const std::string rc = rcOf(s, "matsort t1A 1 down");
    matCheck(rc == "0" && column(s, "t1A", 0) == "5 3 2 1" && cell(s, "t1A", 0, 1) == 5.0 &&
                 rowNames(s, "t1A") == "rd ra rc rb" && joined(mat(s, "t1A")->colnames) == "ca cb",
             "matsort_down (w2ax_t1_community.log: 5 3 2 1, rd ra rc rb, colnames kept)");
  }
  // matsort_up: PREC|matsort_up|0|5|10|20|30|rd ra rb rc|
  {
    const std::string rc = rcOf(s, "matsort t1A 2 up");
    matCheck(rc == "0" && column(s, "t1A", 1) == "5 10 20 30" && rowNames(s, "t1A") == "rd ra rb rc",
             "matsort_up (column 2 ascending: rd ra rb rc)");
  }
  // matsort_default: PREC|matsort_default|0|1|5|rb rc ra rd|
  {
    const std::string rc = rcOf(s, "matsort t1A 1");
    matCheck(rc == "0" && cell(s, "t1A", 0, 0) == 1.0 && cell(s, "t1A", 3, 0) == 5.0 &&
                 rowNames(s, "t1A") == "rb rc ra rd",
             "matsort_default (column given, direction defaults to up)");
  }
  // matsort_badcol / matsort_nomat: both r(198) ("Invalid column number specified: 3",
  // "t1nosuch is not a matrix").
  matCheck(rcOf(s, "matsort t1A 3 up") == "198" && rcOf(s, "matsort t1nosuch 1 up") == "198",
           "matsort_badcol + matsort_nomat (r(198) both)");
  // matsort_ties: PREC|matsort_ties|0|x z y w| -- the selection-and-swap order, not a stable
  // sort (a stable ascending sort would give x z w y).
  run(s, {"matrix t1T = (2, 1 \\ 1, 2 \\ 2, 3 \\ 1, 4)", "matrix rownames t1T = w x y z"});
  matCheck(rcOf(s, "matsort t1T 1 up") == "0" && rowNames(s, "t1T") == "x z y w",
           "matsort_ties (x z y w: swap order of tied rows)");

  // mp1 rankonly / rankmat: `matsort A 1 up rankonly` leaves A, posts r(rank) = 3 1 2 4 and
  // r(order) = 2 3 1 4, and no user matrix `rank` survives.
  run(s, {"matrix A = (3, 10 \\ 1, 20 \\ 2, 30 \\ 5, 5)", "matrix rownames A = ra rb rc rd",
          "matrix rank = (7)"});
  {
    const std::string rc = rcOf(s, "matsort A 1 up rankonly");
    const piet::eval::EvalMatrix* rk = s.eval.findClassMatrix('r', "rank");
    const piet::eval::EvalMatrix* od = s.eval.findClassMatrix('r', "order");
    bool ok = rc == "0" && rowNames(s, "A") == "ra rb rc rd" && cell(s, "A", 0, 0) == 3.0 &&
              rk != nullptr && od != nullptr && rk->data.size() == 4 && od->data.size() == 4 &&
              mat(s, "rank") == nullptr;
    if (ok) {
      ok = rk->data[0][0] == 3 && rk->data[1][0] == 1 && rk->data[2][0] == 2 && rk->data[3][0] == 4 &&
           od->data[0][0] == 2 && od->data[1][0] == 3 && od->data[2][0] == 1 && od->data[3][0] == 4;
    }
    matCheck(ok, "mp1 rankonly/rankgone (r(rank) 3 1 2 4, r(order) 2 3 1 4, user `rank` moved)");
  }
  // mp1 miss_up: a missing value in an ascending sort is r(503) with the matrix untouched;
  // miss_down: it sorts first (A[1,1] = .), rc 0, rows rc rd ra rb.
  run(s, {"matrix M = (3, 10 \\ 1, 20 \\ . , 30 \\ 5, 5)", "matrix rownames M = ra rb rc rd"});
  {
    const std::string up = rcOf(s, "matsort M 1 up");
    const std::string upNames = rowNames(s, "M");
    const std::string down = rcOf(s, "matsort M 1 down");
    matCheck(up == "503" && upNames == "ra rb rc rd" && down == "0" &&
                 std::isnan(cell(s, "M", 0, 0)) && rowNames(s, "M") == "rc rd ra rb",
             "mp1 miss_up r(503) untouched / miss_down missing first");
  }
  // mp1 eq: equation names stay in position, names move: e1:rb e1:rc e2:ra e2:rd.
  run(s, {"matrix E = (3, 10 \\ 1, 20 \\ 2, 30 \\ 5, 5)",
          "matrix rownames E = e1:ra e1:rb e2:rc e2:rd"});
  matCheck(rcOf(s, "matsort E 1 up") == "0" &&
               joined(mat(s, "E")->rownames) == "e1:rb e1:rc e2:ra e2:rd",
           "mp1 eq (equation stripe stays in place: e1:rb e1:rc e2:ra e2:rd)");
  // mp1 nonnum / baddir / dog: `matsort A down` reads `down` as the column, r(111);
  // `xyz` is r(198); `dog` begins with d, so it sorts down.
  run(s, {"matrix D = (3 \\ 1 \\ 2 \\ 5)"});
  matCheck(rcOf(s, "matsort D down") == "111" && rcOf(s, "matsort D 1 xyz") == "198" &&
               rcOf(s, "matsort D 1 dog") == "0" && column(s, "D", 0) == "5 3 2 1",
           "mp1 nonnum r(111) / baddir r(198) / dog = down");
  // Review st_mat sortcol_expr / sortcol_scalar: the column is an expression -- `1+1` sorts
  // by column 2 (r1 r2 r3), a scalar `sc` = 2 down gives r3 r2 r1; st_mat2 col17: a
  // fractional column truncates in the element subscript (1.7 sorts by column 1: r1 r2);
  // st_mat2 comma: `matsort X 1, d` is r(101) "options not allowed".
  run(s, {"matrix M3 = (3, 1 \\ 1, 2 \\ 2, 3)", "matrix M4 = (3, 1 \\ 1, 2 \\ 2, 3)",
          "scalar sc = 2", "matrix X = (2, 1 \\ 1, 2)"});
  matCheck(rcOf(s, "matsort M3 1+1 u") == "0" && rowNames(s, "M3") == "r1 r2 r3" &&
               rcOf(s, "matsort M4 sc d") == "0" && rowNames(s, "M4") == "r3 r2 r1" &&
               rcOf(s, "matsort X 1.7 d") == "0" && rowNames(s, "X") == "r1 r2" &&
               rcOf(s, "matsort X 1, d") == "101",
           "review st_mat sortcol_expr/scalar, st_mat2 col17 truncates, comma r(101)");
  // Review st_mat2 eb_rankonly: a stored-results matrix sorts under rankonly (rc 0, r(order)
  // posted, nothing written back); with replace it is r(501) (rtable_replace).
  run(s, {"clear", "set obs 6", "generate x = _n", "generate y = 7 - _n + mod(_n, 2)",
          "quietly regress y x"});
  {
    const std::string ro = rcOf(s, "matsort e(b) 1 d rankonly");
    const piet::eval::EvalMatrix* od = s.eval.findClassMatrix('r', "order");
    const bool posted = od != nullptr && od->data.size() == 1;
    const std::string rp = rcOf(s, "matsort e(b) 1 d");
    matCheck(ro == "0" && posted && rp == "501",
             "review st_mat2 eb_rankonly rc 0 (r(order) posted) / replace on e(b) r(501)");
  }
}

void matselrcArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  run(s, {"matrix t1B = (1, 2, 3, 4 \\ 5, 6, 7, 8 \\ 9, 10, 11, 12)",
          "matrix rownames t1B = r1 r2 r3", "matrix colnames t1B = c1 c2 c3 c4"});
  // matselrc_rc: PREC|matselrc_rc|0|2|3|1|8|r1 r2|c1 c3 c4|
  {
    const std::string rc = rcOf(s, "matselrc t1B t1C, r(1/2) c(1, 3/4)");
    const piet::eval::EvalMatrix* c = mat(s, "t1C");
    matCheck(rc == "0" && c != nullptr && c->data.size() == 2 && c->data[0].size() == 3 &&
                 cell(s, "t1C", 0, 0) == 1 && cell(s, "t1C", 1, 2) == 8 &&
                 rowNames(s, "t1C") == "r1 r2" && joined(c->colnames) == "c1 c3 c4",
             "matselrc_rc (r(1/2) c(1, 3/4): 2x3, [2,3]=8, names travel)");
  }
  // matselrc_reorder: PREC|matselrc_reorder|0|3|4|9|c4 c1|
  {
    const std::string rc = rcOf(s, "matselrc t1B t1C, c(4 1)");
    matCheck(rc == "0" && mat(s, "t1C")->data.size() == 3 && cell(s, "t1C", 0, 0) == 4 &&
                 cell(s, "t1C", 2, 1) == 9 && joined(mat(s, "t1C")->colnames) == "c4 c1",
             "matselrc_reorder (c(4 1) reorders: c4 c1)");
  }
  // matselrc_toobig: r(125); matselrc_long: row(3) col(2) = 10; matselrc_copy: no row()/col()
  // is r(198) and leaves t1C as matselrc_long made it (1x1).
  {
    const std::string big = rcOf(s, "matselrc t1B t1C, r(1/99) c(1)");
    const std::string lng = rcOf(s, "matselrc t1B t1C, row(3) col(2)");
    const double v = cell(s, "t1C", 0, 0);
    const std::string cp = rcOf(s, "matselrc t1B t1C");
    matCheck(big == "125" && lng == "0" && v == 10 && cp == "198" &&
                 mat(s, "t1C")->data.size() == 1 && mat(s, "t1C")->data[0].size() == 1,
             "matselrc_toobig r(125) / matselrc_long 10 / matselrc_copy r(198) 1x1 kept");
  }
  // matselrc_names: PREC|matselrc_names|0|2|9|r3 r1|
  matCheck(rcOf(s, "matselrc t1B t1C, r(r3 r1)") == "0" && cell(s, "t1C", 0, 0) == 9 &&
               rowNames(s, "t1C") == "r3 r1",
           "matselrc_names (r(r3 r1) by name, order kept)");
  // mp1 cnames_n / cnum_n / rnum_n: with `names`, the column list is still read as numbers
  // (c3 is r(111), c(2 1) works) and the row list as names (r(2) is r(111)).
  matCheck(rcOf(s, "matselrc t1B t1C, c(c3 c1) names") == "111" &&
               rcOf(s, "matselrc t1B t1C, c(2 1) names") == "0" &&
               joined(mat(s, "t1C")->colnames) == "c2 c1" &&
               rcOf(s, "matselrc t1B t1C, r(2) names") == "111",
           "mp1 names: columns stay numeric, rows become names");
  // mp1 nonint r(126), onename r(198), nosrc r(111), mixed c(1 r2) -> names -> "1 not found".
  matCheck(rcOf(s, "matselrc t1B t1C, c(1.5)") == "126" &&
               rcOf(s, "matselrc t1B, c(1)") == "198" &&
               rcOf(s, "matselrc nosuch t1C, c(1)") == "111" &&
               rcOf(s, "matselrc t1B t1C, c(1 r2)") == "111",
           "mp1 nonint 126 / onename 198 / nosrc 111 / mixed 111");
  // mp1 eqs / eqname / eqonly: full names travel; a bare name does not match `b:y`; `b:`
  // selects the equation.
  run(s, {"matrix D = (1, 2 \\ 3, 4)", "matrix rownames D = a:x b:y",
          "matrix colnames D = p:u q:v"});
  {
    const std::string r1 = rcOf(s, "matselrc D E, r(2) c(2 1)");
    const bool full = mat(s, "E") != nullptr && joined(mat(s, "E")->rownames) == "b:y" &&
                      joined(mat(s, "E")->colnames) == "q:v p:u";
    // `r(y a:x)`: the bare `y` fails first, as mp1's `r(y)` did. The one-word spelling
    // `r(y)` cannot reach the handler in this build: expandMacros' bare r(NAME) pass
    // (eval/macros.cpp) rewrites it to the missing value `.` first -- see the skip below.
    const std::string bare = rcOf(s, "matselrc D E, r(y a:x)");
    const std::string eqo = rcOf(s, "matselrc D E, r(b:)");
    matCheck(r1 == "0" && full && bare == "111" && eqo == "0" &&
                 joined(mat(s, "E")->rownames) == "b:y",
             "mp1 eqs/eqname/eqonly (b:y, q:v p:u; bare y r(111); b: selects)");
  }
  // One-name row list: `matselrc t1B t1C, r(r3)` selects row r3 (StataBE 2026-10-28,
  // evidence/t1_matselrc_onename.log `M|onename|0|1|9|r3|`). The macro pass used to turn
  // `r(r3)` into `r(.)` first; isEstaddLine now leaves matselrc lines alone.
  matCheck(rcOf(s, "matselrc t1B t1C, r(r3)") == "0" && mat(s, "t1C") != nullptr &&
               mat(s, "t1C")->data.size() == 1 && cell(s, "t1C", 0, 0) == 9 &&
               joined(mat(s, "t1C")->rownames) == "r3",
           "matselrc one-name row list r(r3) (rc 0, 1 row r3, [1,1] = 9)");
  // Review st_mat2 names_frac / names_slash: with `names` a column word is a subscript
  // EXPRESSION, and the subscript rounds -- c(2.7) is column 3, c(1/2) is 0.5, column 1.
  run(s, {"matrix B9 = (1, 2, 3 \\ 4, 5, 6 \\ 7, 8, 9)"});
  {
    const std::string frac = rcOf(s, "matselrc B9 C9, c(2.7) names");
    const bool three = cell(s, "C9", 0, 0) == 3 && mat(s, "C9")->data[0].size() == 1;
    const std::string half = rcOf(s, "matselrc B9 C9, c(1/2) names");
    matCheck(frac == "0" && three && half == "0" && cell(s, "C9", 0, 0) == 1 &&
                 joined(mat(s, "C9")->colnames) == "c1",
             "review st_mat2 names_frac (2.7 -> column 3) / names_slash (1/2 -> column 1)");
  }
  // Review st_mat dot / hex / to / exp / plus: `r(.)` is r(127) (missing values), `r(0x2)` is
  // a name ("0x2 not found", r(111)), `1 2 3 to 3` is no numlist (names: r(111)), `1e0` and
  // `+2` are numbers.
  matCheck(rcOf(s, "matselrc B9 C9, r(.)") == "127" &&
               rcOf(s, "matselrc B9 C9, r(0x2)") == "111" &&
               rcOf(s, "matselrc B9 C9, r(1 2 3 to 3)") == "111" &&
               rcOf(s, "matselrc B9 C9, r(1e0)") == "0" && cell(s, "C9", 0, 0) == 1 &&
               rcOf(s, "matselrc B9 C9, r(+2)") == "0" && cell(s, "C9", 0, 0) == 4,
           "review st_mat dot r(127) / hex r(111) / to r(111) / 1e0 / +2");
  // Review st_mat transpose_src: the source may be `B'`.
  matCheck(rcOf(s, "matselrc B9' C9, r(1)") == "0" && cell(s, "C9", 0, 1) == 4,
           "review st_mat transpose_src (B9' row 1 = 1 4 7)");
  // matselrc_inplace: PREC|matselrc_inplace|0|1|6|
  matCheck(rcOf(s, "matselrc t1B t1B, c(2)") == "0" && mat(s, "t1B")->data[0].size() == 1 &&
               cell(s, "t1B", 1, 0) == 6,
           "matselrc_inplace (dst = src: 1 column, [2,1] = 6)");
}

}  // namespace

void testW2T1Matrix() {
  std::printf("[selftest] --- T1 Matrix: matsort matselrc (w2ax_t1_community.log, mp1) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_t1matrix").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  matsortArms(workdir);
  matselrcArms(workdir);
  fs::remove_all(workdir, ec);
  std::printf("[selftest] T1 Matrix: %d checks\n", gT1MatrixChecks);
}
