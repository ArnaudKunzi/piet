// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2c lane `macrofns` (BUG-660) -- the PIET macro/arith layer's function-call
// arms of tests/eval/golden/aer_w2/w2c_functions.log: `local x = f(...)` /
// `global` for every function the arm->task map
// (internal note w2c_functions-arms.md) routes to
// piet-macro-layer-functions, PLUS the nine `operators_missing` arms
// (op_disp_*/op_local_*/op_scalar_or) the same map routes here -- Stata's
// bare `!`/`&`/`|` operators, which this task's fix round also had to teach
// pyeval.cpp (see its own Tok::Amp/Tok::Pipe comments).
// Does NOT touch data-context arms (engine-expression-functions), matrix-
// context arms (piet-matrix-expressions), or the shadow CiParser behind
// `count if`.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"

namespace {

// rc of the LAST error block `cmd` produced, or "0" -- same shape as
// selftest_w2_matrix.cpp's rcOf.
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

// The message of the LAST error block `cmd` produced, or "".
std::string errMsgOf(piet::SessionState& session, const std::string& cmd) {
  std::string msg;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") {
      msg = b.value("message", std::string());
    }
  }
  return msg;
}

// Relative-tolerance numeric comparison. Review finding (v2): this used to
// stand in for the oracle TEXT of every t-distribution arm, which hid three
// 16th-digit text mismatches. It is now used only where an arm is NOT an
// exact-text pin: the one listed display mismatch (invttail_macro_second, at
// T1) and the non-oracle identities further down.
bool near(const std::string& text, double want, double rtol = 1e-12) {
  try {
    size_t pos = 0;
    double got = std::stod(text, &pos);
    return pos == text.size() && std::fabs(got - want) <= rtol * std::max(1.0, std::fabs(want));
  } catch (const std::exception&) {
    return false;
  }
}

// The round-trip-exact text `scalar NAME = expr` stored (evalArith keeps full
// precision for scalars), as a double; NaN when absent or unparseable.
double scalarValue(piet::SessionState& session, const std::string& name) {
  const std::string* v = session.eval.scalars.find(name);
  if (v == nullptr) return std::nan("");
  try {
    return std::stod(*v);
  } catch (const std::exception&) {
    return std::nan("");
  }
}

// %.17g text of a double for failure messages (std::to_string prints
// 0.000000 for every value below 5e-7).
std::string g17(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

bool relEq(double a, double b, double rtol) {
  return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= rtol * std::fabs(b);
}

}  // namespace

