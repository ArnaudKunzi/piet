// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `misc` -- selftest arms. Report every assertion through w2check().
//
// piet-el-class-matrix-in-display (FU-118, with FU-178/FU-190/FU-207/FU-324/
// FU-348/FU-418 folded in; BUG-792..794): el()/colsof()/rowsof() of e(b)/e(V)
// in `display`, `scalar =` and `local =`. On the frozen main binary every one
// of these read `NameError: name 'el' is not defined` (display rc 1), because a
// fit that posts no class e(V) -- regress, areg, xtreg fe, logit, xtpoisson fe,
// ivprobit, all six of the step-1 table -- left the operand unsubstituted.
// Oracles: tests/eval/golden/aer_w2/w2l_vce_shape.log (se11 == sex1 and the
// absent-e(V) `type mismatch`), w2p_estim_ev.log (B/G colsof/rowsof),
// w2g_ebmat.log (`x1 x2 _cons` order; S3 scalar), [FN] Matrix functions el()
// and the macro layer's own r(133) mapping (w2c_functions.log:110).
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"

namespace {

struct Run {
  std::string rc = "0";  // the LAST error block's rc (coarseRc), "0" when none
  std::string err;       // that block's message
  std::string text;      // the LAST display block's text
};

Run runOf(piet::SessionState& session, const std::string& cmd) {
  Run r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    const std::string t = b.value("type", std::string());
    if (t == "error") {
      r.rc = piet::eval::coarseRc(b);
      r.err = b.value("message", std::string());
    } else if (t == "display") {
      r.text = b.value("text", std::string());
    }
  }
  return r;
}

std::string shown(piet::SessionState& session, const std::string& cmd) {
  return runOf(session, cmd).text;
}

bool noNameError(const Run& r) { return r.err.find("NameError") == std::string::npos; }

// The FU-118 step-1 panel: 60 obs, 10 ids x 6 periods, deterministic.
void panel(piet::SessionState& session) {
  for (const char* c :
       {"clear", "set obs 60", "generate id = ceil(_n/6)", "generate t = mod(_n-1,6)+1",
        "generate x1 = mod(_n*7,11)/3 + id/10", "generate z1 = mod(_n*5,13)/4 + x1/5",
        "generate x2 = 0.5*z1 + mod(_n*3,5)/2",
        "generate y = 1 + 0.5*x1 - 0.3*x2 + mod(_n*3,7)/5",
        "generate yb = (y + mod(_n*11,7)/3 > 2.6)", "generate cnt = mod(_n*7,5) + (x1 > 2)",
        "generate g = mod(_n,3)+1", "xtset id t"})
    (void)piet::dispatch(c, session);
}

// The fix-round R1 panel: as above, but x2 = mod(id*3,5)+id/8 is constant within
// id, so areg/xtreg fe/reghdfe omit it as collinear with the absorbed effects.
void panelTimeInvariant(piet::SessionState& session) {
  for (const char* c :
       {"clear", "set obs 60", "generate id = ceil(_n/6)", "generate t = mod(_n-1,6)+1",
        "generate x1 = mod(_n*7,11)/3 + id/10", "generate x2 = mod(id*3,5) + id/8",
        "generate x3 = mod(_n*5,13)/4 + x1/5", "generate z1 = mod(_n*11,17)/4 + x1/3",
        "generate y = 1 + 0.5*x1 - 0.3*x2 + 0.2*x3 + mod(_n*3,7)/5",
        "generate yb = (mod(_n*13,7) + x1 > 5)", "generate cnt = mod(_n*7,5) + (x1 > 2)",
        "xtset id t"})
    (void)piet::dispatch(c, session);
}

}  // namespace

