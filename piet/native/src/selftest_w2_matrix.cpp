// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `matrix` -- the `matrix define`-path cluster (BUG-540..543):
// `.`/`.a`-`.z` in a literal, the literal-parser-fails-so-fall-through-to-
// evalMatrixRhs path, `matrix define`/`mat def`, and submatrix extraction
// (`M[r1..r2,c1..c2]`, open ranges, name subscripts). Oracle: StataBE 19.5
// captures tests/eval/golden/aer_w2/w2_openbugs.log (arms lit_dot,
// lit_extmissing, mixed_name_dot, matdef_nested_literals,
// join_dot_nonconformable, rowjoin_submatrix) and w2_smalls.log section F
// (sub_range, sub_row_open, sub_col_open, sub_names, sub_in_join,
// sub_out_of_range, sub_scalar). The w2c section at the end covers
// piet-matrix-expressions (BUG-584..587) against w2c_functions.log and
// w2f_residuals.log.
#include "selftest_w2.h"

#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"

namespace {

// rc of the LAST error block `cmd` produced, or "0" if it produced none --
// same shape as W1b's runOf() (selftest.cpp's testW1bVceAbbrev), which reads
// `_rc` through piet::eval::coarseRc rather than assuming a boolean collapse.
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object() || b.value("type", std::string()) != "error") continue;
    rc = piet::eval::coarseRc(b);
  }
  return rc;
}

// The text of the LAST `display` block `cmd` produced.
std::string displayOf(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display") {
      text = b.value("text", std::string());
    }
  }
  return text;
}

}  // namespace