void testW2cMacroFns() {
  std::printf("[selftest] --- W2c macrofns: the macro-layer function-call arms of "
              "w2c_functions.log (BUG-660) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2cmacrofns").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // abs_macro: local __m_abs = abs(-3.5) -> 3.5, rc 0 (already worked before
  // BUG-660; kept as a plain-arithmetic control alongside the new functions).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_abs = abs(-3.5)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_abs'\"") == "3.5",
            "W2c(macrofns): abs_macro -- `local x = abs(-3.5)` -> 3.5, rc 0 (oracle "
            "PREC|abs_macro|0|3.5|). Got rc " + rc);
  }

  // colsof_macro / rowsof_macro / scalar_macro: already worked before BUG-660
  // (substituteMatrixRefs / the `scalar(NAME)` pre-substitution, both ahead of
  // evalArith) -- kept as regression controls, not new fixes.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("matrix M = (5,1,2 \\ 3,6,4 \\ 7,8,9)", session);
    (void)piet::dispatch("scalar sc = 2", session);
    std::string rc = rcOf(session, "local __m_colsof = colsof(M)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_colsof'\"") == "3",
            "W2c(macrofns): colsof_macro -- `colsof(M)` on a 3x3 -> 3, rc 0 (oracle "
            "PREC|colsof_macro|0|3|). Got rc " + rc);
    rc = rcOf(session, "local __m_rowsof = rowsof(M)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_rowsof'\"") == "3",
            "W2c(macrofns): rowsof_macro -- `rowsof(M)` on a 3x3 -> 3, rc 0 (oracle "
            "PREC|rowsof_macro|0|3|). Got rc " + rc);
    rc = rcOf(session, "local __m_scalar = scalar(sc)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_scalar'\"") == "2",
            "W2c(macrofns): scalar_macro -- `scalar(sc)` with sc==2 -> 2, rc 0 (oracle "
            "PREC|scalar_macro|0|2|). Got rc " + rc);
  }

  // fileexists_present / fileexists_missing: rc 133 before BUG-660 (unknown
  // function). Absolute paths, deliberately sidestepping session.eval.cwd's
  // own sync story (handlers.cpp's `use`/`cd` path) -- the closure's
  // relative-path branch is exercised by the oracle's own relative-name call,
  // not by this selftest.
  {
    piet::SessionState session(workdir);
    const std::string present = (fs::path(workdir) / "w2c_probe.txt").u8string();
    { std::ofstream f(present); f << "x"; }
    const std::string missing = (fs::path(workdir) / "w2c_nosuchfile_9427.txt").u8string();
    std::string rc = rcOf(session, "local __m_fe1 = fileexists(\"" + present + "\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_fe1'\"") == "1",
            "W2c(macrofns): fileexists_present -- an existing path -> 1, rc 0 (oracle "
            "PREC|fileexists_present|0|1|). Got rc " + rc);
    rc = rcOf(session, "local __m_fe2 = fileexists(\"" + missing + "\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_fe2'\"") == "0",
            "W2c(macrofns): fileexists_missing -- a nonexistent path -> 0, rc 0 (oracle "
            "PREC|fileexists_missing|0|0|). Got rc " + rc);
  }

  // group_macro: fn-probe-20260922.json marks `group` "membership": "unknown"
  // -- the captured value (1, for the one tested input) is all this arm
  // claims; see fnGroup's own comment.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_group = group(2)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_group'\"") == "1",
            "W2c(macrofns): group_macro -- `group(2)` -> 1, rc 0 (oracle "
            "PREC|group_macro|0|1|). Got rc " + rc);
  }

  // invt_df1_tailbeyondhalf / invt_macro_second: rc 133 before BUG-660.
  // Exact oracle TEXT (review v2: no near()). invt(1,.6) is the exact
  // quantile at the double inputs (1-.6 = .40000000000000002220 exactly, the
  // Cauchy root .32491969623290624903); the fix-round-2 code stored
  // .3249196962329061.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_invt1 = invt(1,.6)");
    std::string got = displayOf(session, "display \"`__m_invt1'\"");
    w2check(rc == "0" && got == ".3249196962329062",
            "W2c(macrofns): invt_df1_tailbeyondhalf -- `invt(1,.6)` -> .3249196962329062, rc 0 "
            "(oracle PREC|invt_df1_tailbeyondhalf|0|.3249196962329062|). Got rc " + rc +
                " text `" + got + "`");
    rc = rcOf(session, "local __m_invt2 = invt(10,.975)");
    got = displayOf(session, "display \"`__m_invt2'\"");
    w2check(rc == "0" && got == "2.228138851986273",
            "W2c(macrofns): invt_macro_second -- `invt(10,.975)` -> 2.228138851986273, rc 0 "
            "(oracle PREC|invt_macro_second|0|2.228138851986273|). Got rc " + rc + " text `" +
                got + "`");
  }

  // invttail_df1_tailbeyondhalf / invttail_macro_second: rc 133 before
  // BUG-660 (fn-probe F056 / the SPEC's own headline gap).
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_invttail1 = invttail(1,.6)");
    std::string got = displayOf(session, "display \"`__m_invttail1'\"");
    w2check(rc == "0" && got == "-.3249196962329062",
            "W2c(macrofns): invttail_df1_tailbeyondhalf -- `invttail(1,.6)` -> "
            "-.3249196962329062, rc 0 (oracle PREC|invttail_df1_tailbeyondhalf|0|"
            "-.3249196962329062|; fix round 2 stored -.3249196962329064). Got rc " + rc +
                " text `" + got + "`");
    // invttail_macro_second is a FAILING DISPLAY-GATE ARM (review v3: it was
    // a near() PASS). The oracle stores 2.228138851986273; the exact
    // quantile at the double .025 is 2.2281388519862747; this code's root
    // renders as 2.228138851986274 since fix round v4 (conditioning-based
    // ttail form; 2.228138851986275 before). Neither is a half-way rounding flip
    // (TOLERANCE.md section 1), so the text arm is a SKIP line naming the
    // mismatch, never a PASS; it turns into a PASS by itself if the text
    // ever matches. Only the numeric tier (T1, 1e-12) is a PASS, and it is
    // labelled as the numeric tier alone. Stata's own algorithm is not
    // reproducible from its outputs: StataBE 19.5 measured on 44 (df,p)
    // points differs from this code by 0-13 ulp either way, and both are a
    // few ulp from the exact quantile. The reviewer's candidate (evaluate
    // at 1-(1-p)) is refuted by the same run: Stata's invttail(10,1e-10) is
    // 25.46600802169771 while its invt(10,1-1e-10) is 25.46600780801602.
    rc = rcOf(session, "local __m_invttail2 = invttail(10,.025)");
    got = displayOf(session, "display \"`__m_invttail2'\"");
    w2check(rc == "0" && near(got, 2.228138851986273, 1e-12),
            "W2c(macrofns): invttail_macro_second NUMERIC TIER ONLY (T1 1e-12; the display "
            "arm is the separate line below) -- `invttail(10,.025)` ~ 2.228138851986273, rc 0. "
            "Got rc " + rc + " text `" + got + "`");
    const std::string invttailLabel =
        "W2c(macrofns): invttail_macro_second DISPLAY GATE -- oracle "
        "PREC|invttail_macro_second|0|2.228138851986273|";
    if (rc == "0" && got == "2.228138851986273") {
      w2check(true, invttailLabel + " (text matches)");
    } else {
      w2skip(invttailLabel, "NOT PASSED: listed display-gate mismatch, this build stores `" + got +
                                "` rc " + rc + " (not a half-way flip)");
    }
  }

  // Review finding (v2): t-distribution tails and overflow. Not oracle arms
  // (no w2c record exercises these points); each is an identity or a bound
  // the fix-round-2 code failed, measured against that code:
  //  * invt(df,p) == -invttail(df,p) (the t distribution's symmetry): invt
  //    used invttail(df,1-p), and 1-p rounds -- invt(10,1e-10) was off by
  //    4.8e-8 relative, invt(1,1e-12) by 7.8e-5.
  //  * invttail(10,1-1e-6): 1-(1-1e-6) is exact in double, so this must equal
  //    -invttail(10, 1-(1-1e-6)) to the last bit, and -invttail(10,1e-6) to
  //    the precision of the rounded 1-1e-6 (T2).
  //  * ttail(1,1e200) was 0 (t*t overflow; true 3.18e-201) and
  //    invttail(1,1e-300) clamped at 1.34e154 (true 3.18e299).
  //  * invttail(1e8,.025) was off by 3e-8 relative (cecon::betai at large df)
  //    and invttail(1000,1e-100) answered 8.5 (true 23.93); the references
  //    are the 4-term Cornish-Fisher expansion (valid to far below T2 there).
  {
    piet::SessionState session(workdir);
    for (const char* df : {"1", "10"}) {
      for (const char* p : {"1e-6", "1e-10"}) {
        const std::string a = std::string("__t_invt_") + df + "_" + (p[3] == '6' ? "6" : "10");
        const std::string b = a + "_tail";
        (void)piet::dispatch("scalar " + a + " = invt(" + df + "," + p + ")", session);
        (void)piet::dispatch("scalar " + b + " = -invttail(" + df + "," + p + ")", session);
        const double va = scalarValue(session, a), vb = scalarValue(session, b);
        w2check(relEq(va, vb, 1e-15),
                std::string("W2c(macrofns): invt_symmetry -- invt(") + df + "," + p +
                    ") == -invttail(" + df + "," + p + ") to 1e-15 relative. Got " +
                    std::to_string(va) + " vs " + std::to_string(vb));
      }
    }
    (void)piet::dispatch("scalar __t_hi = invttail(10,1-1e-6)", session);
    (void)piet::dispatch("scalar __t_hiexact = -invttail(10,1-(1-1e-6))", session);
    (void)piet::dispatch("scalar __t_lo = -invttail(10,1e-6)", session);
    const double hi = scalarValue(session, "__t_hi");
    w2check(hi == scalarValue(session, "__t_hiexact") &&
                relEq(hi, scalarValue(session, "__t_lo"), 1e-9),
            "W2c(macrofns): invttail_upper_reflection -- invttail(10,1-1e-6) equals "
            "-invttail(10,1-(1-1e-6)) exactly and -invttail(10,1e-6) at T2. Got " +
                std::to_string(hi));
    (void)piet::dispatch("scalar __t_far = ttail(1,1e200)", session);
    const double far = scalarValue(session, "__t_far");
    w2check(far > 0 && relEq(far, 3.183098861837907e-201, 1e-12),
            "W2c(macrofns): ttail_overflow -- ttail(1,1e200) = 1/(pi*1e200) > 0, not 0 "
            "(t*t overflow). Got " + std::to_string(far));
    (void)piet::dispatch("scalar __t_deep = invttail(1,1e-300)", session);
    const double deep = scalarValue(session, "__t_deep");
    w2check(deep > 1e200 && relEq(deep, 3.183098861837907e+299, 1e-12),
            "W2c(macrofns): invttail_no_clamp -- invttail(1,1e-300) = 3.18e299, not clamped "
            "at 1.34e154. Got " + std::to_string(deep));
    (void)piet::dispatch("scalar __t_bigdf = invttail(1e8,.025)", session);
    w2check(relEq(scalarValue(session, "__t_bigdf"), 1.9599640082627658, 1e-12),
            "W2c(macrofns): invttail_large_df -- invttail(1e8,.025) ~ 1.9599640082627658 "
            "(Cornish-Fisher reference; was 1.95996406716, off 3e-8).");
    (void)piet::dispatch("scalar __t_deeptail = invttail(1000,1e-100)", session);
    w2check(relEq(scalarValue(session, "__t_deeptail"), 23.930585561616667, 1e-5),
            "W2c(macrofns): invttail_deep_tail_moderate_df -- invttail(1000,1e-100) ~ 23.93 "
            "(was 8.5: the near-median form cancelled the tail to 0).");
  }

  // max_macro: rc 0 but VALUE "." before BUG-660 -- pyeval.cpp's mMax treats
  // Stata's missing sentinel as +infinity, so it always "won" max() where
  // Stata ignores a missing argument. min_macro (below) already answered
  // right by the same sentinel trick backfiring the other way; both are
  // pinned here so a future change can't silently re-break either.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_max = max(1,2,.)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_max'\"") == "2",
            "W2c(macrofns): max_macro -- `max(1,2,.)` ignores the missing arg -> 2, rc 0 "
            "(oracle PREC|max_macro|0|2|; old behaviour answered `.`). Got rc " + rc);
    rc = rcOf(session, "local __m_min = min(5,2,.)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_min'\"") == "2",
            "W2c(macrofns): min_macro -- `min(5,2,.)` ignores the missing arg -> 2, rc 0 "
            "(oracle PREC|min_macro|0|2|; already worked, now on the explicit Stata-semantics "
            "path instead of the sentinel-ordering accident). Got rc " + rc);
  }

  // normal_macro: rc 133 before BUG-660. Bit-identical to the oracle with the
  // `0.5*(1+erf(z/sqrt2))` formula (see fnNormal's own comment) -- an exact
  // string compare, not `near()`.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_normal = normal(1.96)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_normal'\"") == ".9750021048517796",
            "W2c(macrofns): normal_macro -- `normal(1.96)` -> .9750021048517796, rc 0 (oracle "
            "PREC|normal_macro|0|.9750021048517796|). Got rc " + rc);
  }

  // regexs_macro_standalone: rc 133 before the BUG-660 fix round (regexs()
  // was entirely unregistered). Fix round: regexs(n) now reads the capture
  // groups of the LAST SUCCESSFUL regexm() call in THIS macro/arith
  // evaluator (exprfns.cpp's lastRegexMatch()/fnRegexs) -- the oracle's own
  // sequence sets that state via a data-context `generate ... regexm(...)`
  // call outside this task's manifest, but the arm itself only asserts the
  // macro-layer readback, which is self-contained: a regexm() call in the
  // SAME evaluator (here, `local __junk = regexm(...)`) sets the identical
  // state a data-context regexm() would. Oracle PREC|regexs_macro_standalone|
  // 0|A| -- reproduced here with `regexm("ABC123","A")` matching literal "A".
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __junk = regexm(\"ABC123\",\"A\")");
    w2check(rc == "0", "W2c(macrofns): regexs_macro_standalone setup -- "
                        "regexm(\"ABC123\",\"A\") must match. Got rc " + rc);
    rc = rcOf(session, "local __m_regexs = regexs(0)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_regexs'\"") == "A",
            "W2c(macrofns): regexs_macro_standalone -- `regexs(0)` after a matching "
            "regexm() call -> A, rc 0 (oracle PREC|regexs_macro_standalone|0|A|; old "
            "behaviour was r(133) unknown function). Got rc " + rc);
  }

  // reverse_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_rev = reverse(\"abc\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_rev'\"") == "cba",
            "W2c(macrofns): reverse_macro -- `reverse(\"abc\")` -> cba, rc 0 (oracle "
            "PREC|reverse_macro|0|cba|). Got rc " + rc);
  }

  // round_macro: rc 198 before BUG-660 (the SPEC's own headline gap --
  // pyeval.cpp's mRound is Python round(x,ndigits): ndigits must be an int,
  // and .01 raised TypeError -> the generic r(198) invalid-syntax fallback).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_round = round(1/3,.01)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_round'\"") == ".33",
            "W2c(macrofns): round_macro -- `round(1/3,.01)` -> .33, rc 0 (oracle "
            "PREC|round_macro|0|.33|; old behaviour was r(198)). Got rc " + rc);
  }

  // runiform_macro / runiformint_macro: rc 133 before BUG-660 -- the oracle
  // asserts only rc 0 (the drawn value is never printed), so this checks
  // shape (rc 0, and runiformint's draw actually lands in [0,5]) rather than
  // a specific number.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_runif = runiform()");
    w2check(rc == "0", "W2c(macrofns): runiform_macro -- `runiform()` is rc 0 (oracle "
                        "PREC|runiform_macro|0|, value never printed). Got rc " + rc);
    rc = rcOf(session, "local __m_runifint = runiformint(0,5)");
    const std::string drawText = displayOf(session, "display \"`__m_runifint'\"");
    bool inRange = false;
    try {
      int v = std::stoi(drawText);
      inRange = v >= 0 && v <= 5;
    } catch (const std::exception&) {
    }
    w2check(rc == "0" && inRange,
            "W2c(macrofns): runiformint_macro -- `runiformint(0,5)` is rc 0 and draws an "
            "integer in [0,5] (oracle PREC|runiformint_macro|0|, value never printed). Got rc " +
                rc + " draw=`" + drawText + "`");
  }

  // sign_zero / sign_missing / sign_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_sign0 = sign(0)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sign0'\"") == "0",
            "W2c(macrofns): sign_zero -- `sign(0)` -> 0, rc 0 (oracle PREC|sign_zero|0|0|). "
            "Got rc " + rc);
    rc = rcOf(session, "local __m_signmiss = sign(.)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_signmiss'\"") == ".",
            "W2c(macrofns): sign_missing -- `sign(.)` -> . (missing propagates), rc 0 (oracle "
            "PREC|sign_missing|0|.|). Got rc " + rc);
    rc = rcOf(session, "local __m_sign = sign(1-3)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sign'\"") == "-1",
            "W2c(macrofns): sign_macro -- `sign(1-3)` -> -1, rc 0 (oracle "
            "PREC|sign_macro|0|-1|). Got rc " + rc);
  }

  // sqrt_macro: rc 0 but VALUE "1.4142135623730951" (17 sig figs, Python's
  // shortest round-trip repr) before BUG-660 -- Stata's own local-macro
  // real-to-text conversion caps at 16 (stataLocalRealText). Regression
  // control for the formatting half of BUG-660, not the function-registry
  // half (sqrt() itself already worked).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_sqrt = sqrt(2)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sqrt'\"") == "1.414213562373095",
            "W2c(macrofns): sqrt_macro -- `sqrt(2)` -> 1.414213562373095 (16 sig figs), rc 0 "
            "(oracle PREC|sqrt_macro|0|1.414213562373095|; old behaviour printed 17). Got rc " +
                rc);
  }

  // strmatch_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_sm = strmatch(\"abc\",\"a?c\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sm'\"") == "1",
            "W2c(macrofns): strmatch_macro -- `strmatch(\"abc\",\"a?c\")` -> 1, rc 0 (oracle "
            "PREC|strmatch_macro|0|1|). Got rc " + rc);
  }

  // strofreal_macro: rc 133 before BUG-660 (aliased to fnString, the same
  // function `string(x,fmt)` already used).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_sor = strofreal(3.14159,\"%5.2f\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sor'\"") == "3.14",
            "W2c(macrofns): strofreal_macro -- `strofreal(3.14159,\"%5.2f\")` -> 3.14, rc 0 "
            "(oracle PREC|strofreal_macro|0|3.14|). Got rc " + rc);
  }

  // strproper_macro: rc 133 before BUG-660 (aliased to fnProper).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_sp = strproper(\"  Ab  cD \")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sp'\"") == "  Ab  Cd ",
            "W2c(macrofns): strproper_macro -- `strproper(\"  Ab  cD \")` -> `  Ab  Cd ` "
            "(rc 0, oracle PREC|strproper_macro|0|  Ab  Cd |). Got rc " + rc);
  }

  // strrpos_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_srp = strrpos(\"  Ab  cD \",\"c\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_srp'\"") == "7",
            "W2c(macrofns): strrpos_macro -- `strrpos(\"  Ab  cD \",\"c\")` -> 7, rc 0 (oracle "
            "PREC|strrpos_macro|0|7|). Got rc " + rc);
  }

  // strtoname_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_stn = strtoname(\"  Ab  cD \")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_stn'\"") == "__Ab__cD_",
            "W2c(macrofns): strtoname_macro -- `strtoname(\"  Ab  cD \")` -> __Ab__cD_, rc 0 "
            "(oracle PREC|strtoname_macro|0|__Ab__cD_|). Got rc " + rc);
  }

  // sum_macro: rc 133 before BUG-660. `sum(1)` is NOT the running-sum
  // aggregate (that's sum_data_running, an engine data-context arm outside
  // this manifest) -- outside a row plane a single evaluation just answers
  // its argument (fnSum's own comment).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_sum = sum(1)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_sum'\"") == "1",
            "W2c(macrofns): sum_macro -- `sum(1)` -> 1, rc 0 (oracle PREC|sum_macro|0|1|). "
            "Got rc " + rc);
  }

  // ttail_df1_tailbeyondhalf / ttail_macro_second: rc 133 before BUG-660.
  // Exact oracle TEXT (review v2). Both are Stata's own last-digit values,
  // not the exact ones (ttail(1,-1) is exactly .75; ttail(10,2) is
  // .0366940173853701828), and this build reproduces both texts.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_ttail1 = ttail(1,-1)");
    std::string got = displayOf(session, "display \"`__m_ttail1'\"");
    w2check(rc == "0" && got == ".7500000000000002",
            "W2c(macrofns): ttail_df1_tailbeyondhalf -- `ttail(1,-1)` -> .7500000000000002, "
            "rc 0 (oracle PREC|ttail_df1_tailbeyondhalf|0|.7500000000000002|). Got rc " + rc +
                " text `" + got + "`");
    rc = rcOf(session, "local __m_ttail2 = ttail(10,2)");
    got = displayOf(session, "display \"`__m_ttail2'\"");
    w2check(rc == "0" && got == ".0366940173853702",
            "W2c(macrofns): ttail_macro_second -- `ttail(10,2)` -> .0366940173853702, rc 0 "
            "(oracle PREC|ttail_macro_second|0|.0366940173853702|). Got rc " + rc + " text `" +
                got + "`");
  }

  // ustrregexra_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc =
        rcOf(session, "local __m_ustrregexra = ustrregexra(\"caf\xC3\xA9\",\"caf\",\"tea\")");
    w2check(rc == "0" &&
                displayOf(session, "display \"`__m_ustrregexra'\"") == "tea\xC3\xA9",
            "W2c(macrofns): ustrregexra_macro -- `ustrregexra(\"café\",\"caf\",\"tea\")` -> "
            "teaé, rc 0 (oracle PREC|ustrregexra_macro|0|teaé|). Got rc " + rc);
  }

  // ustrunescape_macro: rc 133 before BUG-660. `\u201c` is Stata's fixed-
  // width \\uXXXX grammar -- literal backslash-u-2-0-1-c text at this
  // evaluator (BUG-502: no string-literal escape processing), decoded to
  // U+201C ("“", UTF-8 E2 80 9C) only inside ustrunescape() itself.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_uni = ustrunescape(\"\\u201c\")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_uni'\"") == "\xE2\x80\x9C",
            "W2c(macrofns): ustrunescape_macro -- `ustrunescape(\"\\u201c\")` -> U+201C, rc 0 "
            "(oracle PREC|ustrunescape_macro|0|\xE2\x80\x9C|). Got rc " + rc);
  }

  // usubinstr_macro: rc 133 before BUG-660 (aliased to fnSubinstr; a missing
  // `.` count arg overflows int() inside fnSubinstr and lands on its own
  // count=-1 "replace all" fallback -- see BUG-660's registration comment).
  {
    piet::SessionState session(workdir);
    const std::string rc =
        rcOf(session, "local __m_usi = usubinstr(\"  Ab  cD \",\"b\",\"z\",.)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_usi'\"") == "  Az  cD ",
            "W2c(macrofns): usubinstr_macro -- `usubinstr(\"  Ab  cD \",\"b\",\"z\",.)` -> "
            "`  Az  cD `, rc 0 (oracle PREC|usubinstr_macro|0|  Az  cD |). Got rc " + rc);
  }

  // wordcount_macro: rc 133 before BUG-660.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local __m_wc = wordcount(\"  Ab  cD \")");
    w2check(rc == "0" && displayOf(session, "display \"`__m_wc'\"") == "2",
            "W2c(macrofns): wordcount_macro -- `wordcount(\"  Ab  cD \")` -> 2, rc 0 (oracle "
            "PREC|wordcount_macro|0|2|). Got rc " + rc);
  }

  // op_disp_not_dot / op_disp_dot_and1 / op_disp_dot_and0 / op_disp_dot_or0 /
  // op_disp_0_or_dot / op_disp_dota_and1: the `operators_missing` arms this
  // same arm->task map also routes here (w2c_functions-arms.md). Stata's
  // bare `!`/`&`/`|` treat ANY missing value as TRUE (Stata represents
  // missing internally as a large positive number) and always coerce their
  // result to a real 0/1 -- a rule this evaluator did not even PARSE before
  // this fix round (pyeval.cpp's lexer failed `!` outright and had no token
  // at all for `&`/`|`; see its own Tok::Amp/Tok::Pipe comments).
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "display !.");
    w2check(rc == "0" && displayOf(session, "display !.") == "0",
            "W2c(macrofns): op_disp_not_dot -- `!.` -> 0, rc 0 (oracle "
            "PREC|op_disp_not_dot|0). Got rc " + rc);
    rc = rcOf(session, "display . & 1");
    w2check(rc == "0" && displayOf(session, "display . & 1") == "1",
            "W2c(macrofns): op_disp_dot_and1 -- `. & 1` -> 1, rc 0 (oracle "
            "PREC|op_disp_dot_and1|1). Got rc " + rc);
    rc = rcOf(session, "display . & 0");
    w2check(rc == "0" && displayOf(session, "display . & 0") == "0",
            "W2c(macrofns): op_disp_dot_and0 -- `. & 0` -> 0, rc 0 (oracle "
            "PREC|op_disp_dot_and0|0). Got rc " + rc);
    rc = rcOf(session, "display . | 0");
    w2check(rc == "0" && displayOf(session, "display . | 0") == "1",
            "W2c(macrofns): op_disp_dot_or0 -- `. | 0` -> 1, rc 0 (oracle "
            "PREC|op_disp_dot_or0|1). Got rc " + rc);
    rc = rcOf(session, "display 0 | .");
    w2check(rc == "0" && displayOf(session, "display 0 | .") == "1",
            "W2c(macrofns): op_disp_0_or_dot -- `0 | .` -> 1, rc 0 (oracle "
            "PREC|op_disp_0_or_dot|1). Got rc " + rc);
    rc = rcOf(session, "display .a & 1");
    w2check(rc == "0" && displayOf(session, "display .a & 1") == "1",
            "W2c(macrofns): op_disp_dota_and1 -- `.a & 1` -> 1, rc 0 (oracle "
            "PREC|op_disp_dota_and1|1; kind-extended missing is still "
            "truthy). Got rc " + rc);
  }

  // op_local_not / op_local_and / op_scalar_or: the same three operators
  // reached through `local`/`scalar` STORAGE, not a bare `display` -- the
  // Node::T::Bool coercion (real 0/1, not the Python and/or "deciding
  // operand") has to survive evalArith's isBool short-circuit branch too.
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_opnot = !.");
    w2check(rc == "0" && displayOf(session, "display \"`__m_opnot'\"") == "0",
            "W2c(macrofns): op_local_not -- `local m = !.` -> 0, rc 0 (oracle "
            "PREC|op_local_not|0|0|). Got rc " + rc);
    rc = rcOf(session, "local __m_opand = . & 1");
    w2check(rc == "0" && displayOf(session, "display \"`__m_opand'\"") == "1",
            "W2c(macrofns): op_local_and -- `local n = . & 1` -> 1, rc 0 (oracle "
            "PREC|op_local_and|0|1|). Got rc " + rc);
    rc = rcOf(session, "scalar __m_opscor = . | 0");
    w2check(rc == "0" && displayOf(session, "display __m_opscor") == "1",
            "W2c(macrofns): op_scalar_or -- `scalar s = . | 0` -> 1, rc 0 (oracle "
            "PREC|op_scalar_or|0|1|). Got rc " + rc);
  }

  // Fix round 2 (review finding): bare `!` was lexed as Python's low-
  // precedence `not` (Tok::Not), so `!1 + 1` parsed as `!(1+1)` -> 0 where
  // Stata's `!` binds at unary-minus precedence -- looser than `^`, tighter
  // than `*`/`/`/`+`/`-`/comparisons/`&`/`|`. Not a pinned oracle arm (no
  // w2c_functions.log record exercises `!` against an adjacent operator);
  // verified live against StataBE 19.5: `display !1 + 1` is `1`, `display
  // !2 == 1` is `0`, `display !0^0` is `0` (`^` still binds TIGHTER than `!`
  // -- `!(0^0)` = `!1` = 0, not `(!0)^0` = `1^0` = 1). All three would have
  // failed under the pre-fix-round-2 parser (`!1+1` gave 0, `!2==1` gave 1).
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_bangprec1 = !1 + 1");
    w2check(rc == "0" && displayOf(session, "display \"`__m_bangprec1'\"") == "1",
            "W2c(macrofns): bang_precedence_plus -- `!1 + 1` -> 1, not 0 "
            "(`!` binds tighter than `+`; live-StataBE-verified). Got rc " + rc);
    rc = rcOf(session, "local __m_bangprec2 = !2 == 1");
    w2check(rc == "0" && displayOf(session, "display \"`__m_bangprec2'\"") == "0",
            "W2c(macrofns): bang_precedence_eq -- `!2 == 1` -> 0, not 1 "
            "(`!` binds tighter than `==`; live-StataBE-verified). Got rc " + rc);
    rc = rcOf(session, "local __m_bangprec3 = !0^0");
    w2check(rc == "0" && displayOf(session, "display \"`__m_bangprec3'\"") == "0",
            "W2c(macrofns): bang_precedence_pow -- `!0^0` -> 0, i.e. `!(0^0)` "
            "-- `^` still binds TIGHTER than `!` (live-StataBE-verified: "
            "`0^0` is 1, `!1` is 0). Got rc " + rc);
  }

  // Fix round 2 (review finding): stataTtail/stataInvttail near the median
  // (t^2 < df) used to derive the small tail mass as `1.0 - x` from an
  // x already rounded to double precision near 1.0, losing the answer
  // entirely once t was small enough. `invttail(df,.5)`'s true root is
  // exactly t=0 by symmetry; before this fix round it landed on -1.49e-8
  // (this exact code, measured), now within 1e-9 of 0. `ttail(3,1e-9)`
  // used to be exactly 0.5 (t entirely lost); the correct value is
  // .4999999996324474 (measured against this fixed code, cross-checked by
  // the closed-form t-density-at-0 slope, not a pinned oracle arm).
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_invtmed = invttail(3,.5)");
    w2check(rc == "0" && near(displayOf(session, "display \"`__m_invtmed'\""), 0.0),
            "W2c(macrofns): ttail_median_root -- `invttail(3,.5)` ~ 0 (exact "
            "root by t-distribution symmetry; was -1.49e-8 before this fix "
            "round). Got rc " + rc);
    rc = rcOf(session, "local __m_ttailmed = ttail(3,1e-9)");
    w2check(rc == "0" && near(displayOf(session, "display \"`__m_ttailmed'\""), 0.4999999996324474),
            "W2c(macrofns): ttail_near_median -- `ttail(3,1e-9)` ~ "
            ".4999999996324474 (was exactly .5 before this fix round -- t "
            "entirely lost to `1.0 - x` cancellation). Got rc " + rc);
  }

  // Fix round 2 (review finding): evalMacroExpr's inline `` `=expr' `` form
  // rendered a Stata-function numeric result with Python's round-trip repr
  // (up to 17 significant digits, leading `0.`) instead of Stata's own
  // 16-sig-fig, no-leading-zero convention (stataLocalRealText, already
  // applied to `local`/`global` storage) -- reachable now that
  // postfileStataMathNamespace() (this fix round) added the Stata functions
  // to this surface too. Not a pinned oracle arm (w2c_functions.log's own
  // inline-form arms are all data-variable lookups, not function calls), but
  // this exact inline call would have rendered "0.33" before this addition.
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "display \"`=round(1/3,.01)'\"");
    w2check(rc == "0" && displayOf(session, "display \"`=round(1/3,.01)'\"") == ".33",
            "W2c(macrofns): inline_macro_expr_precision -- `` `=round(1/3,.01)' `` "
            "-> .33 (Stata's own real-storage precision), not \"0.33\" "
            "(Python repr). Got rc " + rc);
  }

  // Fix round 2 (review finding): ustrregexra's optional 4th (case-
  // insensitive) argument -- live-verified against StataBE 19.5:
  // `ustrregexra("aAbB","a","X",1)` -> "XXbB" vs the 3-arg form's "XAbB".
  //
  // Review finding (v2): matching is on CODE POINTS (std::wregex over UTF-32).
  // The fix-round-2 arm here asserted `!= "xxxx"` for ustrregexra("café",".",
  // "x") -- it forbade the correct answer and passed on the byte-wise defect
  // ("xxxxx"). Stata/ICU `.` matches one code point: "xxxx", rc 0 (measured
  // in StataBE 19.5 on the same construct: ustrregexra("é",".","x") = "x",
  // piet-macro-layer-functions probe uregex.log). `é+` must repeat the whole
  // code point ("x", where byte-wise gave "xx").
  {
    piet::SessionState session(workdir);
    std::string rc = rcOf(session, "local __m_ustrregexra_ci = ustrregexra(\"aAbB\",\"a\",\"X\",1)");
    w2check(rc == "0" && displayOf(session, "display \"`__m_ustrregexra_ci'\"") == "XXbB",
            "W2c(macrofns): ustrregexra_case_insensitive -- "
            "`ustrregexra(\"aAbB\",\"a\",\"X\",1)` -> XXbB (live-StataBE-"
            "verified). Got rc " + rc);
    rc = rcOf(session, "local __m_ustrregexra_cp = ustrregexra(\"caf\xC3\xA9\",\".\",\"x\")");
    std::string got = displayOf(session, "display \"`__m_ustrregexra_cp'\"");
    w2check(rc == "0" && got == "xxxx",
            "W2c(macrofns): ustrregexra_codepoint_dot -- `ustrregexra(\"caf\xC3\xA9\",\".\",\"x\")` "
            "-> xxxx (one x per code point), rc 0. Got rc " + rc + " text `" + got + "`");
    rc = rcOf(session, "local __m_ure1 = ustrregexra(\"\xC3\xA9\",\".\",\"x\")");
    got = displayOf(session, "display \"`__m_ure1'\"");
    w2check(rc == "0" && got == "x",
            "W2c(macrofns): ustrregexra_codepoint_measured -- `ustrregexra(\"\xC3\xA9\",\".\","
            "\"x\")` -> x (StataBE 19.5, uregex.log). Got rc " + rc + " text `" + got + "`");
    rc = rcOf(session, "local __m_ure2 = ustrregexra(\"\xC3\xA9\xC3\xA9\",\"\xC3\xA9+\",\"x\")");
    got = displayOf(session, "display \"`__m_ure2'\"");
    w2check(rc == "0" && got == "x",
            "W2c(macrofns): ustrregexra_codepoint_quantifier -- `ustrregexra(\"\xC3\xA9\xC3\xA9\","
            "\"\xC3\xA9+\",\"x\")` -> x (the quantifier repeats the code point; byte-wise gave "
            "xx). Got rc " + rc + " text `" + got + "`");
  }

  // Review finding (v2): what ECMAScript does not share with Stata's ICU is a
  // NAMED refusal at rc 199 (exprfns.cpp fnUstrregexra), reaching `_rc` and
  // the message -- never r(198) "invalid syntax" (the fix-round-2 refusal
  // surfaced as exactly that) and never the subject echoed back. Measured in
  // StataBE 19.5: ustrregexra("café","[^\w ]","") keeps é (ICU \w is
  // Unicode); the classic-locale \w would delete it, so this is refused.
  {
    piet::SessionState session(workdir);
    const std::string cmd = "local __m_urw = ustrregexra(\"caf\xC3\xA9\",\"[^\\w ]\",\"\")";
    std::string rc = rcOf(session, cmd);
    std::string msg = errMsgOf(session, cmd);
    w2check(rc == "199" && msg.find("\\w on non-ASCII text") != std::string::npos,
            "W2c(macrofns): ustrregexra_refuse_word_class -- `[^\\w ]` on a non-ASCII subject "
            "is refused by name at rc 199. Got rc " + rc + " message `" + msg + "`");
    (void)piet::dispatch("capture local __m_urw2 = ustrregexra(\"caf\xC3\xA9\",\"[^\\w ]\",\"\")",
                         session);
    std::string drc = displayOf(session, "display _rc");
    w2check(drc == "199",
            "W2c(macrofns): ustrregexra_refusal_rc_reaches__rc -- `capture` of the refusal leaves "
            "_rc = 199. Got `" + drc + "`");
    const std::string bad = "local __m_urbad = ustrregexra(\"abc\",\"(\",\"x\")";
    rc = rcOf(session, bad);
    msg = errMsgOf(session, bad);
    w2check(rc == "199" && msg.find("cannot compile") != std::string::npos,
            "W2c(macrofns): ustrregexra_refuse_bad_pattern -- an uncompilable pattern is a named "
            "refusal (rc 199), not the subject echoed back. Got rc " + rc + " message `" + msg + "`");
    const std::string ci = "local __m_urci = ustrregexra(\"\xC3\x89\",\"\xC3\xA9\",\"x\",1)";
    rc = rcOf(session, ci);
    w2check(rc == "199",
            "W2c(macrofns): ustrregexra_refuse_nonascii_icase -- case-insensitive matching of "
            "non-ASCII text (ICU folds \xC3\x89 to \xC3\xA9) is refused at rc 199. Got rc " + rc);
    // ASCII input keeps \w: no divergence, no refusal.
    rc = rcOf(session, "local __m_urascii = ustrregexra(\"a-b c\",\"[^\\w ]\",\"\")");
    const std::string got = displayOf(session, "display \"`__m_urascii'\"");
    w2check(rc == "0" && got == "ab c",
            "W2c(macrofns): ustrregexra_ascii_word_class -- `[^\\w ]` on ASCII -> ab c, rc 0. "
            "Got rc " + rc + " text `" + got + "`");
  }

  // Review finding (v2): runiform()/runiformint() follow `set seed` (they
  // drew from the C library's rand() and ignored it). Oracle runiform_macro /
  // runiformint_macro pin rc 0 only; the stream is ours (Philox, core/rng.h).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("set seed 7", session);
    std::string rc = rcOf(session, "local __r_a = runiform()");
    const std::string a = displayOf(session, "display \"`__r_a'\"");
    (void)rcOf(session, "local __r_a2 = runiform()");
    const std::string a2 = displayOf(session, "display \"`__r_a2'\"");
    (void)piet::dispatch("set seed 7", session);
    (void)rcOf(session, "local __r_b = runiform()");
    const std::string b = displayOf(session, "display \"`__r_b'\"");
    double va = -1;
    try {
      va = std::stod(a);
    } catch (const std::exception&) {
    }
    w2check(rc == "0" && a == b && va >= 0 && va < 1 && a != a2,
            "W2c(macrofns): runiform_follows_seed -- `set seed 7` twice gives the same first draw, "
            "in [0,1), and the next draw differs. Got a=`" + a + "` a2=`" + a2 + "` b=`" + b +
                "` rc " + rc);
    rc = rcOf(session, "local __r_i = runiformint(1,6)");
    const std::string it = displayOf(session, "display \"`__r_i'\"");
    bool ok = false;
    try {
      size_t pos = 0;
      const long long v = std::stoll(it, &pos);
      ok = pos == it.size() && v >= 1 && v <= 6;
    } catch (const std::exception&) {
    }
    w2check(rc == "0" && ok,
            "W2c(macrofns): runiformint_range -- runiformint(1,6) is an integer in [1,6]. Got `" +
                it + "` rc " + rc);
    // The inline form draws from the same seeded stream.
    (void)piet::dispatch("set seed 7", session);
    const std::string inl = displayOf(session, "display \"`=runiform()'\"");
    w2check(inl == a,
            "W2c(macrofns): runiform_inline_follows_seed -- `` `=runiform()' `` after `set seed 7` "
            "is the same first draw as the local form. Got `" + inl + "` vs `" + a + "`");
  }

  // Fix round v4 (review): the macro real-to-text rule. StataBE 19.5 stores
  // `local x = expr` as exactly strtrim(string(x,"%18.0g")). Round 5: every
  // row now cites its golden record in tests/eval/golden/aer_w2/
  // w2f_residuals.log section A (the lt_NN rows are the local form, nw_NN the
  // plain quoted-local read; gt_NN/in_NN the global and `=expr' forms), not
  // the task's scratch fmt.log the golden was built from. Every row failed on
  // the old 16-significant-digit rule except the plain 16-digit ones (1/3 ..
  // 1e15/3, 12345.678..., 0.1+0.2, 1e15); e.g. 0.1/3 stored
  // .03333333333333333, 1e-5/3 3.333333333333333e-06, 1e17 the full
  // 18-digit expansion.
  {
    struct Row { const char* expr; const char* want; int line; };
    static const Row kRows[] = {
      {"1/3", ".3333333333333333", 76},  // lt_01
      {"2/3", ".6666666666666666", 98},  // lt_02
      {"10/3", "3.333333333333333", 120},  // lt_03
      {"100/3", "33.33333333333334", 142},  // lt_04
      {"1e5/3", "33333.33333333334", 164},  // lt_05
      {"1e10/3", "3333333333.333333", 186},  // lt_06
      {"1e14/3", "33333333333333.33", 208},  // lt_07
      {"1e15/3", "333333333333333.3", 230},  // lt_08
      {"1e16/3", "3333333333333334", 252},  // lt_09
      {"1e17/3", "3.33333333333e+16", 274},  // lt_10
      {"1e20/3", "3.33333333333e+19", 296},  // lt_11
      {"1e30/3", "3.33333333333e+29", 318},  // lt_12
      {"0.1/3", ".0333333333333333", 340},  // lt_13
      {"0.01/3", ".0033333333333333", 362},  // lt_14
      {"0.001/3", ".0003333333333333", 384},  // lt_15
      {"1e-4/3", ".0000333333333333", 406},  // lt_16
      {"1e-5/3", "3.33333333333e-06", 428},  // lt_17
      {"1e-8/3", "3.33333333333e-09", 450},  // lt_18
      {"1e-20/3", "3.33333333333e-21", 472},  // lt_19
      {"-1/3", "-.3333333333333333", 494},  // lt_20
      {"-0.01/3", "-.0033333333333333", 516},  // lt_21
      {"-1e-5/3", "-3.33333333333e-06", 538},  // lt_22
      {"-1e20/3", "-3.33333333333e+19", 560},  // lt_23
      {"0.1+0.2", ".3", 582},  // lt_24
      {"1e15", "1000000000000000", 604},  // lt_25
      {"1e16", "1.00000000000e+16", 626},  // lt_26
      {"1e17", "1.00000000000e+17", 648},  // lt_27
      {"1e20", "1.00000000000e+20", 670},  // lt_28
      {"123456789012345678", "1.23456789012e+17", 692},  // lt_29
      {"-1e16", "-1.00000000000e+16", 714},  // lt_30
      {"12345.678901234567", "12345.67890123457", 736},  // lt_31
      {"0.017189507518210193", ".0171895075182102", 758},  // lt_32
      {"99999999999999999", "1.00000000000e+17", 780},  // lt_33
      {"1e-300", "1.0000000000e-300", 802},  // lt_34
      {"5e-324", "4.9406564584e-324", 824},  // lt_35
      {"1e16", "1.00000000000e+16", 1015},  // nw_01
      {"1e15", "1000000000000000", 1028},  // nw_02
      {"123456789012345678", "1.23456789012e+17", 1041},  // nw_03
      {"-0", "0", 1054},  // nw_04
      {"5e-324", "4.9406564584e-324", 1067},  // nw_05
      {"1e-300", "1.0000000000e-300", 1080},  // nw_06
      {"1e-5/3", "3.33333333333e-06", 1093},  // nw_07
    };
    static const Row kGlobalRows[] = {
      {"1/3", ".3333333333333333", 837},  // gt_01
      {"0.017189507518210193", ".0171895075182102", 859},  // gt_02
      {"1e-5/3", "3.33333333333e-06", 881},  // gt_03
      {"1e16", "1.00000000000e+16", 903},  // gt_04
      {"1e17/3", "3.33333333333e+16", 925},  // gt_05
      {"-0.01/3", "-.0033333333333333", 947},  // gt_06
      {"12345.678901234567", "12345.67890123457", 969},  // gt_07
      {"1e15/3", "333333333333333.3", 991},  // gt_08
    };
    static const Row kInlineRows[] = {
      {"1/3", ".3333333333333333", 847},  // in_01
      {"0.017189507518210193", ".0171895075182102", 869},  // in_02
      {"1e-5/3", "3.33333333333e-06", 891},  // in_03
      {"1e16", "1.00000000000e+16", 913},  // in_04
      {"1e17/3", "3.33333333333e+16", 935},  // in_05
      {"-0.01/3", "-.0033333333333333", 957},  // in_06
      {"12345.678901234567", "12345.67890123457", 979},  // in_07
      {"1e15/3", "333333333333333.3", 1001},  // in_08
    };
    piet::SessionState session(workdir);
    auto runTable = [&](const char* arm, const Row* rows, size_t n, int form) {
      int bad = 0;
      std::string first;
      for (size_t i = 0; i < n; ++i) {
        const Row& r = rows[i];
        std::string rc, got;
        if (form == 0) {
          rc = rcOf(session, std::string("local __m_fmt = ") + r.expr);
          got = displayOf(session, "display \"`__m_fmt'\"");
        } else if (form == 1) {
          rc = rcOf(session, std::string("global W2C_FMT_G = ") + r.expr);
          got = displayOf(session, "display \"$W2C_FMT_G\"");
        } else {
          rc = rcOf(session, std::string("local __m_fmti = \"`=(") + r.expr + ")'\"");
          got = displayOf(session, "display \"`__m_fmti'\"");
        }
        if (rc != "0" || got != r.want) {
          if (bad++ == 0) {
            first = std::string(r.expr) + " stored `" + got + "` rc " + rc + ", golden " + r.want +
                    " (w2f_residuals.log:" + std::to_string(r.line) + ")";
          }
        }
      }
      w2check(bad == 0, std::string("W2c(macrofns): ") + arm + " -- " + std::to_string(n) +
                            " golden texts (w2f_residuals.log section A, %18.0g rule). Mismatches " +
                            std::to_string(bad) + (bad ? "; first: " + first : std::string()));
    };
    runTable("local_text_rule_18_0g", kRows, sizeof(kRows) / sizeof(kRows[0]), 0);
    runTable("global_text_rule_18_0g", kGlobalRows, sizeof(kGlobalRows) / sizeof(kGlobalRows[0]), 1);
    runTable("inline_text_rule_18_0g", kInlineRows, sizeof(kInlineRows) / sizeof(kInlineRows[0]), 2);
  }

  // `local x = 1e16`. Round 4 left this a SKIP: the task's first acceptance
  // item said 10000000000000000 (the text frozen main stores) while StataBE
  // stored 1.00000000000e+16. The orchestrator withdrew that item (ruling R1,
  // 2026-09-27); the golden is the truth: w2f_residuals.log:626 (lt_26) and
  // :1015 (nw_01, the plain quoted-local read) both store 1.00000000000e+16,
  // and :903/:913 (gt_04/in_04) agree for the global and inline forms. Fails
  // on frozen main (10000000000000000).
  {
    piet::SessionState session(workdir);
    const std::string rc = rcOf(session, "local x = 1e16");
    const std::string got = displayOf(session, "display \"`x'\"");
    w2check(rc == "0" && got == "1.00000000000e+16",
            "W2c(macrofns): local_1e16_text -- `local x = 1e16` stores 1.00000000000e+16 (golden "
            "PREC|nw_01|0|1e16|1.00000000000e+16|, w2f_residuals.log:1015). Got rc " + rc +
                " text `" + got + "`");
  }

  // BUG-661: round() ties go up -- round(x) = floor(x+.5), round(x,u) =
  // u*floor(x/u+.5). Golden w2f_residuals.log:1303 (round_matrix_row, %21x):
  // round(2.5) 3, round(-2.5) -2, round(0.5) 1, round(1.5) 2,
  // round(2.675,.01) 2.68 (+1.570a3d70a3d71X+001, the double 2.68),
  // round(1/3,.01) .33 (+1.51eb851eb851fX-002, the double .33); :1339/:1349/
  // :1360 (round_local/round_scalar/round_display) give round(2.5) 3 in the
  // other contexts. The branch's std::round answered round(-2.5) = -3 in all
  // three contexts; frozen main's mRound (Python, half-to-even) answered
  // round(2.5) = 2 and refused round(x,.01) with r(198). Matrix-context arms
  // belong to piet-matrix-expressions.
  {
    struct Case { const char* expr; const char* text; double value; };
    static const Case kCases[] = {
        {"round(2.5)", "3", 3.0},          {"round(-2.5)", "-2", -2.0},
        {"round(0.5)", "1", 1.0},          {"round(1.5)", "2", 2.0},
        {"round(2.675,.01)", "2.68", 2.68}, {"round(1/3,.01)", ".33", 0.33},
    };
    piet::SessionState session(workdir);
    for (const Case& c : kCases) {
      std::string rc = rcOf(session, std::string("local __m_rnd = ") + c.expr);
      std::string got = displayOf(session, "display \"`__m_rnd'\"");
      w2check(rc == "0" && got == c.text,
              std::string("W2c(macrofns): round_local_ties -- `local x = ") + c.expr + "` stores " +
                  c.text + " (golden w2f_residuals.log:1303/:1339). Got rc " + rc + " text `" + got + "`");
      rc = rcOf(session, std::string("scalar __s_rnd = ") + c.expr);
      const double sv = scalarValue(session, "__s_rnd");
      w2check(rc == "0" && sv == c.value,
              std::string("W2c(macrofns): round_scalar_ties -- `scalar s = ") + c.expr + "` is exactly " +
                  c.text + " (golden w2f_residuals.log:1303/:1349, %21x). Got rc " + rc + " value " +
                  g17(sv));
      got = displayOf(session, std::string("display ") + c.expr);
      w2check(got == c.text,
              std::string("W2c(macrofns): round_display_ties -- `display ") + c.expr + "` shows " + c.text +
                  " (golden w2f_residuals.log:1303/:1360). Got `" + got + "`");
    }
  }

  // BUG-662: normal() in the lower tail. 0.5*(1+erf(z/sqrt2)) cancels for
  // z < 0: the branch gave normal(-8) = 6.106226635438361e-16 (1.8% off) and
  // normal(-10) = normal(-30) = 0. References: normal(-8) is StataBE 19.5's
  // 6.22096057427178486e-16 (scratch capture engine-expression-functions/
  // stata/meas.log:128, not a golden); normal(-10) and normal(-30) were
  // computed for this arm in 60-digit Decimal arithmetic from the Laplace
  // continued fraction Q(x) = phi(x)/(x+1/(x+2/(x+...))) (task scratch
  // r6/ref.py), which also reproduces the StataBE normal(-8) to 1.2e-16.
  // Scalars carry the full double; locals carry %18.0g text.
  {
    piet::SessionState session(workdir);
    struct Case { const char* z; double ref; };
    static const Case kCases[] = {
        {"-8", 6.22096057427178486e-16},
        {"-10", 7.61985302416052606597e-24},
        {"-30", 4.90671392714818705953e-198},
    };
    for (const Case& c : kCases) {
      const std::string rc = rcOf(session, std::string("scalar __s_nrm = normal(") + c.z + ")");
      const double sv = scalarValue(session, "__s_nrm");
      w2check(rc == "0" && sv > 0.0 && relEq(sv, c.ref, 1e-12),
              std::string("W2c(macrofns): normal_lower_tail -- `scalar s = normal(") + c.z +
                  ")` is positive and within 1e-12 relative of " + g17(c.ref) +
                  ". Got rc " + rc + " value " + g17(sv));
      const std::string lrc = rcOf(session, std::string("local __m_nrm = normal(") + c.z + ")");
      const std::string txt = displayOf(session, "display \"`__m_nrm'\"");
      double lv = std::nan("");
      try { lv = std::stod(txt); } catch (const std::exception&) {}
      w2check(lrc == "0" && lv > 0.0 && relEq(lv, c.ref, 1e-11),
              std::string("W2c(macrofns): normal_lower_tail_local -- `local x = normal(") + c.z +
                  ")` stores a positive value (its %18.0g text carries 12 digits). Got rc " + lrc +
                  " text `" + txt + "`");
    }
    std::string rc = rcOf(session, "local __m_nrm0 = normal(0)");
    std::string got = displayOf(session, "display \"`__m_nrm0'\"");
    w2check(rc == "0" && got == ".5",
            "W2c(macrofns): normal_zero -- `local x = normal(0)` stores .5. Got rc " + rc + " text `" + got + "`");
    rc = rcOf(session, "local __m_nrmm = normal(.)");
    got = displayOf(session, "display \"`__m_nrmm'\"");
    w2check(rc == "0" && got == ".",
            "W2c(macrofns): normal_missing -- `local x = normal(.)` stores `.`. Got rc " + rc + " text `" + got + "`");
  }

  // Review finding (v3): negative zero. std::round(-0.4) is -0.0, and the
  // integer renderers printed it as "-0"; frozen main (Python round's int
  // result) stored 0. Not a w2c_functions arm; StataBE 19.5 measured in this
  // fix round: round(-0.4), round(-0.001,.01), invt(10,.5), -0.0,
  // max(-0.4,-0.5)*0, ceil(-0.4) and string(-0.0) are all 0 in local,
  // global, display and the inline form.
  {
    piet::SessionState session(workdir);
    const char* cases[][2] = {{"round(-0.4)", "rneg"},       {"round(-0.001,.01)", "rnegu"},
                              {"invt(10,.5)", "invtmed"},    {"-0.0", "negz"},
                              {"max(-0.4,-0.5)*0", "maxz"},  {"ceil(-0.4)", "ceil"},
                              {"string(-0.0)", "strneg"}};
    for (const auto& c : cases) {
      const std::string name = std::string("__m_z_") + c[1];
      const std::string rc = rcOf(session, "local " + name + " = " + c[0]);
      const std::string got = displayOf(session, "display \"`" + name + "'\"");
      w2check(rc == "0" && got == "0",
              std::string("W2c(macrofns): no_negative_zero -- `local x = ") + c[0] +
                  "` stores 0, not -0 (StataBE 19.5). Got rc " + rc + " text `" + got + "`");
    }
    std::string rc = rcOf(session, "global M_ZNEG_W2C = round(-0.4)");
    std::string got = displayOf(session, "display \"$M_ZNEG_W2C\"");
    w2check(rc == "0" && got == "0",
            "W2c(macrofns): no_negative_zero_global -- `global G = round(-0.4)` stores 0. Got `" +
                got + "`");
    got = displayOf(session, "display round(-0.4)");
    w2check(got == "0",
            "W2c(macrofns): no_negative_zero_display -- `display round(-0.4)` shows 0. Got `" +
                got + "`");
    got = displayOf(session, "display \"`=-0.0'\"");
    w2check(got == "0",
            "W2c(macrofns): no_negative_zero_inline -- `` `=-0.0' `` is 0. Got `" + got + "`");
  }

  // Review finding (v3): Stata's `&`, `|` and `!` over a string operand are
  // r(109) type mismatch (StataBE 19.5, this fix round), whichever side the
  // string is on and even where the other operand already decides the
  // result. The branch had answered 1, 1, 1, 0, 0 and 0 with rc 0 (the
  // operators coerced strings through truthy()); `"a" + 1` stored its own
  // literal text, rc 0, through the string-literal exemption.
  {
    piet::SessionState session(workdir);
    const char* exprs[] = {"\"a\" & 1", "\"x\" | 0", "1 | \"a\"", "0 & \"a\"",
                           "!\"a\"",     "!\"\"",     "\"a\" + 1"};
    int k = 0;
    for (const char* ex : exprs) {
      const std::string name = "__m_tm" + std::to_string(k++);
      const std::string rc = rcOf(session, "local " + name + " = " + ex);
      const std::string got = displayOf(session, "display \"[`" + name + "']\"");
      w2check(rc == "109" && got == "[]",
              std::string("W2c(macrofns): logical_string_type_mismatch -- `local x = ") + ex +
                  "` is r(109) and leaves x unset (StataBE 19.5). Got rc " + rc + " text `" +
                  got + "`");
    }
    // `&`/`|` evaluate every operand (no short-circuit): after `set seed 1`,
    // `local x = 1 | runiform()` consumes the first draw (StataBE 19.5:
    // the next runiform() is the stream's second draw).
    (void)piet::dispatch("set seed 1", session);
    (void)piet::dispatch("local __m_sc1 = runiform()", session);
    (void)piet::dispatch("local __m_sc2 = runiform()", session);
    (void)piet::dispatch("set seed 1", session);
    (void)piet::dispatch("local __m_scx = 1 | runiform()", session);
    (void)piet::dispatch("local __m_sc3 = runiform()", session);
    const std::string d2 = displayOf(session, "display \"`__m_sc2'\"");
    const std::string d3 = displayOf(session, "display \"`__m_sc3'\"");
    w2check(!d2.empty() && d2 == d3,
            "W2c(macrofns): logical_no_short_circuit -- `1 | runiform()` draws, so the next "
            "draw is the second of the stream. Got `" + d3 + "` vs `" + d2 + "`");
    // The stream is Stata's (core/rng_mt64.h): after `set seed 1` the first two
    // runiform() draws are StataBE's +1.122deafddb434X-003 and +1.175c928118c7cX-003
    // (internal probe g13), and runiformint(7,7) takes no draw (impl_C probe P3).
    (void)piet::dispatch("set seed 1", session);
    const std::string m1 = displayOf(session, "display %21x runiform()");
    const std::string m7 = displayOf(session, "display runiformint(7,7)");
    const std::string m2 = displayOf(session, "display %21x runiform()");
    w2check(m1.find("1.122deafddb434X-003") != std::string::npos &&
                m2.find("1.175c928118c7cX-003") != std::string::npos && m7 == "7",
            "W2c(macrofns): runiform_stata_mt64 -- `set seed 1` then runiform() is Stata's "
            "mt64 stream draw for draw (+1.122deafddb434X-003, then +1.175c928118c7cX-003 "
            "after a draw-free runiformint(7,7)). Got `" + m1 + "`, `" + m2 + "`, `" + m7 + "`");
  }

  // Review finding (v3): `local x = foo | bar` over dataset variables. Stata
  // reads the first observation (1, 0 and 0 for foo=1, bar=0: StataBE 19.5).
  // BUG-710: the macro layer now binds a bare variable at observation 1
  // (w2i_varsub.log F section, f_01..), so these store StataBE's 1/0/0 at
  // rc 0. Main refused them with r(198); the literal text is never stored.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    const std::string g1 = rcOf(session, "gen foo = 1");
    const std::string g2 = rcOf(session, "gen bar = 0");
    if (g1 != "0" || g2 != "0") {
      w2skip("W2c(macrofns): logical_over_variables_obs1",
             "PRECONDITION: gen failed (no engine in this environment), rc " + g1 + "/" + g2);
    } else {
      const char* exprs[] = {"foo | bar", "foo & bar", "!foo"};
      const char* want[] = {"[1]", "[0]", "[0]"};
      int k = 0;
      for (const char* ex : exprs) {
        const std::string name = "__m_lv" + std::to_string(k);
        const std::string rc = rcOf(session, "local " + name + " = " + ex);
        const std::string got = displayOf(session, "display \"[`" + name + "']\"");
        w2check(rc == "0" && got == want[k],
                std::string("W2c(macrofns): logical_over_variables_obs1 -- `local x = ") + ex +
                    "` reads observation 1 and stores " + want[k] + " (StataBE 19.5), never the "
                    "literal text. Got rc " + rc + " text `" + got + "`");
        ++k;
      }
    }
  }

  // T7 unit L4 (AER package 007): the function Ftail(df1,df2,f), the F upper
  // tail, in the macro/display/scalar/matrix layers. Corpus use: TA3 - Balance
  // across arms among responders.do:108 and TA4 ... full sample.do:103,
  //   global fstat_pval : di %6.3fc Ftail(e(df_m), e(df_r), e(F)).
  // Every StataBE value below is a number this unit read off StataBE 19.5
  // (`display %26.17e Ftail(...)` over a 1,440-point df1 x df2 x f grid plus
  // edge cases, tools/stata_batch.sh); the rows that match a repo oracle log
  // name it. Identities are closed forms evaluated here, not copied numbers.
  {
    piet::SessionState session(workdir);
    // The scalar is dropped first and the assignment's rc is required to be 0,
    // so a refused or failed call reads NaN and can never inherit the
    // previous call's value.
    auto ft = [&](const std::string& args) {
      (void)piet::dispatch("scalar drop __t7_ft", session);
      const std::string rc = rcOf(session, "scalar __t7_ft = Ftail(" + args + ")");
      return rc == "0" ? scalarValue(session, "__t7_ft") : std::nan("");
    };
    auto shown = [&](const std::string& expr) {
      return displayOf(session, "display " + expr);
    };

    // 1. Values against StataBE, one row per branch of the implementation:
    //    the continued fraction (both df < 2e4), the expansion in 1/A (one df
    //    >= 2e4, the other < 20: the large-sample regression case), and the
    //    log-density quadrature (one df >= 2e4, the other >= 20).
    struct Ref {
      const char* args;
      double want;
      double rtol;
    };
    static const Ref refs[] = {
        // continued fraction
        {"3,100,2.5", 6.38329599790864871e-02, 1e-11},
        {"1,5,4", 1.01939478829858279e-01, 1e-11},
        {"1,30,3.5", 7.11520192591346690e-02, 1e-11},
        {"0.5,3,0.1", 5.91273872666287392e-01, 1e-11},
        {"0.5,10,2.5", 1.41234373262984464e-01, 1e-11},
        {"2.5,3.5,1.7", 3.12890425813900419e-01, 1e-11},
        {"7.5,0.25,3", 6.99767270627172477e-01, 1e-11},
        {"0.25,7.5,3", 1.09994557671814139e-01, 1e-11},
        {"50,100,2.5", 5.06836920933830128e-05, 1e-11},
        {"1000,100,1.5", 5.45883816847500276e-03, 1e-11},
        {"1000,1000,2.5", 2.50511946849770569e-46, 1e-11},
        {"2,2,3", 0.25, 1e-13},
        {"1,1,2", 3.91826552030607123e-01, 1e-11},
        // expansion in 1/A: df2 >= 2e4 (or df1), the other df < 20
        {"2,73076,468.5", 6.70570688442791622e-203, 1e-11},
        {"3,73076,468.5", 1.47742998510012583e-301, 1e-11},
        {"10,73076,1.5", 1.32088047464453184e-01, 1e-11},
        {"10,73076,10", 5.60995085335858385e-17, 1e-11},
        {"3,1e6,2.5", 5.75590782707778512e-02, 1e-11},
        {"3,1e10,2.5", 5.75584520352660570e-02, 1e-11},
        {"1,1e15,468.5", 6.79486424007085279e-104, 1e-11},
        {"1e8,5,0.5", 9.24764743936393519e-01, 1e-11},
        {"0.01,1e6,3", 1.80425620855270523e-02, 1e-11},
        {"3,2e17,2", 1.11610225094712606e-01, 1e-11},
        {"2e17,3,2", 3.17729669663787295e-01, 1e-11},
        {"2e-10,2e17,5", 2.08391973314734676e-09, 1e-9},
        // tiny df1 with df2 >= 20 (the expansion takes over from the CF where
        // the CF would return 1 - P)
        {"2e-10,40,5", 2.08644056127301700e-09, 1e-11},
        {"2e-10,40,1e-10", 4.54996943728598044e-09, 1e-11},
        {"2e-10,15000,2", 2.17555547292441534e-09, 1e-11},
        {"1e-6,20,2", 6.64454231589650555e-06, 1e-11},
        {"1e-6,100,2", 6.62414287589135693e-06, 1e-11},
        {"1e-3,15000,1.5", 3.30425336933489518e-03, 1e-11},
        {"0.05,20,2", 6.14463276269955616e-02, 1e-11},
        // deep tails of the expansion with df1 = 19 (B = 9.5): the tail is a
        // normal double while e^-z alone is subnormal or 0
        {"19,73076,60", 4.58528385582333876e-228, 1e-11},
        {"19,73076,76", 4.48250713590199849e-292, 1e-11},
        // log-density quadrature: both df >= 20, one of them >= 2e4
        {"50,1e6,2.5", 2.34214867775739902e-08, 1e-11},
        {"1000,1e6,1.5", 1.11389784557478053e-22, 1e-11},
        {"1e5,1000,10", 4.01549301113656773e-306, 1e-11},
        {"1e8,1e6,1", 5.00185258576474223e-01, 1e-11},
        {"1e8,1e8,1.0001", 3.08546340304065137e-01, 1e-11},
        {"2e17,2e17,1", 0.5, 1e-12},
        // StataBE's own digits drift once both df pass ~1e10 (about 2e-9 here,
        // 9e-7 at df 2e17): the tolerance is StataBE's, not this code's. An
        // independent 70-digit quadrature of the log-F density gives
        // 5.6923157933930828e-2 for this point.
        {"1e15,1e15,1.0000001", 5.69231580394491746e-02, 1e-7},
        // documented limit: relative error ~1e-16/df1 when df1 < ~1e-3 (the
        // absolute error stays ~1e-16); StataBE keeps full precision there.
        {"2e-10,3,2", 2.21244631943694839e-09, 1e-6},
    };
    for (const Ref& r : refs) {
      const double got = ft(r.args);
      w2check(relEq(got, r.want, r.rtol),
              std::string("W2c(macrofns): t7_ftail_value -- Ftail(") + r.args + ") = " +
                  g17(r.want) + " (StataBE 19.5 probe, rtol " + g17(r.rtol) + "). Got " + g17(got));
    }

    // Past 1e-298 StataBE's own digits drift (Ftail(19,73076,78) is 3.9e-7 off,
    // Ftail(15,73076,100) 8.7e-4 off) and it returns 0 for Ftail(19,73076,79.5),
    // so these three are pinned to the exact value instead: df2/2 = 36538 is an
    // integer, hence Q = 1 - y^b sum_{k<36538} (b)_k/k! x^k with b = df1/2,
    // x = a/(a+b f), y = 1-x, summed in 460-digit Decimal arithmetic.
    static const Ref exact[] = {
        {"19,73076,78", 4.52643886723028035e-300, 1e-11},
        {"19,73076,79.5", 4.57134235673965844e-306, 1e-11},
        {"15,73076,100", 8.70009531514310040e-308, 1e-11},
    };
    for (const Ref& r : exact) {
      const double got = ft(r.args);
      w2check(relEq(got, r.want, r.rtol),
              std::string("W2c(macrofns): t7_ftail_exact_deep -- Ftail(") + r.args + ") = " +
                  g17(r.want) + " (460-digit finite-sum value). Got " + g17(got));
    }

    // 2. The repo's own StataBE logs. iv_scalars_oracle.log:43-53 shows
    //    Ftail(e(df_m),e(df_r),e(F)) = 3.8062503920808e-203 for df_m 2, df_r
    //    73076, e(F) 4.69073580261539e+02 (printed to 15 digits, so the F the
    //    log's Ftail saw differs from the literal below in the 16th; a
    //    sensitivity of d ln p / d ln F ~ 469 turns that into ~4e-13).
    //    test_chow_golden.do:40 computes Ftail(4, 73071, 207.288325149405) =
    //    3.795539655343e-177 (its log is not in this checkout; the same call
    //    run through StataBE for this unit gives the second value).
    {
      const double g1 = ft("2,73076,4.69073580261539e+02");
      w2check(relEq(g1, 3.80625039207911666e-203, 1e-11) &&
                  relEq(g1, 3.8062503920808e-203, 1e-11),
              "W2c(macrofns): t7_ftail_oracle_iv -- Ftail(2,73076,4.69073580261539e+02) = "
              "3.80625039207911666e-203 (StataBE probe; iv_scalars_oracle.log:51-53 prints "
              "3.8062503920808e-203 for the unrounded e(F)). Got " + g17(g1));
      const double g2 = ft("4,73071,207.288325149405");
      w2check(relEq(g2, 3.79553965534116614e-177, 1e-11) && relEq(g2, 3.795539655343e-177, 1e-11),
              "W2c(macrofns): t7_ftail_oracle_chow -- Ftail(4,73071,207.288325149405) = "
              "3.79553965534116614e-177 (StataBE probe; test_chow_golden.do:40 prints "
              "3.795539655343e-177). Got " + g17(g2));
    }

    // 3. Closed forms and identities, across all three branches.
    //  df1 = 2:  Ftail(2,d,f) = (1 + 2f/d)^(-d/2).
    //  df1 = 4:  Ftail(4,d,f) = x^a (1 + a y), a = d/2, s = 4f/d, x = 1/(1+s), y = 1-x.
    //  df1 = df2 = 2:  1/(1+f).
    //  df1 = 1:  Ftail(1,d,f) = 2 ttail(d, sqrt f)  (this build's ttail).
    //  df1 = df2 = d:  Ftail(d,d,f) + Ftail(d,d,1/f) = 1.
    for (double d : {7.0, 73076.0, 1e6, 3.7e9, 2e17}) {
      for (double f : {0.3, 2.5, 40.0, 468.5}) {
        const double a = d / 2.0;
        const double want2 = std::exp(-a * std::log1p(2.0 * f / d));
        if (want2 > 1e-300) {
          const double got = ft("2," + g17(d) + "," + g17(f));
          w2check(relEq(got, want2, 1e-11),
                  "W2c(macrofns): t7_ftail_df1_is_2 -- Ftail(2," + g17(d) + "," + g17(f) +
                      ") = (1+2f/d)^(-d/2) = " + g17(want2) + ". Got " + g17(got));
        }
        const double s = 4.0 * f / d;
        const double want4 = std::exp(-a * std::log1p(s)) * (1.0 + a * (s / (1.0 + s)));
        if (want4 > 1e-300) {
          const double got = ft("4," + g17(d) + "," + g17(f));
          w2check(relEq(got, want4, 1e-11),
                  "W2c(macrofns): t7_ftail_df1_is_4 -- Ftail(4," + g17(d) + "," + g17(f) +
                      ") = x^a (1 + a y) = " + g17(want4) + ". Got " + g17(got));
        }
      }
    }
    for (double f : {0.3, 1.0, 9.0, 1e6}) {
      const double got = ft("2,2," + g17(f));
      w2check(relEq(got, 1.0 / (1.0 + f), 1e-13),
              "W2c(macrofns): t7_ftail_2_2 -- Ftail(2,2," + g17(f) + ") = 1/(1+f). Got " + g17(got));
    }
    for (double d : {1.0, 2.0, 5.0, 30.0, 200.0, 5000.0}) {
      for (double f : {0.5, 2.0, 6.25, 50.0}) {
        const double got = ft("1," + g17(d) + "," + g17(f));
        (void)piet::dispatch("scalar __t7_tt = 2 * ttail(" + g17(d) + ", sqrt(" + g17(f) + "))",
                             session);
        const double via = scalarValue(session, "__t7_tt");
        w2check(relEq(got, via, 1e-9),
                "W2c(macrofns): t7_ftail_vs_ttail -- Ftail(1," + g17(d) + "," + g17(f) +
                    ") = 2 ttail(d, sqrt f) = " + g17(via) + ". Got " + g17(got));
      }
    }
    for (double d : {40.0, 3e4, 1e9}) {
      const double sig = std::sqrt(4.0 / d);
      // 1/f is rounded once before Ftail sees it; the density of ln F at its
      // mode is sqrt(d/(8 pi)), so that rounding alone moves the sum by up to
      // 1.1e-16 * sqrt(d/(8 pi)). The tolerance is that jitter, not slack.
      const double tol = 1e-13 + 4.0 * std::sqrt(d / (8.0 * M_PI)) * 1.2e-16;
      for (double z : {0.5, 2.0}) {
        const double f = 1.0 + z * sig;
        const double p = ft(g17(d) + "," + g17(d) + "," + g17(f));
        const double q = ft(g17(d) + "," + g17(d) + "," + g17(1.0 / f));
        w2check(std::isfinite(p) && std::isfinite(q) && std::fabs(p + q - 1.0) < tol,
                "W2c(macrofns): t7_ftail_equal_df_symmetry -- Ftail(d,d,f) + Ftail(d,d,1/f) = 1 "
                "at d " + g17(d) + " f " + g17(f) + ". Got " + g17(p) + " + " + g17(q));
      }
    }

    // 4. Continuity across the points where the method changes (df 2e4 and 20):
    //    a 2e-10 step in a df moves the answer by < 1e-9, so a seam mismatch
    //    larger than 5e-9 shows.
    struct Seam {
      const char* lo;
      const char* hi;
      const char* what;
    };
    static const Seam seams[] = {
        {"2,19999.9999999,2.5", "2,20000.0000001,2.5", "continued fraction -> 1/A expansion (df2)"},
        {"19999.9999999,2,0.4", "20000.0000001,2,0.4", "continued fraction -> 1/A expansion (df1)"},
        {"19.9999999999,1e6,1.9", "20.0000000001,1e6,1.9",
         "1/A expansion -> quadrature (df1 = 20)"},
        {"1e6,19.9999999999,0.6", "1e6,20.0000000001,0.6",
         "1/A expansion -> quadrature (df2 = 20)"},
        {"19.9999999999,3000,1.9", "20.0000000001,3000,1.9",
         "continued fraction, prefactor form (df1 = 20)"},
        {"19999.9999999,30,1.5", "20000.0000001,30,1.5", "continued fraction -> quadrature"},
    };
    for (const Seam& sm : seams) {
      const double lo = ft(sm.lo), hi = ft(sm.hi);
      w2check(relEq(lo, hi, 5e-9),
              std::string("W2c(macrofns): t7_ftail_seam -- ") + sm.what + ": Ftail(" + sm.lo +
                  ") vs Ftail(" + sm.hi + "). Got " + g17(lo) + " vs " + g17(hi));
    }

    // 5. Missing, range, domain: StataBE prints a plain `.` for a missing
    //    argument of any kind (.a-.z do not survive), `.` for a df outside
    //    2e-10..2e17 or <= 0 (2e-10 and 2e17 themselves are inside), and 1 for
    //    f <= 0 once the dfs are valid (Ftail(0,100,0) is `.`, not 1).
    struct Txt {
      const char* expr;
      const char* want;
    };
    static const Txt texts[] = {
        {"Ftail(3,100,.)", "."},         {"Ftail(3,100,.a)", "."},
        {"Ftail(3,100,.z)", "."},        {"Ftail(.,100,2)", "."},
        {"Ftail(.a,100,2)", "."},        {"Ftail(3,.,2)", "."},
        {"Ftail(3,.b,2)", "."},          {"Ftail(.a,.b,.c)", "."},
        {"Ftail(3,100,0)", "1"},         {"Ftail(3,100,-1)", "1"},
        {"Ftail(3,100,-1e300)", "1"},    {"Ftail(0,100,2)", "."},
        {"Ftail(-1,100,2)", "."},        {"Ftail(3,0,2)", "."},
        {"Ftail(3,-1,2)", "."},          {"Ftail(0,100,0)", "."},
        {"Ftail(3,0,-1)", "."},          {"Ftail(-1,-1,-1)", "."},
        {"Ftail(-1,100,.)", "."},        {"Ftail(1.99999e-10,3,2)", "."},
        {"Ftail(1e-10,3,2)", "."},       {"Ftail(3,1.99999e-10,2)", "."},
        {"Ftail(2.00001e17,3,2)", "."},  {"Ftail(3,2.00001e17,2)", "."},
        {"Ftail(3e17,3,2)", "."},        {"Ftail(3,1e300,2)", "."},
    };
    for (const Txt& t : texts) {
      const std::string got = shown(t.expr);
      w2check(got == t.want, std::string("W2c(macrofns): t7_ftail_edge -- display ") + t.expr +
                                 " -> `" + t.want + "` (StataBE probe). Got `" + got + "`");
    }
    // Underflow is a clean 0, never a NaN or a negative; overflow is `.`.
    w2check(ft("3,100,1e300") == 0.0 && ft("3,100,8e307") == 0.0 && ft("1,1,1e300") > 0.0 &&
                shown("Ftail(3,100,1e308)") == ".",
            "W2c(macrofns): t7_ftail_range -- Ftail(3,100,1e300) and (...,8e307) underflow to 0, "
            "Ftail(1,1,1e300) = 6.4e-151 stays positive (StataBE 6.36619772367581208e-151), "
            "and 1e308 is past Stata's largest double -> missing");
    w2check(relEq(ft("1,1,1e300"), 6.36619772367581208e-151, 1e-11),
            "W2c(macrofns): t7_ftail_range_value -- Ftail(1,1,1e300) = 6.36619772367581208e-151 "
            "(StataBE probe)");

    // 6. Every layer that evaluates an expression: rc 0 and the value.
    std::string rc = rcOf(session, "local __t7_l = Ftail(2,73076,468.5)");
    const std::string lv = displayOf(session, "display \"`__t7_l'\"");
    double lvn = std::nan("");
    try {
      lvn = std::stod(lv);
    } catch (const std::exception&) {
    }
    w2check(rc == "0" && relEq(lvn, 6.70570688442791622e-203, 1e-9),
            "W2c(macrofns): t7_ftail_local -- `local x = Ftail(2,73076,468.5)` stores "
            "6.7057068844e-203 at rc 0. Got rc " + rc + " text `" + lv + "`");

    rc = rcOf(session, "global __t7_g : di %6.3fc Ftail(3,100,2.5)");
    std::string gv = displayOf(session, "display \"[$__t7_g]\"");
    w2check(rc == "0" && gv == "[ 0.064]",
            "W2c(macrofns): t7_ftail_global_di -- `global x : di %6.3fc Ftail(3,100,2.5)` (the "
            "007 spelling) stores ` 0.064` at rc 0. Got rc " + rc + " text `" + gv + "`");
    (void)piet::dispatch("scalar __t7_dfm = 2", session);
    (void)piet::dispatch("scalar __t7_dfr = 73076", session);
    (void)piet::dispatch("scalar __t7_F = 4.69073580261539e+02", session);
    rc = rcOf(session, "global __t7_g2 : di %6.3fc Ftail(__t7_dfm, __t7_dfr, __t7_F)");
    gv = displayOf(session, "display \"[$__t7_g2]\"");
    w2check(rc == "0" && gv == "[ 0.000]",
            "W2c(macrofns): t7_ftail_global_di_scalars -- the 007 form with scalar arguments "
            "(the p-value is 3.8e-203) stores ` 0.000` at rc 0. Got rc " + rc + " text `" + gv +
                "`");

    rc = rcOf(session, "matrix __t7_m = (Ftail(4,100,2.5), Ftail(50,100,2.5))");
    const std::string m1 = displayOf(session, "display %21.15e __t7_m[1,1]");
    const std::string m2 = displayOf(session, "display %21.15e __t7_m[1,2]");
    double m1n = std::nan(""), m2n = std::nan("");
    try {
      m1n = std::stod(m1);
      m2n = std::stod(m2);
    } catch (const std::exception&) {
    }
    w2check(rc == "0" && relEq(m1n, 4.72392389135944807e-02, 1e-11) &&
                relEq(m2n, 5.06836920933830128e-05, 1e-11),
            "W2c(macrofns): t7_ftail_matrix -- `matrix m = (Ftail(4,100,2.5), Ftail(50,100,2.5))` "
            "holds StataBE's 4.72392389135944807e-02 and 5.06836920933830128e-05 at rc 0. Got rc " +
                rc + " `" + m1 + "` `" + m2 + "`");

    const std::string inl = shown("\"`=Ftail(2,3,1)'\"");
    double inln = std::nan("");
    try {
      inln = std::stod(inl);
    } catch (const std::exception&) {
    }
    w2check(relEq(inln, std::pow(5.0 / 3.0, -1.5), 1e-12),
            "W2c(macrofns): t7_ftail_inline -- the inline `=Ftail(2,3,1)' expands to "
            "(1+2/3)^(-3/2) = 0.46475800154489. Got `" + inl + "`");

    // The by-name refusal of a Stata function this build still lacks is
    // untouched: Ftail used to be its example, invFtail is one that is still
    // refused rather than silently expanding to nothing.
    rc = rcOf(session, "display \"x`=invFtail(2,3,.5)'y\"");
    const std::string em = errMsgOf(session, "display \"x`=invFtail(2,3,.5)'y\"");
    w2check(rc != "0" && em.find("invFtail()") != std::string::npos,
            "W2c(macrofns): t7_ftail_refusal_intact -- an inline `=invFtail(2,3,.5)' (a Stata "
            "function not implemented here) is still refused by name, not expanded to nothing. "
            "Got rc " + rc + " message `" + em + "`");
  }

  fs::remove_all(workdir, ec);
}