void testW2bMisc() {
  std::printf("[selftest] --- W2B misc: el()/colsof()/rowsof() of e(b)/e(V) in display "
              "(FU-118, BUG-792..794) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bmisc").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  panel(session);

  // (1) w2l_vce_shape.log:106/:111 regress_ctl_cluster_se == regress_ctl_cluster_sex1
  // (both .0603201952794689): sqrt(el(e(V),1,1)) prints the text _se[x1] prints,
  // after each estimator of the step-1 table that keeps Stata's column order.
  for (const char* fit : {"regress y x1 x2, cluster(id)", "areg y x1 x2, absorb(id)",
                          "xtreg y x1 x2, fe", "logit yb x1 x2"}) {
    (void)piet::dispatch(std::string("quietly ") + fit, session);
    const std::string se = shown(session, "display %18.0g _se[x1]");
    const Run r = runOf(session, "display \"a\" %18.0g sqrt(el(e(V),1,1)) \"|\"");
    w2check(!se.empty() && r.rc == "0" && r.text == "a" + se + "|",
            std::string("W2B misc (FU-118): after `") + fit +
                "`, `display \"a\" %18.0g sqrt(el(e(V),1,1)) \"|\"` prints the text of "
                "`%18.0g _se[x1]` (w2l_vce_shape.log:106 == :111) -- main: NameError rc 1. Got rc " +
                r.rc + " `" + r.text + "` vs `" + se + "` " + r.err);
  }

  // (2) Stata's order puts _cons LAST (w2g_ebmat.log:230 `x1 x2 _cons`); the feols
  // coeftable has `(intercept)` first (FU-190). el(e(V),3,3) is _cons's variance,
  // el(e(b),1,1) is _b[x1] and el(e(b),1,3) is _b[_cons], bit for bit (%21x).
  (void)piet::dispatch("quietly regress y x1 x2, cluster(id)", session);
  w2check(shown(session, "display %18.0g sqrt(el(e(V),3,3))") ==
                  shown(session, "display %18.0g _se[_cons]") &&
              shown(session, "display %21x el(e(b),1,1)") == shown(session, "display %21x _b[x1]") &&
              shown(session, "display %21x el(e(b),1,3)") ==
                  shown(session, "display %21x _b[_cons]") &&
              !shown(session, "display %21x _b[x1]").empty(),
          "W2B misc (FU-190): e(b)/e(V) operands are in Stata's order, _cons last "
          "(w2g_ebmat.log:230) -- el(e(b),1,1) = _b[x1], el(e(b),1,3) = _b[_cons], "
          "sqrt(el(e(V),3,3)) = _se[_cons]");

  // (3) w2p_estim_ev.log B_regress `...|3|3|` and G_c_fit `colsof(e(b))` 3, G_nc_fit 2.
  w2check(shown(session, "display rowsof(e(V)) \"|\" colsof(e(V)) \"|\" colsof(e(b))") == "3|3|3",
          "W2B misc (FU-178): rowsof(e(V))|colsof(e(V))|colsof(e(b)) after a two-regressor "
          "regress is 3|3|3 (w2p_estim_ev.log B_regress, G_c_fit) -- main: NameError rc 1");
  // eb-order ROUND 3 (FU-1016): `regress ..., noconstant` posts two columns
  // (w2g_ebmat.log:1241 e9_colnames_eb `x1 x2`, :1255 e9_colsof_eb
  // +1.0000000000000X+001; w2p_estim_ev.log G_nc_fit 2). runRegress records
  // `noconstant` in the block, so the table without `_cons` is whole. A table that
  // lost `_cons` to absorbed effects stays refused: arm (13).
  (void)piet::dispatch("quietly regress y x1 x2, noconstant", session);
  {
    const Run r = runOf(session, "display colsof(e(b))");
    const Run n = runOf(session, "local cn_e9 : colnames e(b)");
    const std::string cn = shown(session, "display \"`cn_e9'\"");
    w2check(r.rc == "0" && r.text == "2" && n.rc == "0" && cn == "x1 x2",
            "W2B misc (FU-178 / FU-1016): colsof(e(b)) after `regress y x1 x2, noconstant` is 2 "
            "and `: colnames e(b)` is `x1 x2` (w2g_ebmat.log:1241/:1255). Got rc " + r.rc + " `" +
                r.text + "` " + r.err + " / " + n.rc + " `" + cn + "`");
  }

  // (4) After `regress` the engine sends the fit's full covariance (--emit-v), so
  // el(e(V),1,2) is the real Cov(b_x1, b_x2) (w2g_ebmat.log:365 e1_V12
  // -1.f7d0ccaeed4b1X-00a, never 0). Checked here against the OLS identity
  // Cov(b1,b2) = -Var(b1) * rho12 * sd1/sd2 (two regressors and `_cons`), and
  // against the symmetric cell.
  (void)piet::dispatch("quietly regress y x1 x2", session);
  {
    const Run r = runOf(session, "scalar v12_fu118 = el(e(V),1,2)");
    (void)piet::dispatch("scalar v21_fu118 = el(e(V),2,1)", session);
    (void)piet::dispatch("scalar se1_fu118 = _se[x1]", session);
    (void)piet::dispatch("quietly correlate x1 x2", session);
    (void)piet::dispatch("scalar rho_fu118 = r(rho)", session);
    (void)piet::dispatch("quietly summarize x1", session);
    (void)piet::dispatch("scalar sd1_fu118 = r(sd)", session);
    (void)piet::dispatch("quietly summarize x2", session);
    (void)piet::dispatch("scalar sd2_fu118 = r(sd)", session);
    const std::string ok = shown(
        session,
        "display (v12_fu118 != 0) * (abs(v12_fu118 - v21_fu118) <= 1e-12*abs(v12_fu118)) * (abs(v12_fu118 + "
        "se1_fu118^2*rho_fu118*sd1_fu118/sd2_fu118) <= 1e-9*abs(v12_fu118))");
    w2check(r.rc == "0" && ok == "1",
            "W2B misc (FU-1016): after `regress y x1 x2`, el(e(V),1,2) is the fit's covariance "
            "(w2g_ebmat.log:365 e1_V12 nonzero), = el(e(V),2,1) = -_se[x1]^2*rho*sd1/sd2. Got rc " +
                r.rc + " " + r.err + " check `" + ok + "` v12 `" +
                shown(session, "display %21x v12_fu118") + "`");
  }
  // An off-diagonal cell of an e(V) known only from standard errors (qreg's
  // engine path, vce(robust): the engine sends no covariance, w2v_estripe_census
  // qreg row; the default iid qreg posts its full e(V) since w2j_e3_qreg2) is a
  // named r(198), never the silent 0 of a diagonal synthesis (FU-98). Unpinned rc.
  (void)piet::dispatch("quietly qreg y x1 x2, vce(robust)", session);
  {
    const Run r = runOf(session, "display el(e(V),1,2)");
    w2check(r.rc == "198" && noNameError(r) && r.err.find("e(V)[1,2]") != std::string::npos &&
                r.text.find("0") == std::string::npos,
            "W2B misc (FU-118): after `qreg`, el(e(V),1,2) with no fitted covariance is a named "
            "r(198) naming e(V)[1,2], not 0 and not a NameError. Got rc " + r.rc + " " + r.err);
  }
  (void)piet::dispatch("quietly regress y x1 x2", session);
  // el() outside the matrix is missing ([FN] Matrix functions, el()).
  w2check(shown(session, "display el(e(V),9,9)") == "." &&
              shown(session, "display el(e(b),1,4)") == ".",
          "W2B misc (FU-118): el(e(V),9,9) and el(e(b),1,4) are missing ([FN] el(): "
          "missing if i or j are out of range) -- main: NameError rc 1");

  // (5) FU-348 (w2g_ebmat.log:2642 s3_scalar_assign rc 0, :2655 s3_display
  // +1.8000000000000X+001): `scalar s = colsof(e(b))` stores the NUMBER 3.
  {
    const Run a = runOf(session, "scalar s_s3 = colsof(e(b))");
    const Run r = runOf(session, "scalar r_s3 = rowsof(e(V))");
    const Run e1 = runOf(session, "scalar e1_s3 = el(e(b),1,1)");
    w2check(a.rc == "0" && shown(session, "display %21x s_s3") == "+1.8000000000000X+001" &&
                r.rc == "0" && shown(session, "display r_s3") == "3" && e1.rc == "0" &&
                shown(session, "display %21x e1_s3") == shown(session, "display %21x _b[x1]"),
            "W2B misc (FU-348): `scalar s = colsof(e(b))` stores 3 (w2g_ebmat.log:2655 "
            "+1.8000000000000X+001), rowsof(e(V)) 3, el(e(b),1,1) = _b[x1] -- main stored the "
            "text `colsof(e(b))` and `display s` was a NameError");
  }

  // (6) Commands whose Stata e(b) the coeftable cannot reproduce: refused r(198)
  // by name (nbreg -- Stata's e(b) carries `/lnalpha`, w2v_estripe_census.log:1789
  // esc_nbreg `yn:x1 yn:x2 yn:_cons /lnalpha`, which the engine's table does not;
  // a factor-variable fit -- Stata's base column 1b.g is absent from the engine's
  // table, :220 esc_regress_fv). Never rc 1. xtpoisson fe moved out of this arm
  // (FU-1016): Stata's e(b) is `yn:x1 yn:x2` (:1236 esc_xtpois_fe), which the
  // branch builds (selftest_w2_ebmat arm 13).
  // The factor-variable fit is served now (ME08/EB-BASE): e(b) carries the base
  // column, b 0 (w2g_ebmat.log:452 e2_colnames_eb `x1 1b.g 2.g 3.g _cons`,
  // e2_el_eb_12 0).
  {
    const Run f = runOf(session, "quietly regress y x1 i.g");
    const Run c = runOf(session, "display el(e(b),1,2) \"|\" el(e(V),2,2)");
    w2check(f.rc == "0" && c.rc == "0" && c.text == "0|0",
            "W2B misc (FU-118 / EB-BASE): after `regress y x1 i.g` e(b)/e(V) carry the base "
            "column of g at 0 (w2g_ebmat.log:452). Got fit rc " + f.rc + ", rc " + c.rc + " `" +
                c.text + "` " + c.err);
  }
  for (const char* fit : {"nbreg cnt x1 x2"}) {
    const Run f = runOf(session, std::string("quietly ") + fit);
    const Run r = runOf(session, "display \"a\" %18.0g sqrt(el(e(V),1,1)) \"|\"");
    const Run c = runOf(session, "display colsof(e(b))");
    w2check(f.rc == "0" && r.rc == "198" && c.rc == "198" && noNameError(r) && noNameError(c) &&
                r.err.find("not available in this version of PIET") != std::string::npos,
            std::string("W2B misc (FU-118): after `") + fit +
                "` the e(V)/e(b) operand is a named r(198) refusal (no NameError, never rc 1). "
                "Got fit rc " + f.rc + ", rc " + r.rc + "/" + c.rc + " " + r.err);
  }

  // (7) No e(): w2l_vce_shape.log:280-283 regress_opt_c2_se -- after `ereturn
  // clear` and a failed fit the line prints `se11=` and stops at `type mismatch`,
  // display_rc=109. `scalar t = colsof(e(b))` is refused and creates no scalar.
  (void)piet::dispatch("ereturn clear", session);
  {
    const Run r = runOf(session, "display \"se11=\" %18.0g sqrt(el(e(V),1,1)) \"|\"");
    w2check(r.rc == "109" && r.text == "se11=type mismatch" && noNameError(r),
            "W2B misc (FU-118): with no e(), the _se line prints `se11=type mismatch` at "
            "rc 109 (w2l_vce_shape.log:280/:283). Got rc " + r.rc + " `" + r.text + "`");
    const Run s = runOf(session, "scalar t_none = colsof(e(b))");
    const Run probe = runOf(session, "display scalar(t_none)");
    w2check(s.rc == "109" && probe.rc == "111",
            "W2B misc (FU-348): with no e(), `scalar t = colsof(e(b))` is r(109) and creates "
            "no scalar (display scalar(t) r(111)) -- main stored the text at rc 0. Got rc " +
                s.rc + "/" + probe.rc);
  }

  // (8) CONTROL: a POSTED e(V) is read as posted, off-diagonal included.
  (void)piet::dispatch("matrix pb = (1, 2)", session);
  (void)piet::dispatch("matrix pV = (4, 0.5 \\ 0.5, 9)", session);
  (void)piet::dispatch("matrix colnames pb = a b", session);
  (void)piet::dispatch("matrix colnames pV = a b", session);
  (void)piet::dispatch("matrix rownames pV = a b", session);
  (void)piet::dispatch("ereturn post pb pV", session);
  w2check(shown(session, "display el(e(V),1,2) \"|\" sqrt(el(e(V),2,2)) \"|\" colsof(e(b))") ==
              ".5|3|2",
          "W2B misc CONTROL: a posted e(V) answers its own off-diagonal (.5), diagonal and "
          "dimensions -- unchanged by the coeftable rules");

  // (9) FU-207: a variable subscript inside an expression reads the observation.
  panel(session);
  (void)piet::dispatch("generate double gm = 1/3 if _n<=2", session);
  {
    const Run a = runOf(session, "display missing(gm[5])");
    const Run b = runOf(session, "display (gm[5] >= .)");
    const Run c = runOf(session, "display missing(gm[1])");
    w2check(a.rc == "0" && a.text == "1" && b.rc == "0" && b.text == "1" && c.rc == "0" &&
                c.text == "0",
            "W2B misc (FU-207): missing(g[5]) 1, (g[5] >= .) 1, missing(g[1]) 0 at rc 0 -- "
            "main: r(198) invalid syntax. Got " + a.rc + ":" + a.text + " " + b.rc + ":" +
                b.text + " " + c.rc + ":" + c.text);
  }

  // (9b) R3: `display s[2]+"c"` never echoes `"ab"+c` at rc 0 (bb150603 did).
  // BUG-1419: it is now Stata's own answer, `abc` (the arm held r(111) until a
  // string cell was a string operand in display). A lone `display s[2]` stays `ab`.
  (void)piet::dispatch("generate str3 s = \"ab\"", session);
  {
    const Run a = runOf(session, "display s[2]+\"c\"");
    const Run b = runOf(session, "display s[2]");
    w2check(a.rc == "0" && a.text == "abc" && b.rc == "0" && b.text == "ab",
            "W2B misc (R3): `display s[2]+\"c\"` is abc, as in Stata, never `\"ab\"+c` "
            "at rc 0; `display s[2]` is `ab`. Got rc " + a.rc + " `" + a.text + "` " + a.err +
                " / " + b.rc + " `" + b.text + "`");
  }

  // (10) FU-418: an unknown function in display is r(133) "unknown function
  // NAME()", the code `local x = f(1)` already answers (macros.cpp
  // classifyFailedEqExpr; w2c_functions.log:110), never the raw NameError at rc 1.
  // Only an invented name is pinned: reldif() is a real Stata function.
  {
    const Run b = runOf(session, "display nosuchfn_x(1)");
    const Run c = runOf(session, "display \"x\" nosuchfn_x(1) \"|\"");
    w2check(b.rc == "133" && b.err == "unknown function nosuchfn_x()" && c.rc == "133" &&
                noNameError(c),
            "W2B misc (FU-418): `display nosuchfn_x(1)` is r(133) unknown function -- main: "
            "NameError rc 1. Got " + b.rc + " `" + b.err + "`, " + c.rc);
  }
  // (10b) R4: a display DIRECTIVE is not an unknown function (bb150603 answered
  // `aunknown function _col()` r(133)). name-binding-display-literals closed its
  // gap (FU-984): `_col(#)` moves to column # at rc 0, as StataBE does
  // (tests/eval/golden/aer_w2/w2av_name_binding.log:1073 `PREC|bd_dy_col|0|[ x]|`
  // with x in column 30); bin/main printed `aNameError`.
  {
    const Run d = runOf(session, "display \"a\" _col(10) \"y\"");
    w2check(d.rc == "0" && d.err.empty() && d.text == "a        y",
            "W2B misc (R4): `display \"a\" _col(10) \"y\"` puts y in column 10 at rc 0. Got rc " +
                d.rc + " `" + d.text + "` " + d.err);
  }

  // (11) el() of a bare matrix: out of range is missing ([FN] el()); an unknown
  // name is StataBE's r(111) `NAME not found` (w2f_residuals.log:1261-1267
  // b560_unknown: `NoSuchMatrix not found`, `PREC|b560_unknown|111|1|`).
  (void)piet::dispatch("matrix M2 = (1,2 \\ 3,4)", session);
  {
    const Run out = runOf(session, "display el(M2,3,3)");
    const Run unk = runOf(session, "display el(NoSuchM2,1,1)");
    const Run dim = runOf(session, "display colsof(NoSuchM2)");
    w2check(out.rc == "0" && out.text == "." && shown(session, "display el(M2,2,1)") == "3" &&
                unk.rc == "111" && unk.err == "NoSuchM2 not found" && dim.rc == "111",
            "W2B misc (FU-118): el(M,3,3) of a 2x2 is missing, el(M,2,1) is 3, el()/colsof() "
            "of an unknown matrix are r(111) `NAME not found` (w2f_residuals.log:1267) -- "
            "main: NameError rc 1; bb150603 said `matrix NAME not found`. Got " + out.rc + ":" +
                out.text + " " + unk.rc + " `" + unk.err + "` " + dim.rc);
  }

  // (12) FU-324 CONTROL (already rc 0 on main at 63adbba1, re-measured): after
  // sktest, colsof/rowsof(r(table)) and the combined line print at rc 0.
  (void)piet::dispatch("quietly sktest x1", session);
  {
    const Run a = runOf(session, "display colsof(r(table)) \"|\" rowsof(r(table))");
    const Run b = runOf(session,
                        "display \"E|\" _rc \"|\" %18.0g el(r(table),1,1) \"|\" colsof(r(table))");
    w2check(a.rc == "0" && a.text == "5|1" && b.rc == "0" &&
                b.text == "E|0|                60|5",
            "W2B misc (FU-324) CONTROL: after `sktest x1`, colsof|rowsof(r(table)) is 5|1 and the "
            "combined el()/colsof() line prints at rc 0. Got " + a.rc + " `" + a.text + "` " +
                b.rc + " `" + b.text + "`");
  }

  // (13) R1: after areg / xtreg fe / reghdfe with x2 collinear with the absorbed
  // effects, Stata's e(b) is `x1 o.x2 x3 _cons` (w2d_uncovered.log:3195-3221
  // keeps `_cons` under omission; w2g_ebmat.log e5a/e5r `x1 x2 _cons`). The
  // fitted covariance now carries the recovered intercept and an exact zero
  // row/column for the omitted regressor (FU-986).
  panelTimeInvariant(session);
  for (const char* fit : {"areg y x1 x2 x3, absorb(id)", "xtreg y x1 x2 x3, fe",
                          "reghdfe y x1 x2 x3, absorb(id)"}) {
    const Run f = runOf(session, std::string("quietly ") + fit);
    const auto b = session.eval.resolveEMatrix("b");
    const auto V = session.eval.resolveEMatrix("V");
    const std::vector<std::string> names = {"x1", "o.x2", "x3", "_cons"};
    w2check(f.rc == "0" && b && b->colnames == names && b->data.size() == 1 &&
                b->data[0].size() == 4 && b->data[0][1] == 0.0,
            std::string("W2B misc (R1): ") + fit + " carries x1 o.x2 x3 _cons and zero omitted b");
    bool zeros = V && V->colnames == names && V->rownames == names && V->data.size() == 4;
    if (zeros) {
      for (size_t j = 0; j < 4; ++j)
        zeros = zeros && V->data[1][j] == 0.0 && V->data[j][1] == 0.0;
      zeros = zeros && V->data[3][3] > 0.0;
    }
    const Run aligned = runOf(session,
        "display (el(e(b),1,3)==_b[x3]) (abs(el(e(V),3,3)-_se[x3]^2)<1e-14)");
    w2check(zeros && aligned.rc == "0" && aligned.text == "11",
            std::string("W2B misc (R1): ") + fit +
                " has a full recovered covariance, exact zero omitted row and aligned x3 b/se");
    (void)runOf(session, "estimates store R1saved");
    (void)runOf(session, "quietly regress y x1 x3");
    const Run restored = runOf(session, "estimates restore R1saved");
    const auto rb = session.eval.resolveEMatrix("b");
    const auto rV = session.eval.resolveEMatrix("V");
    w2check(restored.rc == "0" && b && V && rb && rV && b->colnames == rb->colnames &&
                b->data == rb->data && V->colnames == rV->colnames && V->data == rV->data,
            std::string("W2B misc (R1): ") + fit + " stored estimates retain the full omitted b/V stripe");
    (void)runOf(session, "estimates drop R1saved");
  }
  // (13b) The same fits without x2, and every command on macros.cpp's
  // coefficients-only allowlist: colsof(e(b)) is 3 (x1 x3 _cons; ivreghdfe 2,
  // x1 x3 with `_cons` partialled out, w2b_estim.log:1168) and
  // `%18.0g sqrt(el(e(V),1,1))` prints the text of `%18.0g _se[x1]`
  // (w2l_vce_shape.log:106 == :111). main: NameError rc 1.
  for (const char* fit :
       {"regress y x1 x3", "areg y x1 x3, absorb(id)", "xtreg y x1 x3, fe",
        "reghdfe y x1 x3, absorb(id)", "ivregress 2sls y x3 (x1 = z1)",
        "ivreghdfe y x3 (x1 = z1), absorb(id)", "qreg y x1 x3", "ppmlhdfe cnt x1 x3, absorb(id)",
        "logit yb x1 x3", "logistic yb x1 x3", "probit yb x1 x3", "poisson cnt x1 x3",
        // newey needs `force` after `xtset id t` (t repeats across panels: r(198);
        // StataBE 19.5, xtts m5.do S4/S5).
        "newey y x1 x3, lag(1) force", "prais y x1 x3"}) {
    const Run f = runOf(session, std::string("quietly ") + fit);
    const Run c = runOf(session, "display colsof(e(b))");
    const std::string se = shown(session, "display %18.0g _se[x1]");
    const Run v = runOf(session, "display %18.0g sqrt(el(e(V),1,1))");
    const std::string cols = std::string(fit).rfind("ivreghdfe", 0) == 0 ? "2" : "3";
    w2check(f.rc == "0" && c.rc == "0" && c.text == cols && v.rc == "0" && !se.empty() &&
                v.text == se,
            std::string("W2B misc (R1): after `") + fit + "`, colsof(e(b)) is " + cols + " and "
                "sqrt(el(e(V),1,1)) prints `%18.0g _se[x1]`. Got fit rc " + f.rc + " " + f.err +
                ", colsof rc " + c.rc + " `" + c.text + "` " + c.err + ", el rc " + v.rc + " `" +
                v.text + "` vs `" + se + "`");
  }

  fs::remove_all(workdir, ec);
}