void testW2Matrix() {
  std::printf("[selftest] --- W2 matrix: the `matrix define`-path cluster "
              "(w2_openbugs.log, w2_smalls.log sec. F) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2matrix").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // --- oracle arm lit_dot: a `.` cell in a plain literal is accepted, rc 0,
  // and stays a genuine Stata missing (not merely absorbed as 0). Old
  // behavior: parseMatrixLiteral's std::stod("."), threw -> "malformed
  // literal", rc 1. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    const std::string rc = rcOf(session, "matrix M = (1.5, 2.5 \\ ., . \\ 4.5, 5.5)");
    w2check(rc == "0",
            "W2(matrix): lit_dot -- `matrix M = (1.5, 2.5 \\ ., . \\ 4.5, 5.5)` runs (oracle "
            "w2_openbugs.log PREC|lit_dot|0|). Got rc " + rc);
    w2check(displayOf(session, "display (M[1,1]==1.5)") == "1",
            "W2(matrix): lit_dot -- M[1,1] is the plain 1.5 cell, untouched by the `.` fix");
    w2check(displayOf(session, "display (M[2,1]==.)") == "1",
            "W2(matrix): lit_dot -- M[2,1] (the `.` cell) reads back as a genuine Stata missing "
            "(oracle cell hex +1.0000000000000X+3ff)");
    w2check(displayOf(session, "display (M[3,2]==5.5)") == "1",
            "W2(matrix): lit_dot -- M[3,2] is the trailing 5.5 cell, past the missing row");
    w2check(displayOf(session, "display rowsof(M)") == "3" &&
                displayOf(session, "display colsof(M)") == "2",
            "W2(matrix): lit_dot -- shape is unchanged by the missing cells (oracle 3|2)");
  }

  // --- oracle arm lit_extmissing: `.a`/`.z` are accepted AND keep their
  // KIND -- `E[1,2]==.a` is true but `E[1,2]==.` is false, so a plain
  // quiet_NaN collapse (indistinguishable kinds) would fail this arm even
  // though rc alone would look fine. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    const std::string rc = rcOf(session, "matrix E = (1, .a \\ .z, 4)");
    w2check(rc == "0",
            "W2(matrix): lit_extmissing -- `matrix E = (1, .a \\ .z, 4)` runs (oracle "
            "PREC|lit_extmissing|0|). Got rc " + rc);
    w2check(displayOf(session, "display (E[1,2]==.a)") == "1",
            "W2(matrix): lit_extmissing -- E[1,2] keeps its `.a` kind (oracle cell 1)");
    w2check(displayOf(session, "display (E[2,1]==.z)") == "1",
            "W2(matrix): lit_extmissing -- E[2,1] keeps its `.z` kind (oracle cell 1)");
    w2check(displayOf(session, "display (E[1,2]==.)") == "0",
            "W2(matrix): lit_extmissing -- ...and `.a` is NOT plain `.` under `==` (oracle cell "
            "0) -- a quiet_NaN-for-everything storage would answer 1 here instead");
  }

  // --- oracle arm mixed_name_dot (paper 107 shape): a transpose (`A'`) and a
  // bare `.` sit alongside a NAME inside the SAME parenthesised join --
  // exactly what the old "parseMatrixLiteral fails -> hard error" path could
  // never reach past. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix A = (1, 2)", session);
    (void)piet::dispatch("matrix S1 = (7)", session);
    const std::string rc = rcOf(session, "matrix X = (A' , (S1 \\ .))");
    w2check(rc == "0",
            "W2(matrix): mixed_name_dot -- `matrix X = (A' , (S1 \\ .))` runs (oracle "
            "PREC|mixed_name_dot|0|, paper 107's shape). Got rc " + rc);
    w2check(displayOf(session, "display (X[1,1]==1)") == "1" &&
                displayOf(session, "display (X[2,1]==2)") == "1",
            "W2(matrix): mixed_name_dot -- the transposed A' column is (1,2)");
    w2check(displayOf(session, "display (X[1,2]==7)") == "1",
            "W2(matrix): mixed_name_dot -- X[1,2] is S1's own 7 (oracle hex +1.c...X+002 == 7)");
    w2check(displayOf(session, "display (X[2,2]==.)") == "1",
            "W2(matrix): mixed_name_dot -- X[2,2] is the bare `.` operand, still missing");
  }

  // --- oracle arm matdef_nested_literals (paper 047 shape) + the `mat def`/
  // `matrix define` keyword (BUG-543): a nested all-missing literal joined
  // with a NAME and another literal, spelled with the `def` abbreviation. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix CEN = (1, 2, 3)", session);
    const std::string rc = rcOf(session, "mat def SS = ((., ., .) \\ CEN \\ (8, 8, 8))");
    w2check(rc == "0",
            "W2(matrix): matdef_nested_literals -- `mat def SS = ((.,.,.) \\ CEN \\ (8,8,8))` "
            "runs (oracle PREC|matdef_nested_literals|0|, paper 047's shape). Old behavior: `mat "
            "def` was not recognised at all (neither reMat nor reDefine matched the `def` token). "
            "Got rc " + rc);
    w2check(displayOf(session, "display (SS[1,1]==.)") == "1",
            "W2(matrix): matdef_nested_literals -- SS[1,1] is the all-missing row's first cell");
    w2check(displayOf(session, "display (SS[2,3]==3)") == "1",
            "W2(matrix): matdef_nested_literals -- SS[2,3] is CEN's third value (oracle hex "
            "+1.8...X+001 == 3)");
    w2check(displayOf(session, "display rowsof(SS)") == "3",
            "W2(matrix): matdef_nested_literals -- three joined rows (oracle rowsof 3)");
  }

  // --- BUG-543's own ambiguity guard: a matrix literally NAMED `define` must
  // still work as a NAME, not be swallowed as the keyword (there is no
  // second identifier before `=` to make it the keyword form). ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    const std::string rc = rcOf(session, "matrix define = (9, 9)");
    w2check(rc == "0",
            "W2(matrix): a matrix literally NAMED `define` (`matrix define = (9,9)`, no second "
            "identifier before `=`) still defines -- the `def(?:ine)?` keyword strip must not "
            "swallow a real matrix name. Got rc " + rc);
    w2check(displayOf(session, "display (define[1,2]==9)") == "1",
            "W2(matrix): ...and reads back under its own name `define`");
  }

  // --- oracle arm join_dot_nonconformable: `A \ .` (2-col \\ 1-col) is a
  // genuine Stata r(503), not a generic refusal -- the old fallthrough
  // silently discarded evalMatrixRhs's own "not conformable" reason and
  // reported a fixed r(1) via the dead splitMatrixExprTerms leg. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix A = (1, 2)", session);
    const std::string rc = rcOf(session, "matrix Y = A \\ .");
    w2check(rc == "503",
            "W2(matrix): join_dot_nonconformable -- `matrix Y = A \\ .` (1x2 stacked on a 1x1) "
            "is Stata's own r(503) 'conformability error' (oracle "
            "PREC|join_dot_nonconformable|503|). Got rc " + rc);
  }

  // --- submatrix extraction (NEW: unfiled scout item, papers 004/036/040/
  // 073/077/086/089/094/107) -- w2_smalls.log section F's fixture. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix M = (1,2,3,4 \\ 5,6,7,8 \\ 9,10,11,12)", session);
    (void)piet::dispatch("matrix rownames M = ra rb rc", session);
    (void)piet::dispatch("matrix colnames M = ca cb cc cd", session);

    // sub_range: M[1..2,2..4]
    {
      const std::string rc = rcOf(session, "matrix S1 = M[1..2,2..4]");
      w2check(rc == "0",
              "W2(matrix): sub_range -- `M[1..2,2..4]` runs (oracle PREC|sub_range|0|). Got rc " +
                  rc);
      (void)piet::dispatch("local rn : rownames S1", session);
      (void)piet::dispatch("local cn : colnames S1", session);
      w2check(displayOf(session, "display \"`rn'\"") == "ra rb",
              "W2(matrix): sub_range -- row names carried (oracle matrix list S1: ra/rb)");
      w2check(displayOf(session, "display \"`cn'\"") == "cb cc cd",
              "W2(matrix): sub_range -- col names carried (oracle matrix list S1: cb/cc/cd)");
      w2check(displayOf(session, "display (S1[1,1]==2)") == "1" &&
                  displayOf(session, "display (S1[2,3]==8)") == "1",
              "W2(matrix): sub_range -- corner cells match M's own [1,2] and [2,4]");
    }
    // sub_row_open: M[2,1...]
    {
      const std::string rc = rcOf(session, "matrix S2 = M[2,1...]");
      w2check(rc == "0",
              "W2(matrix): sub_row_open -- `M[2,1...]` runs (oracle PREC|sub_row_open|0|). Got "
              "rc " + rc);
      w2check(displayOf(session, "display (S2[1,1]==5)") == "1" &&
                  displayOf(session, "display (S2[1,4]==8)") == "1",
              "W2(matrix): sub_row_open -- open col range reaches the LAST column (oracle "
              "matrix list S2: rb 5 6 7 8)");
    }
    // sub_col_open: M[2...,3]
    {
      const std::string rc = rcOf(session, "matrix S3 = M[2...,3]");
      w2check(rc == "0",
              "W2(matrix): sub_col_open -- `M[2...,3]` runs (oracle PREC|sub_col_open|0|). Got "
              "rc " + rc);
      w2check(displayOf(session, "display (S3[1,1]==7)") == "1" &&
                  displayOf(session, "display (S3[2,1]==11)") == "1",
              "W2(matrix): sub_col_open -- open row range reaches the LAST row (oracle matrix "
              "list S3: rb 7 / rc 11)");
    }
    // sub_names: M["rb","cb".."cc"]
    {
      const std::string rc = rcOf(session, "matrix S4 = M[\"rb\",\"cb\"..\"cc\"]");
      w2check(rc == "0",
              "W2(matrix): sub_names -- `M[\"rb\",\"cb\"..\"cc\"]` runs (oracle "
              "PREC|sub_names|0|). Got rc " + rc);
      w2check(displayOf(session, "display (S4[1,1]==6)") == "1" &&
                  displayOf(session, "display (S4[1,2]==7)") == "1",
              "W2(matrix): sub_names -- name subscripts resolve to the same cells as the "
              "numeric range they alias (oracle matrix list S4: rb 6 7)");
    }
    // sub_in_join: (0,0,0) \ M[1..2,1..3] -- a submatrix inside a join,
    // carrying its OWN names past the concatenation (paper 004's shape).
    {
      const std::string rc = rcOf(session, "matrix S5 = (0,0,0) \\ M[1..2,1..3]");
      w2check(rc == "0",
              "W2(matrix): sub_in_join -- `(0,0,0) \\ M[1..2,1..3]` runs (oracle "
              "PREC|sub_in_join|0|, paper 004's motive). Got rc " + rc);
      (void)piet::dispatch("local rn5 : rownames S5", session);
      w2check(displayOf(session, "display \"`rn5'\"") == "r1 ra rb",
              "W2(matrix): sub_in_join -- the literal's default r1 sits next to the submatrix's "
              "CARRIED names ra/rb, unrenumbered (oracle matrix list S5: r1/ra/rb)");
    }
    // sub_out_of_range: M[1..5,1] -- Stata's own r(503), same as
    // join_dot_nonconformable, on a genuinely out-of-bounds range.
    {
      const std::string rc = rcOf(session, "matrix S6 = M[1..5,1]");
      w2check(rc == "503",
              "W2(matrix): sub_out_of_range -- `M[1..5,1]` (M has 3 rows) is r(503) "
              "'conformability error' (oracle PREC|sub_out_of_range|503|). Got rc " + rc);
    }
    // sub_scalar (load-bearing): M[2,3] -- the pre-existing SCALAR element
    // accessor -- must still answer directly, untouched by the new
    // `NAME[...]` matrix-RHS grammar (a different code path, macros.cpp's
    // substituteMatrixRefs, reached from `display`/`scalar`, not from
    // `matrix NAME = ...`).
    w2check(displayOf(session, "display M[2,3]") == "7",
            "W2(matrix): sub_scalar -- M[2,3] (plain scalar element access) still answers "
            "directly (oracle PREC|sub_scalar|7|...)");
    // sub_scalar (el half, BUG-560): `el(M,3,4)` -- Stata's function form of
    // NAME[i,j], same session, same M. Oracle w2_smalls.log:
    // `PREC|sub_scalar|7|12|` -- the `12` is el(M,3,4) (M's row 3 is
    // 9,10,11,12). Was "NameError: name 'el' is not defined" on the frozen
    // main binary; el() did not exist in ANY form.
    w2check(displayOf(session, "display el(M,3,4)") == "12",
            "W2(matrix): sub_scalar -- `el(M,3,4)` reads the same cell M[3,4] does (oracle "
            "PREC|sub_scalar|7|12|, the `12`)");
  }

  // --- el() function form (BUG-560), papers 058 and 044: Stata's
  // el(NAME,i,j) / el(e(NAME),i,j) / el(r(NAME),i,j), added to macros.cpp's
  // substituteMatrixRefs family alongside NAME[i,j]/colsof/rowsof/colnumb. No
  // oracle arm exists for any of the three below (only sub_scalar's bare
  // form above rests on the oracle) -- each is a design decision, called out
  // inline. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);

    // design decision, no oracle arm: el(r(NAME),i,j) -- paper 044's
    // `el(r(singleresults),1,4)` shape. Built here off `sktest`, the one
    // command in this tree that posts an r-class matrix (r(table)), rather
    // than a matrix invented for the test. `fatalities` is the only bundled
    // dataset (`sysuse auto` is not available in this build).
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("sktest income", session);
    // The reference is a COPY of the posted matrix read with NAME[i,j]; the
    // earlier `display r(table)[1,4]` reference errored with the same text
    // as the el() side, so the equality held while both were broken.
    (void)piet::dispatch("matrix ELT = r(table)", session);
    const std::string elRef = displayOf(session, "display ELT[1,4]");
    (void)piet::dispatch("local F1=el(r(table),1,4)", session);
    w2check(!elRef.empty() && elRef.find("not set") == std::string::npos &&
                displayOf(session, "display el(r(table),1,4)") == elRef &&
                displayOf(session, "display `F1'") == elRef,
            "W2(matrix): el (design decision) -- `el(r(table),1,4)` and paper 044's "
            "`local F1=el(r(NAME),1,4)` read the same cell as a copy's ELT[1,4] (got `" +
                elRef + "`)");

    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix M = (1,2,3,4 \\ 5,6,7,8 \\ 9,10,11,12)", session);

    // design decision, no oracle arm: the `local` form both 058 call sites
    // use -- `local max = el(WaveNum, 1, `z')` -- with the index already an
    // integer literal by the time evalArith sees it, same as the corpus
    // (058's `` `z' `` is a resolved local by then).
    (void)piet::dispatch("local z = 4", session);
    (void)piet::dispatch("local max = el(M,1,`z')", session);
    w2check(displayOf(session, "display \"`max'\"") == "4",
            "W2(matrix): el (design decision) -- `local max = el(M,1,\\`z')` (paper 058's "
            "shape) resolves the already-expanded literal index and reads M[1,4]");

    // design decision, no oracle arm: an unknown matrix name is a named
    // refusal (the text is left unsubstituted so the parser rejects it), not
    // a silent 0 or missing -- same rule readMatrixElement already applies
    // to NAME[i,j].
    const std::string rcUnknown = rcOf(session, "display el(NoSuchMatrix,1,1)");
    w2check(rcUnknown != "0" && displayOf(session, "display el(NoSuchMatrix,1,1)") != "0",
            "W2(matrix): el (design decision) -- `el(NoSuchMatrix,1,1)` is refused (rc " +
                rcUnknown + "), not a silent 0 (unknown matrix name)");
  }

  // --- oracle arm rowjoin_submatrix (BUG-499b half): a submatrix sitting on
  // the RIGHT of a `\` join carries its OWN row names through concatenation,
  // same rule as a plain NAME operand (handlers_m3.cpp's concatMatrices,
  // unchanged by this task) -- this only needed submatrix extraction to
  // exist at all to reach that pre-existing rule. ---
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix L = (1, 2 \\ 3, 4)", session);
    (void)piet::dispatch("matrix E1 = J(1, 2, .)", session);
    const std::string rc = rcOf(session, "matrix P3 = E1 \\ L[1..2, 1..2]");
    w2check(rc == "0",
            "W2(matrix): rowjoin_submatrix -- `E1 \\ L[1..2,1..2]` runs (oracle "
            "PREC|rowjoin_submatrix|0|...). Got rc " + rc);
    (void)piet::dispatch("local rn3 : rownames P3", session);
    w2check(displayOf(session, "display \"`rn3'\"") == "r1 r1 r2",
            "W2(matrix): rowjoin_submatrix -- E1's own r1 next to L's carried r1/r2, "
            "unrenumbered (oracle PREC|rowjoin_submatrix|0|r1 r1 r2|)");
  }

  // ==========================================================================
  // w2c section (piet-matrix-expressions, BUG-584..587): scalar-expression
  // cells in a `matrix X = (...)` literal, I(n)/vecdiag(M), a scalar
  // coefficient, element assignment with evaluated and range-checked
  // subscripts, and refusals that carry an exact rc. Oracle: StataBE 19.5
  // tests/eval/golden/aer_w2/w2c_functions.log and w2f_residuals.log. The
  // fixture mirrors w2c_functions.do:26-29. An arm without a golden record says
  // "design decision, no oracle arm", and an rc no golden records is labelled
  // UNPINNED (FIX-SPEC section 9).
  // ==========================================================================
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("scalar sc = 2", session);
    (void)piet::dispatch("matrix M = (5,1,2 \\ 3,6,4 \\ 7,8,9)", session);
    (void)piet::dispatch("matrix A = (0,0 \\ 0,0)", session);

    // I_n3 (w2c_functions.log:75 `PREC|I_n3|0|3|3|1|0|`).
    {
      const std::string rc = rcOf(session, "matrix X_I3 = I(3)");
      w2check(rc == "0", "W2c(matrix): I_n3 -- `matrix X_I3 = I(3)` is rc 0 (w2c_functions.log:75). "
                         "Got rc " + rc);
      w2check(displayOf(session, "display rowsof(X_I3)") == "3" &&
                  displayOf(session, "display colsof(X_I3)") == "3" &&
                  displayOf(session, "display (X_I3[1,1]==1)") == "1" &&
                  displayOf(session, "display (X_I3[1,2]==0)") == "1" &&
                  displayOf(session, "display (X_I3[3,3]==1)") == "1",
              "W2c(matrix): I_n3 -- 3x3, X_I3[1,1]=1, X_I3[1,2]=0 (w2c_functions.log:75)");
    }
    // I_scaled (w2c_functions.log:84 `PREC|I_scaled|0|2|0|2|`).
    {
      const std::string rc = rcOf(session, "matrix Y_I2 = 2*I(2)");
      w2check(rc == "0", "W2c(matrix): I_scaled -- `matrix Y_I2 = 2*I(2)` is rc 0 "
                         "(w2c_functions.log:84). Got rc " + rc);
      w2check(displayOf(session, "display (Y_I2[1,1]==2)") == "1" &&
                  displayOf(session, "display (Y_I2[1,2]==0)") == "1" &&
                  displayOf(session, "display (Y_I2[2,2]==2)") == "1",
              "W2c(matrix): I_scaled -- cells 2, 0, 2 (w2c_functions.log:84)");
      w2check(rcOf(session, "matrix Y_s = sc*I(2)") == "0" &&
                  displayOf(session, "display (Y_s[2,2]==2)") == "1",
              "W2c(matrix): I_scaled -- a scalar-name coefficient `sc*I(2)` scales the same way "
              "(design decision, no oracle arm)");
      // BUG-1398: a scalar times a matrix needs no parentheses (StataBE 19.5,
      // 2026-11-03 oracle: `matrix Y_bad = 2*M` is rc 0, 10 2 4 \ 6 12 8 \ 14 16
      // 18). This arm used to pin the old grammar's refusal.
      const std::string rcBare = rcOf(session, "matrix Y_bad = 2*M");
      w2check(rcBare == "0" &&
                  displayOf(session, "display (Y_bad[1,1]==10 & Y_bad[3,3]==18)") == "1" &&
                  rcOf(session, "matrix Y_bad2 = M*2") == "0" &&
                  displayOf(session, "display (Y_bad2[2,3]==8)") == "1",
              "W2c(matrix): `matrix Y_bad = 2*M` and `M*2` scale M (StataBE 19.5). Got rc " +
                  rcBare);
    }
    // vecdiag_basic (w2c_functions.log:100 `PREC|vecdiag_basic|0|1|3|6|rownames=r1|
    // colnames=c1 c2 c3|`). The golden reads the stripes with `: rowfullnames`,
    // owned by postest-factor-levels-rtable; this arm reads them with `: rownames`.
    {
      const std::string rc = rcOf(session, "matrix D_vd = vecdiag(M)");
      w2check(rc == "0", "W2c(matrix): vecdiag_basic -- `matrix D_vd = vecdiag(M)` is rc 0 "
                         "(w2c_functions.log:100). Got rc " + rc);
      w2check(displayOf(session, "display rowsof(D_vd)") == "1" &&
                  displayOf(session, "display colsof(D_vd)") == "3" &&
                  displayOf(session, "display (D_vd[1,2]==6)") == "1",
              "W2c(matrix): vecdiag_basic -- 1x3 and D_vd[1,2]=6 (w2c_functions.log:100)");
      (void)piet::dispatch("local dr : rownames D_vd", session);
      (void)piet::dispatch("local dc : colnames D_vd", session);
      w2check(displayOf(session, "display \"`dr'|`dc'\"") == "r1|c1 c2 c3",
              "W2c(matrix): vecdiag_basic -- rownames r1, colnames c1 c2 c3 "
              "(w2c_functions.log:100)");
      // vecdiag_el_local (w2c_functions.log:110 `PREC|vecdiag_el_local|133||`):
      // vecdiag() is NOT a macro-layer function.
      const std::string rcEl = rcOf(session, "local vd_el = el(vecdiag(M),1,3)");
      w2check(rcEl == "133", "W2c(matrix): vecdiag_el_local -- `local x = el(vecdiag(M),1,3)` "
                             "stays r(133) (w2c_functions.log:110). Got rc " + rcEl);
      (void)piet::dispatch("matrix M_rect = (1,2,3 \\ 4,5,6)", session);
      // StataBE 19.5 (2026-11-03 oracle): `vecdiag(NS)` of a 2x3 NS is r(503)
      // `conformability error`. This arm pinned an unmeasured r(505).
      const std::string rcRect = rcOf(session, "matrix D_bad = vecdiag(M_rect)");
      w2check(rcRect == "503",
              "W2c(matrix): vecdiag of a 2x3 matrix is r(503), never a truncated diagonal. "
              "Got rc " + rcRect);
      const std::string rcNone = rcOf(session, "matrix D_none = vecdiag(NoSuchM)");
      w2check(rcNone == "111",
              "W2c(matrix): vecdiag of an unknown matrix (design decision, no oracle arm; "
              "rc 111 UNPINNED). Got rc " + rcNone);
      // BUG-1400: a dimension is rounded to the nearest integer (StataBE 19.5,
      // 2026-10-29 oracle: I(2.5) is 3x3). This arm used to pin a refusal.
      const std::string rcI0 = rcOf(session, "matrix I_bad = I(2.5)");
      w2check(rcI0 == "0" && displayOf(session, "display rowsof(I_bad)*10+colsof(I_bad)") == "33",
              "W2c(matrix): I(2.5) is the 3x3 identity (StataBE 19.5). Got rc " + rcI0);
    }
    // matrix_mixed_literal (w2c_functions.log:160 `PREC|matrix_mixed_literal|0|`).
    {
      const std::string rc =
          rcOf(session, "matrix X_mix = (1+1, sqrt(4) \\ abs(-3), round(2.6))");
      w2check(rc == "0", "W2c(matrix): matrix_mixed_literal -- rc 0 (w2c_functions.log:160). "
                         "Got rc " + rc);
      w2check(displayOf(session, "display (X_mix[1,1]==2)") == "1" &&
                  displayOf(session, "display (X_mix[1,2]==2)") == "1" &&
                  displayOf(session, "display (X_mix[2,1]==3)") == "1" &&
                  displayOf(session, "display (X_mix[2,2]==3)") == "1",
              "W2c(matrix): matrix_mixed_literal -- cells 2, 2, 3, 3 (design decision, no "
              "oracle arm for the values)");
    }
    // el_matrix_lit (w2c_functions.log:180 `PREC|el_matrix_lit|0|`).
    {
      const std::string rc = rcOf(session, "matrix X_el = (el(M,2,3))");
      w2check(rc == "0" && displayOf(session, "display (X_el[1,1]==4)") == "1",
              "W2c(matrix): el_matrix_lit -- `(el(M,2,3))` is rc 0 (w2c_functions.log:180) "
              "and holds M[2,3]=4. Got rc " + rc);
    }
    // The function `_matrix_lit` / `_matrix_elem` arms: rc 0 in both forms
    // (golden lines below), and the cell equals the same call through `scalar`
    // (design decision, no oracle arm: one evaluator serves both contexts).
    struct FnArm {
      const char* arm;
      const char* call;
      const char* litLine;
      const char* elemLine;
    };
    const FnArm fnArms[] = {
        {"abs", "abs(1)", "209", "220"},
        {"colsof", "colsof(M)", "240", "251"},
        {"invnormal", "invnormal(.975)", "320", "331"},
        {"invt", "invt(10,.975)", "369", "380"},
        {"invttail", "invttail(10,.025)", "418", "429"},
        {"max", "max(1,2)", "478", "489"},
        {"min", "min(1,2)", "527", "538"},
        {"normal", "normal(1.96)", "576", "587"},
        {"round", "round(1/3,.01)", "695", "706"},
        {"rowsof", "rowsof(M)", "726", "737"},
        {"scalar", "scalar(sc)", "799", "810"},
        {"sign", "sign(1-3)", "857", "868"},
        {"sqrt", "sqrt(1)", "897", "908"},
        {"ttail", "ttail(10,2)", "1066", "1077"},
    };
    for (const FnArm& f : fnArms) {
      const std::string a = f.arm;
      const std::string rcLit = rcOf(session, "matrix X_" + a + " = (" + f.call + ")");
      w2check(rcLit == "0", "W2c(matrix): " + a + "_matrix_lit -- `matrix X = (" + f.call +
                                ")` is rc 0 (w2c_functions.log:" + f.litLine + "). Got rc " +
                                rcLit);
      (void)piet::dispatch("matrix E_" + a + " = J(1,1,.)", session);
      const std::string rcElem = rcOf(session, "matrix E_" + a + "[1,1] = " + f.call);
      w2check(rcElem == "0", "W2c(matrix): " + a + "_matrix_elem -- `matrix E[1,1] = " + f.call +
                                 "` is rc 0 (w2c_functions.log:" + f.elemLine + "). Got rc " +
                                 rcElem);
      (void)piet::dispatch("scalar s_" + a + " = " + f.call, session);
      w2check(displayOf(session, "display (X_" + a + "[1,1]==s_" + a + ")") == "1" &&
                  displayOf(session, "display (E_" + a + "[1,1]==s_" + a + ")") == "1",
              "W2c(matrix): " + a + " -- the literal cell and the element cell equal `scalar s = " +
                  f.call + "` (design decision, no oracle arm: one evaluator)");
    }
    // invnormal() outside its domain ([FN]: 1e-323 to 1-2^-53) is missing and
    // compares equal to `.`; before the fix it returned NaN, which printed `.`
    // but made `invnormal(2) == .` false (design decision, no oracle arm).
    w2check(displayOf(session, "display (invnormal(2)==.)*10 + (invnormal(0)==.)") == "11",
            "W2c(matrix): `invnormal(2)` and `invnormal(0)` are missing and equal `.`");
    {
      // runiform_matrix_lit/_elem (w2c_functions.log:757/:768): rc 0; the
      // draw is ours (doctrine carve-out), so only its range is checked.
      const std::string rcLit = rcOf(session, "matrix X_runif = (runiform())");
      (void)piet::dispatch("matrix E_runif = J(1,1,.)", session);
      const std::string rcElem = rcOf(session, "matrix E_runif[1,1] = runiform()");
      w2check(rcLit == "0" && rcElem == "0",
              "W2c(matrix): runiform_matrix_lit/_elem -- rc 0 (w2c_functions.log:757, :768). "
              "Got rc " + rcLit + "/" + rcElem);
      w2check(displayOf(session, "display (X_runif[1,1]>=0 & X_runif[1,1]<1 & "
                                 "E_runif[1,1]>=0 & E_runif[1,1]<1)") == "1",
              "W2c(matrix): runiform -- both draws lie in [0,1) (design decision, no oracle "
              "arm)");
    }

    // round_matrix_row (w2f_residuals.log:1303: 3, -2, 1, 2, 2.68, .33 -- Stata's
    // ties-up round, not banker's). Each cell is compared with the literal
    // decimal held in a scalar, not with round() itself.
    {
      const std::string rc = rcOf(session,
                                  "matrix X_rr = (round(2.5), round(-2.5), round(0.5), "
                                  "round(1.5), round(2.675,.01), round(1/3,.01))");
      w2check(rc == "0", "W2c(matrix): round_matrix_row -- rc 0 (w2f_residuals.log:1303). "
                         "Got rc " + rc);
      const char* expect[] = {"3", "-2", "1", "2", "2.68", ".33"};
      for (int k = 0; k < 6; ++k) {
        const std::string j = std::to_string(k + 1);
        (void)piet::dispatch("scalar rr_want" + j + " = " + expect[k], session);
        w2check(displayOf(session, "display (X_rr[1," + j + "]==rr_want" + j + ")") == "1",
                "W2c(matrix): round_matrix_row -- cell " + j + " is " + expect[k] +
                    " (w2f_residuals.log:1303)");
      }
    }
    // round_matrix_cellassign (w2f_residuals.log:1329: 3 and 2).
    {
      (void)piet::dispatch("matrix A_r = J(1,2,0)", session);
      const std::string rc1 = rcOf(session, "matrix A_r[1,1] = round(2.5)");
      const std::string rc2 = rcOf(session, "matrix A_r[1,2] = round(2.5,2)");
      w2check(rc1 == "0" && rc2 == "0" && displayOf(session, "display (A_r[1,1]==3)") == "1" &&
                  displayOf(session, "display (A_r[1,2]==2)") == "1",
              "W2c(matrix): round_matrix_cellassign -- A[1,1]=round(2.5) is 3, "
              "A[1,2]=round(2.5,2) is 2, both rc 0 (w2f_residuals.log:1329). Got rc " + rc1 +
                  "/" + rc2);
    }

    // matassign_* (w2c_functions.log:121, :130, :141, :151), cumulative on A.
    {
      w2check(rcOf(session, "matrix A[1,1] = sc + M[1,1]") == "0" &&
                  displayOf(session, "display (A[1,1]==7)") == "1",
              "W2c(matrix): matassign_expr -- A[1,1] = sc + M[1,1] = 7 (w2c_functions.log:121)");
      w2check(rcOf(session, "matrix A[1,2] = sc") == "0" &&
                  displayOf(session, "display (A[1,2]==2)") == "1",
              "W2c(matrix): matassign_scalar -- A[1,2] = 2 (w2c_functions.log:130)");
      (void)piet::dispatch("local mm = 7", session);
      w2check(rcOf(session, "matrix A[2,1] = `mm'") == "0" &&
                  displayOf(session, "display (A[2,1]==7)") == "1",
              "W2c(matrix): matassign_macro -- A[2,1] = 7 (w2c_functions.log:141)");
      const std::string rcOob = rcOf(session, "matrix A[5,5] = 1");
      w2check(rcOob == "503", "W2c(matrix): matassign_oob -- r(503) (w2c_functions.log:151). "
                              "Got rc " + rcOob);
    }

    // FU-89: `_b[]` cells after an estimation, then a stripe on the result.
    // vecdiag(e(V)) reads the synthesised e(V)'s diagonal, se^2. Design arms,
    // no oracle arm.
    {
      (void)piet::dispatch("clear", session);
      (void)piet::dispatch("set obs 12", session);
      (void)piet::dispatch("generate double x1 = _n*1.5", session);
      (void)piet::dispatch("generate double y = 1 + 0.5*x1 + mod(_n, 4)", session);
      (void)piet::dispatch("quietly regress y x1", session);
      const std::string rc = rcOf(session, "matrix bb = (_b[x1], _b[_cons])");
      w2check(rc == "0" && displayOf(session, "display rowsof(bb)*10+colsof(bb)") == "12" &&
                  displayOf(session, "display (bb[1,1]==_b[x1] & bb[1,2]==_b[_cons])") == "1",
              "W2c(matrix): FU-89 -- `matrix bb = (_b[x1], _b[_cons])` is 1x2 holding _b[x1] "
              "and _b[_cons] (design decision, no oracle arm). Got rc " + rc);
      w2check(rcOf(session, "matrix colnames bb = b1 b0") == "0",
              "W2c(matrix): FU-89 -- `matrix colnames bb = b1 b0` then runs (design decision, "
              "no oracle arm)");
      // The diagonal is compared with `matrix VV = e(V)` (the same synthesis,
      // whose coefficient order is FU-98/FU-139's, not this task's).
      (void)piet::dispatch("matrix VV = e(V)", session);
      const std::string rcV = rcOf(session, "matrix dv = vecdiag(e(V))");
      w2check(rcV == "0" && displayOf(session, "display colsof(dv)") == "2" &&
                  displayOf(session, "display (dv[1,1]==VV[1,1] & dv[1,2]==VV[2,2])") == "1",
              "W2c(matrix): vecdiag(e(V)) -- 1x2 holding e(V)'s diagonal (design decision, no "
              "oracle arm). Got rc " + rcV);
    }
  }

  // Crash class (E1b, E5a): an over-long digit subscript used to throw
  // std::out_of_range out of the handler and end the whole run, even under
  // `capture`. Each is r(503) now and the session goes on. Before the fix these
  // arms abort the selftest binary.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix A = (1,2)", session);
    (void)piet::dispatch("matrix M = (5,1,2 \\ 3,6,4 \\ 7,8,9)", session);
    const std::string rc1 = rcOf(session, "matrix A[99999999999999999999999,1] = 1");
    w2check(rc1 == "503", "W2c(matrix): crash class -- `matrix A[99999999999999999999999,1] = 1` "
                          "is r(503), not a whole-run abort (design decision, no oracle arm). "
                          "Got rc " + rc1);
    // The range form is StataBE's r(503) (2026-11-03 oracle; sub_out_of_range,
    // w2_smalls.log:295, is `M[1..5,1]`). The two-scalar form
    // `M[99999999999999999999999,1]` is one element there, `.` at rc 0.
    const std::string rc2 = rcOf(session, "matrix S = M[1..99999999999999999999999,1]");
    w2check(rc2 == "503", "W2c(matrix): crash class -- `matrix S = "
                          "M[1..99999999999999999999999,1]` is r(503) (StataBE 19.5). Got rc " +
                              rc2);
    const std::string rc2s = rcOf(session, "matrix S = M[99999999999999999999999,1]");
    const std::string s2s = displayOf(session, "display S[1,1]");
    w2check(rc2s == "0" && s2s == ".",
            "W2c(matrix): crash class -- `matrix S = M[99999999999999999999999,1]` is `.` at "
            "rc 0 (StataBE 19.5). Got rc " + rc2s + ", S[1,1] `" + s2s + "`");
    const std::string rc3 = rcOf(session, "matrix A[.,1] = 1");
    w2check(rc3 == "503", "W2c(matrix): crash class -- `matrix A[.,1] = 1` is r(503) (design "
                          "decision, no oracle arm; rc UNPINNED). Got rc " + rc3);
    const std::string rc4 = rcOf(session, "matrix A[0,1] = 1");
    w2check(rc4 == "503", "W2c(matrix): `matrix A[0,1] = 1` is r(503) (design decision, no "
                          "oracle arm; rc UNPINNED). Got rc " + rc4);
    // I()/J() above the largest c(max_matdim) (65,534, [P] creturn) are
    // Stata's r(915) "unable to allocate matrix" ([P] error), checked before
    // any cast or allocation (design decision, no oracle arm; rc UNPINNED).
    const std::string rcJ = rcOf(session, "matrix JH = J(99999999999999999999999,1,0)");
    const std::string rcI = rcOf(session, "matrix IH = I(99999999999999999999999)");
    w2check(rcJ == "915" && rcI == "915",
            "W2c(matrix): crash class -- J()/I() with a 1e23 dimension are r(915), not an "
            "allocation abort (design decision, no oracle arm; rc UNPINNED). Got rc " + rcJ + "/" +
                rcI);
    w2check(rcOf(session, "matrix A[1,2] = 9") == "0" &&
                displayOf(session, "display A[1,2]") == "9",
            "W2c(matrix): crash class -- the session still runs the next command");
  }

  // `matrix input` and the unknown-operation refusal (StataBE 19.5, 2026-11-03
  // oracle): blanks or commas separate a row's numbers, `\` the rows; ragged
  // rows are r(503), an expression r(198); `matrix nosuchsub Q = 1` is r(501)
  // `matrix operation not found`. Both reached a code-less rc-1 refusal.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    const std::string rc = rcOf(session, "matrix input Q = (1 2 \\ 3 .a)");
    const std::string q21 = displayOf(session, "display Q[2,1]");
    const std::string q22 = displayOf(session, "display Q[2,2] == .a");
    w2check(rc == "0" && q21 == "3" && q22 == "1",
            "W2c(matrix): `matrix input Q = (1 2 \\ 3 .a)` is a 2x2 holding 3 and .a in row 2. "
            "Got rc " + rc + ", Q[2,1] `" + q21 + "`, Q[2,2]==.a `" + q22 + "`");
    const std::string rcRagged = rcOf(session, "matrix input Q2 = (1 2 \\ 3)");
    const std::string rcExpr = rcOf(session, "matrix input Q3 = (1+1, 2)");
    const std::string rcOp = rcOf(session, "matrix nosuchsub Q4 = 1");
    w2check(rcRagged == "503" && rcExpr == "198" && rcOp == "501",
            "W2c(matrix): ragged `matrix input` r(503), an expression in it r(198), an unknown "
            "matrix operation r(501). Got rc " + rcRagged + "/" + rcExpr + "/" + rcOp);
  }

  // Subscripts are scalar expressions (073's expanded shapes). Design arms, no
  // oracle arm.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix H = J(10,4,0)", session);
    w2check(rcOf(session, "matrix H[2*3+2+1,4] = 55") == "0" &&
                displayOf(session, "display (H[9,4]==55)") == "1",
            "W2c(matrix): `matrix H[2*3+2+1,4] = 55` lands at H[9,4] (design decision, no oracle "
            "arm)");
    (void)piet::dispatch("scalar rr = 2", session);
    w2check(rcOf(session, "matrix H[rr,1] = 77") == "0" &&
                displayOf(session, "display (H[2,1]==77)") == "1",
            "W2c(matrix): `matrix H[rr,1] = 77` with scalar rr = 2 lands at H[2,1] (design "
            "decision, no oracle arm)");
    (void)piet::dispatch("matrix H[3,2] = 123", session);
    w2check(rcOf(session, "matrix H[2,3] = H[3,2]") == "0" &&
                displayOf(session, "display (H[2,3]==123)") == "1",
            "W2c(matrix): `matrix H[2,3] = H[3,2]` copies 123 (design decision, no oracle arm)");
    w2check(rcOf(session, "matrix H[max(1,2),1] = 6") == "0" &&
                displayOf(session, "display (H[2,1]==6)") == "1",
            "W2c(matrix): `matrix H[max(1,2),1] = 6` lands at H[2,1] (design decision, no "
            "oracle arm)");
  }

  // Refusal rcs: never rc 1. Each rc is a design decision, no oracle arm, and
  // UNPINNED (FIX-SPEC section 9).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix A = (0,0 \\ 0,0)", session);
    const std::string rcLitFn = rcOf(session, "matrix Z = (nosuchfn_q(1))");
    const std::string rcElemFn = rcOf(session, "matrix A[1,1] = nosuchfn_q(1)");
    w2check(rcLitFn == "133" && rcElemFn == "133",
            "W2c(matrix): an unknown function in a literal and in an element RHS is r(133) "
            "in both paths (design decision, no oracle arm; rc UNPINNED). Got rc " + rcLitFn +
                "/" + rcElemFn);
    const std::string rcStr = rcOf(session, "matrix A[1,1] = \"a\"");
    const std::string rcStrLit = rcOf(session, "matrix Z = (\"a\")");
    w2check(rcStr == "109" && rcStrLit == "109",
            "W2c(matrix): a string element RHS and a string literal cell are r(109) (design "
            "decision, no oracle arm; rc UNPINNED). Got rc " + rcStr + "/" + rcStrLit);
    const std::string rcMissing = rcOf(session, "matrix NoSuchTarget[1,1] = 5");
    w2check(rcMissing == "111", "W2c(matrix): a missing target is r(111) (design decision, no "
                                "oracle arm; rc UNPINNED). Got rc " + rcMissing);
    // BUG-1414: a target subscript rounds, halves up (StataBE 19.5,
    // w2q_matrix_edges A05 and 2026-11-03 oracle). This arm used to pin an
    // r(198) refusal (a design decision, no oracle arm).
    const std::string rcFrac = rcOf(session, "matrix A[1.5,1] = 1");
    w2check(rcFrac == "0" && displayOf(session, "display (A[2,1]==1)") == "1",
            "W2c(matrix): `matrix A[1.5,1] = 1` writes A[2,1] (StataBE 19.5). Got rc " + rcFrac);
    const std::string rcName = rcOf(session, "matrix Z = (1, nosuchname_q)");
    w2check(rcName == "111", "W2c(matrix): a bare unknown name in a literal is r(111) (design "
                             "decision, no oracle arm; rc UNPINNED). Got rc " + rcName);
    w2check(displayOf(session, "display (A[1,1]==0)") == "1",
            "W2c(matrix): the refused element writes left A[1,1] untouched");
    // BUG-1398: matrix arithmetic is evaluated inside a group and on an element
    // assignment's right-hand side too (StataBE 19.5, 2026-11-03 oracle: `(-M)`
    // is -5 -1 \ -3 -6, `(A*A)` rc 0, `A[1,1] = -M` writes the 2x2 block). This
    // arm used to pin an r(198) refusal (BUG-584's design decision).
    (void)piet::dispatch("matrix M = (5,1 \\ 3,6)", session);
    const std::string rcNegM = rcOf(session, "matrix NM = (-M)");
    const std::string rcProd = rcOf(session, "matrix PM = (A*A)");
    const std::string rcElemNeg = rcOf(session, "matrix A[1,1] = -M");
    w2check(rcNegM == "0" && rcProd == "0" && rcElemNeg == "0" &&
                displayOf(session, "display (NM[1,2]==-1 & NM[2,1]==-3 & A[2,2]==-6)") == "1",
            "W2c(matrix): `(-M)`, `(A*A)` and `A[1,1] = -M` evaluate (StataBE 19.5). Got rc " +
                rcNegM + "/" + rcProd + "/" + rcElemNeg);
    // BUG-584: `e(b)` at a join position with no estimation in memory is
    // r(301), never rc 1 (design decision, no oracle arm; rc UNPINNED; an absent
    // class-matrix name on the same leg is r(111), also UNPINNED).
    const std::string rcNoEst = rcOf(session, "matrix X = e(b) \\ e(b)");
    w2check(rcNoEst == "301",
            "W2c(matrix): `matrix X = e(b) \\ e(b)` with no estimation results is r(301) (design "
            "decision, no oracle arm; rc UNPINNED). Got rc " + rcNoEst);
  }

  // BUG-587/BUG-586: a leading-zero integer subscript or I() argument is its
  // value, as in Stata (bin/main accepted the subscripts at rc 0). Design arms,
  // no oracle arm.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("matrix A = (0,0 \\ 0,0)", session);
    const std::string rcLead = rcOf(session, "matrix A[01,1] = 5");
    w2check(rcLead == "0" && displayOf(session, "display (A[1,1]==5)") == "1",
            "W2c(matrix): `matrix A[01,1] = 5` writes A[1,1] (design decision, no oracle arm). "
            "Got rc " + rcLead);
    (void)piet::dispatch("local j : display %02.0f 2", session);
    const std::string rcLoc = rcOf(session, "matrix A[`j',2] = 7");
    w2check(rcLoc == "0" && displayOf(session, "display (A[2,2]==7)") == "1",
            "W2c(matrix): a `%02.0f` subscript (`02`) writes A[2,2] (design decision, no oracle "
            "arm). Got rc " + rcLoc);
    const std::string rcI03 = rcOf(session, "matrix I3 = I(03)");
    w2check(rcI03 == "0" && displayOf(session, "display rowsof(I3)*10+colsof(I3)") == "33",
            "W2c(matrix): `matrix I3 = I(03)` is the 3x3 identity (design decision, no oracle "
            "arm). Got rc " + rcI03);
  }

  // Preserved behaviour of main that no oracle pins (FIX-SPEC section 2).
  // Design arms, no oracle arm.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    // N01: scalars and matrices share one namespace ([P] matrix define,
    // "Namespace"): `scalar Bcol = 99` destroys the matrix Bcol, so every
    // later Bcol is the scalar. StataBE 19.5 (2026-11-03 oracle): `matrix list
    // Bcol` r(111); `(Bcol, Bcol)` 1x2 of 99, `(2*Bcol)` 198, `(-Bcol)` -99,
    // `((Bcol+1)*I(2))` 100*I(2), `el(Bcol,2,1)` r(109) `type mismatch`; and
    // the other way round a matrix destroys the scalar. These arms used to pin
    // a design where both lived on and the matrix won.
    (void)piet::dispatch("matrix Bcol = (11 \\ 22)", session);
    (void)piet::dispatch("scalar Bcol = 99", session);
    w2check(rcOf(session, "matrix list Bcol") == "111",
            "W2c(matrix): N01 -- `scalar Bcol = 99` destroys the matrix Bcol: `matrix list "
            "Bcol` is r(111) (StataBE 19.5)");
    w2check(rcOf(session, "matrix C_collide = (Bcol, Bcol)") == "0" &&
                displayOf(session, "display rowsof(C_collide)*10+colsof(C_collide)") == "12" &&
                displayOf(session, "display (C_collide[1,2]==99)") == "1",
            "W2c(matrix): N01 -- `(Bcol, Bcol)` joins the scalar 99 twice (1x2, StataBE 19.5)");
    const std::string rcCoef = rcOf(session, "matrix C_coef = (2*Bcol)");
    w2check(rcCoef == "0" &&
                displayOf(session, "display rowsof(C_coef)*10+colsof(C_coef)") == "11" &&
                displayOf(session, "display (C_coef[1,1]==198)") == "1",
            "W2c(matrix): N01 -- `(2*Bcol)` is the 1x1 198 (StataBE 19.5). Got rc " + rcCoef);
    const std::string rcNeg = rcOf(session, "matrix C_neg = (-Bcol)");
    w2check(rcNeg == "0" && displayOf(session, "display (C_neg[1,1]==-99)") == "1",
            "W2c(matrix): N01 -- `(-Bcol)` is the 1x1 -99 (StataBE 19.5). Got rc " + rcNeg);
    const std::string rcGrp = rcOf(session, "matrix C_grp = ((Bcol+1)*I(2))");
    w2check(rcGrp == "0" &&
                displayOf(session, "display (C_grp[1,1]==100 & C_grp[2,1]==0 & C_grp[2,2]==100)") ==
                    "1",
            "W2c(matrix): N01 -- `((Bcol+1)*I(2))` is 100*I(2) (StataBE 19.5). Got rc " + rcGrp);
    const std::string rcSc = rcOf(session, "matrix C_sc = (scalar(Bcol), el(Bcol,2,1))");
    w2check(rcSc == "109",
            "W2c(matrix): N01 -- `el(Bcol,2,1)` of the scalar Bcol is r(109) type mismatch "
            "(StataBE 19.5). Got rc " + rcSc);
    (void)piet::dispatch("scalar Bnew = 5", session);
    (void)piet::dispatch("matrix Bnew = (1,2)", session);
    w2check(displayOf(session, "display scalar(Bnew)") != "5" &&
                displayOf(session, "display Bnew[1,2]") == "2",
            "W2c(matrix): N01 -- `matrix Bnew = (1,2)` destroys the scalar Bnew (StataBE 19.5: "
            "`display scalar(Bnew)` is r(109))");
    // L02: an element reference is a scalar, so a literal of them is named by
    // position (StataBE 19.5, 2026-11-03 oracle: `(MN[1,1] \ MN[2,1])` is r1 r2
    // x c1). This arm used to pin MN's names ra rb|ca, a design decision.
    (void)piet::dispatch("matrix MN = (1,2 \\ 3,4)", session);
    (void)piet::dispatch("matrix rownames MN = ra rb", session);
    (void)piet::dispatch("matrix colnames MN = ca cb", session);
    w2check(rcOf(session, "matrix L2 = (MN[1,1] \\ MN[2,1])") == "0",
            "W2c(matrix): L02 -- `(MN[1,1] \\ MN[2,1])` runs (StataBE 19.5)");
    (void)piet::dispatch("local l2r : rownames L2", session);
    (void)piet::dispatch("local l2c : colnames L2", session);
    w2check(displayOf(session, "display \"`l2r'|`l2c'\"") == "r1 r2|c1",
            "W2c(matrix): L02 -- the cells are named by position r1 r2|c1 (StataBE 19.5)");
    // A11: a name-subscript RHS goes through block substitution.
    (void)piet::dispatch("matrix T4 = J(4,1,0)", session);
    w2check(rcOf(session, "matrix T4[4,1] = MN[\"rb\",1]") == "0" &&
                displayOf(session, "display (T4[4,1]==3)") == "1",
            "W2c(matrix): A11 -- `T4[4,1] = MN[\"rb\",1]` stores 3 (design decision, no oracle "
            "arm)");
    // A custom stripe survives element assignment.
    (void)piet::dispatch("matrix S2 = (0,0 \\ 0,0)", session);
    (void)piet::dispatch("matrix rownames S2 = x y", session);
    (void)piet::dispatch("matrix S2[2,1] = 5", session);
    (void)piet::dispatch("local s2r : rownames S2", session);
    w2check(displayOf(session, "display \"`s2r'\"") == "x y" &&
                displayOf(session, "display (S2[2,1]==5)") == "1",
            "W2c(matrix): an element write keeps the custom row stripe x y (design decision, no "
            "oracle arm)");
    // The constant-expression literal of w2c_ml_getmata.do:244.
    (void)piet::dispatch("scalar want_rc = 0.1*3 + 0.05", session);
    w2check(rcOf(session, "matrix rchange = (0.1*3 + 0.05)") == "0" &&
                displayOf(session, "display (rchange[1,1]==want_rc)") == "1",
            "W2c(matrix): `matrix rchange = (0.1*3 + 0.05)` is .35 (w2c_ml_getmata.do:244 shape)");
    // A `*` inside a scalar term at a join position keeps arithmetic
    // precedence; it is not read as a scalar coefficient.
    (void)piet::dispatch("matrix J1 = (1, 2)", session);
    w2check(rcOf(session, "matrix JP = (J1, 0.1*3+0.05)") == "0" &&
                displayOf(session, "display (JP[1,3]==want_rc)") == "1",
            "W2c(matrix): `(J1, 0.1*3+0.05)` appends .35, not 0.1*(3+0.05) (design decision, no "
            "oracle arm)");
  }

  fs::remove_all(workdir, ec);
}
