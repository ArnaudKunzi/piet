// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `getmata` (ml-getmata T4): the M0 Mata subset, the separate Mata
// namespace, the `mata:` ... `end` block fold, and the `getmata` verb -- plus
// the `set obs` extension path that svmat and getmata's `force` share
// (BUG-697). Oracle: StataBE 19.5 capture tests/eval/golden/aer_w2/
// w2c_ml_getmata.{do,log}, families (8) and (9) and arm ml_fv_rowjoin; the log
// line of each record is cited on its arm. Values are asserted after float
// rounding where Stata stored the variable as float (TOLERANCE.md §3.3):
// getmata compresses integer vectors and copies fractional vectors as float
// unless double was requested. Both the stored values and : type are pinned.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mata_subset.h"
#include "piet/parallel_auto.h"
#include "piet/session_state.h"

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

void runSrc(piet::SessionState& session, const std::string& src) {
  (void)piet::doexec::executeDoSource(src, session, piet::dispatch);
}

// The rc a `capture noisily X` + `local __rc = _rc` pair recorded.
std::string rcLocal(piet::SessionState& session) { return session.eval.locals.get("__rc", "?"); }

// rc of the LAST error block one dispatched line produced ("0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object() || b.value("type", std::string()) != "error") continue;
    rc = piet::eval::coarseRc(b);
  }
  return rc;
}

// The first error block one dispatched line produced (null if none).
nlohmann::ordered_json errorOf(piet::SessionState& session, const std::string& cmd) {
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") return b;
  }
  return nullptr;
}

// A number read back through `display %18.0g EXPR`; `.` (or no output) is NaN.
double num(piet::SessionState& session, const std::string& expr) {
  std::string text;
  for (const auto& b : piet::dispatch("display %18.0g " + expr, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text = b.value("text", std::string());
  }
  const size_t a = text.find_first_not_of(' ');
  if (a == std::string::npos || text.substr(a) == ".") return kNaN;
  char* end = nullptr;
  const double v = std::strtod(text.c_str() + a, &end);
  return end == text.c_str() + a ? kNaN : v;
}

double rval(piet::SessionState& session, const std::string& k) {
  const double* v = session.eval.rValues.find(k);
  return v ? *v : kNaN;
}

bool sameFloat(double got, double want) {
  return static_cast<float>(got) == static_cast<float>(want);
}

std::string fmt(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string trimmed(const std::string& t) {
  const size_t a = t.find_first_not_of(" \t\n");
  if (a == std::string::npos) return std::string();
  return t.substr(a, t.find_last_not_of(" \t\n") - a + 1);
}

// The text of every `display` block one executeDoSource run produced, trimmed.
std::vector<std::string> displayed(const piet::doexec::ExecuteResult& r) {
  std::vector<std::string> out;
  for (const auto& b : r.blocks) {
    if (!b.is_object() || b.value("type", std::string()) != "display") continue;
    out.push_back(trimmed(b.value("text", std::string())));
  }
  return out;
}

std::string joinText(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
  return s;
}

const piet::eval::EvalMatrix* mataM(piet::SessionState& s, const std::string& n) {
  return piet::mata::findMataMatrix(s.eval, n);
}

// 030 Wage_Setting.do:54-105 mirrored on small matrices, verbatim from the
// oracle's family (8) up to `generate byte k = _n` (w2c_ml_getmata.do:240-253).
const char* kFamily8Setup =
    "mata: rchange = J(1,1,.)\n"
    "mata: code = J(1,1,.)\n"
    "foreach i in 1 2 3 {\n"
    "    mata: code = (code \\ `i' )\n"
    "    matrix rchange = (0.1*`i' + 0.05)\n"
    "    mata: rchange = (rchange \\ st_matrix(\"rchange\"))\n"
    "}\n"
    "mata: rchange = select(rchange, rowmissing(rchange):==0)\n"
    "mata: code = select(code, rowmissing(code):==0)\n"
    "mata: f_change = 1:-exp(-rchange)\n"
    "mata: d_change = 1:/rchange\n"
    "clear\n"
    "set obs 3\n"
    "generate byte k = _n\n";

}  // namespace

void testW2Getmata() {
  std::printf("[selftest] --- W2 getmata: M0 Mata subset, Mata namespace, getmata "
              "(w2c_ml_getmata.log families 8/9) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2getmata").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ---- family (8): one session walks the oracle's sequence, arm by arm -------
  {
    piet::SessionState s(workdir);
    runSrc(s, kFamily8Setup);
    const piet::eval::EvalMatrix* rc0 = mataM(s, "rchange");
    w2check(rc0 != nullptr && rc0->data.size() == 3,
            "W2(getmata): 030 setup -- the loop's one-liners built Mata rchange as 3 rows after "
            "select() dropped the leading `.` (was " +
                std::string(rc0 ? std::to_string(rc0->data.size()) : "absent") + ")");

    // getmata_030_seq (log :1058): rc 0, K_new 4, K_existing 0, _N 3, values.
    runSrc(s, "capture noisily getmata code rchange f_change d_change\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0",
            "W2(getmata): getmata_030_seq -- rc 0 (log :1058). Got " + rcLocal(s));
    w2check(rval(s, "K_new") == 4 && rval(s, "K_existing") == 0,
            "W2(getmata): getmata_030_seq -- r(K_new)=4, r(K_existing)=0 (log :1058). Got " +
                fmt(rval(s, "K_new")) + "/" + fmt(rval(s, "K_existing")));
    w2check(num(s, "_N") == 3, "W2(getmata): getmata_030_seq -- _N stays 3");
    w2check(num(s, "code[1]") == 1 && num(s, "code[2]") == 2 && num(s, "code[3]") == 3,
            "W2(getmata): getmata_030_seq -- code = 1,2,3 (the macro-expanded `i' of the loop; "
            "log :1058 code[3]=3)");
    w2check(sameFloat(num(s, "rchange[1]"), .15) && sameFloat(num(s, "rchange[2]"), .25) &&
                sameFloat(num(s, "rchange[3]"), .35),
            "W2(getmata): getmata_030_seq -- rchange = .15,.25,.35 at float precision (log :1058 "
            "rchange[2]=.25) -- the ACCUMULATED Mata vector, not e(b) joined onto itself");
    w2check(s.datastore()->col("f_change").f64()[1] ==
                static_cast<double>(static_cast<float>(1 - std::exp(-.25))),
            "W2(getmata): getmata_030_seq -- f_change[2] = float(1-exp(-.25)) exactly (log :1058 "
            ".2211992144584656 is the float-stored copy). Got " + fmt(num(s, "f_change[2]")));
    w2check(s.datastore()->col("d_change").f64()[2] ==
                static_cast<double>(static_cast<float>(1 / .35)),
            "W2(getmata): getmata_030_seq -- d_change[3] = float(1/.35) exactly (log :1058). Got " +
                fmt(num(s, "d_change[3]")));

    // getmata_exists (log :1070-1074): r(110), Stata's wording.
    runSrc(s, "capture noisily getmata code\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "110",
            "W2(getmata): getmata_exists -- a second `getmata code` is r(110) (log :1074). Got " +
                rcLocal(s));
    {
      auto e = errorOf(s, "getmata code");
      w2check(!e.is_null() && e.value("message", std::string()).find("already exists") !=
                                  std::string::npos,
              "W2(getmata): getmata_exists -- the message is Stata's `variable code already "
              "exists` (log :1070)");
    }

    // getmata_replace (log :1083): rc 0, K_new 0, K_existing 1, code[3] 3.
    runSrc(s, "capture noisily getmata code, replace\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && rval(s, "K_new") == 0 && rval(s, "K_existing") == 1 &&
                num(s, "code[3]") == 3,
            "W2(getmata): getmata_replace -- rc 0, r(K_new)=0, r(K_existing)=1, code[3]=3 (log "
            ":1083). Got rc " + rcLocal(s) + ", " + fmt(rval(s, "K_new")) + "/" +
                fmt(rval(s, "K_existing")));

    // getmata_double (log :1092): rc 0, cd[1] = 1.
    runSrc(s, "capture noisily getmata cd=code, double\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "cd[1]") == 1,
            "W2(getmata): getmata_double -- `getmata cd=code, double` is rc 0 with cd[1]=1 (log "
            ":1092). Got rc " + rcLocal(s));

    runSrc(s, "local ct : type code\nlocal rt : type rchange\nlocal dt : type cd\n");
    w2check(s.eval.locals.get("ct") == "byte" && s.eval.locals.get("rt") == "float" &&
                s.eval.locals.get("dt") == "byte",
            "W2(getmata): integer copies compress even with double; fractional copies use float "
            "(w2c_ml_getmata.log:1058/1092)");
    runSrc(s, "getmata rd=rchange, double\nlocal rdt : type rd\n");
    w2check(s.eval.locals.get("rdt") == "double" && num(s, "rd[3]") == .35,
            "W2(getmata): double option retains fractional Mata double precision");

    // getmata_rows_ne (log :1107): `set obs 5` EXTENDS a 3-row dataset (the
    // BUG-697 path), then a 3-row vector against 5 obs is r(459).
    runSrc(s, "set obs 5\ncapture noisily getmata r2=rchange\nlocal __rc = _rc\n");
    w2check(num(s, "_N") == 5,
            "W2(getmata): getmata_rows_ne setup -- `set obs 5` extends 3 obs to 5 (BUG-697: this "
            "was `cobra produced non-JSON output`, rc 1). _N = " + fmt(num(s, "_N")));
    w2check(rcLocal(s) == "459",
            "W2(getmata): getmata_rows_ne -- rows != _N without force is r(459) (log :1107). Got " +
                rcLocal(s));

    // getmata_force_fewer (log :1116): rc 0, _N 5, r2[3] .35, r2[4]/r2[5] missing.
    runSrc(s, "capture noisily getmata r2=rchange, force\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "_N") == 5 && sameFloat(num(s, "r2[3]"), .35) &&
                std::isnan(num(s, "r2[4]")) && std::isnan(num(s, "r2[5]")),
            "W2(getmata): getmata_force_fewer -- rc 0, _N 5, r2[3]=.35, r2[4] and r2[5] MISSING "
            "(padding, never 0; log :1116). Got rc " + rcLocal(s) + ", r2[4]=" +
                fmt(num(s, "r2[4]")));

    // getmata_force_more (log :1130): 2 obs, 3-row vector, force -> _N 3.
    runSrc(s, "clear\nset obs 2\ncapture noisily getmata r3=rchange, force\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "_N") == 3 && sameFloat(num(s, "r3[3]"), .35),
            "W2(getmata): getmata_force_more -- force EXTENDS _N from 2 to 3 and r3[3]=.35 (log "
            ":1130). Got rc " + rcLocal(s) + ", _N " + fmt(num(s, "_N")));

    // getmata_nodata (log :1141): nothing in memory implies force.
    runSrc(s, "clear\ncapture noisily getmata r4=rchange\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "_N") == 3 && sameFloat(num(s, "r4[1]"), .15),
            "W2(getmata): getmata_nodata -- on an empty session getmata creates the dataset: rc 0, "
            "_N 3, r4[1]=.15 (log :1141; getmata.ado _N==0 & k==0 implies force). Got rc " +
                rcLocal(s) + ", _N " + fmt(num(s, "_N")));

    // getmata_notfound (log :1146-1151): r(111).
    runSrc(s, "capture noisily getmata zz=nosuchvec\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "111",
            "W2(getmata): getmata_notfound -- an absent Mata name is r(111) (log :1151). Got " +
                rcLocal(s));

    // getmata_matrix_paren (log :1167): (m1 m2)=M, K_new 2, m1[2]=3, m2[3]=6.
    runSrc(s,
           "mata: M = (1, 2 \\ 3, 4 \\ 5, 6)\nclear\nset obs 3\n"
           "capture noisily getmata (m1 m2)=M\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && rval(s, "K_new") == 2 && num(s, "m1[2]") == 3 &&
                num(s, "m2[3]") == 6,
            "W2(getmata): getmata_matrix_paren -- rc 0, r(K_new)=2, m1[2]=3, m2[3]=6 (log :1167; "
            "`,` binds tighter than `\\`). Got rc " + rcLocal(s));

    // getmata_paren_badcount (log :1172-1177): r(102), not r(198).
    runSrc(s, "capture noisily getmata (m3)=M\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "102",
            "W2(getmata): getmata_paren_badcount -- one name for a 2-column matrix is r(102) "
            "`too few variables specified for matrix M` (log :1177). Got " + rcLocal(s));
  }

  // ---- family (9): 087's bare block, then getmata; a non-M0 block ------------
  {
    piet::SessionState s(workdir);
    runSrc(s,
           "matrix all_results = J(3, 3, .)\n"
           "matrix all_results[1,1] = 0.5\n"
           "matrix all_results[2,1] = -1.25\n"
           "matrix all_results[3,2] = 2\n"
           "matrix all_results[1,3] = 7\n"
           "mata:\n"
           "    result = st_matrix(\"all_results\")\n"
           "    d10_1 = result[.,1]\n"
           "    dinf_1 = result[.,2]\n"
           "    M_bp_1_1 = result[.,3]\n"
           "end\n"
           "clear\n"
           "set obs 3\n"
           // Verbatim w2c_ml_getmata.do:324-325: the getlist continues after a
           // `/* */` comment that spans the line break (BUG-698; Stata echoes
           // the joined command at log :1216-1217).
           "capture noisily getmata d10_1 dinf_1 /*\n"
           "    */ M_bp_1_1\n"
           "local __rc = _rc\n");
    w2check(rcLocal(s) == "0" && rval(s, "K_new") == 3 && num(s, "_N") == 3,
            "W2(getmata): getmata_087_shape -- the bare `mata:`/`end` block runs and getmata of "
            "its three column vectors, the third named after a two-line `/* */`, is rc 0, _N 3, "
            "r(K_new)=3 = the name count (log :1225; its own `.` is the draft artefact of "
            "arms-file (h)). Got rc " + rcLocal(s) +
                ", K_new " + fmt(rval(s, "K_new")));
    w2check(num(s, "d10_1[1]") == .5 && num(s, "d10_1[2]") == -1.25 &&
                std::isnan(num(s, "d10_1[3]")) && num(s, "M_bp_1_1[1]") == 7,
            "W2(getmata): getmata_087_shape -- d10_1 = .5, -1.25, . and M_bp_1_1[1] = 7 (log "
            ":1225): result[.,k] is column k of the Stata matrix, missing cells kept");
    runSrc(s, "count if missing(dinf_1)\n");
    w2check(rval(s, "N") == 2,
            "W2(getmata): getmata_087_shape -- dinf_1 has 2 missing cells (log :1225 __nm=2)");

    // mata_block_nonsubset (log :1244: Stata rc 0, ai11 = .6). invsym() is in
    // the subset, so the block runs and publishes its scalar. The old arm here
    // asserted the r(199) refusal, i.e. it pinned the divergence from Stata.
    runSrc(s,
           "capture noisily mata:\n"
           "    A = (2, 1 \\ 1, 3)\n"
           "    Ai = invsym(A)\n"
           "    st_numscalar(\"ai11\", Ai[1,1])\n"
           "end\n"
           "local __rc = _rc\n"
           "local __after = 1\n");
    w2check(rcLocal(s) == "0" && s.eval.locals.get("__after", "") == "1",
            "W2(getmata): mata_block_nonsubset -- the invsym block runs at rc 0 (log :1244). Got rc " +
                rcLocal(s));
    {
      const auto* ai = mataM(s, "Ai");
      w2check(ai && ai->data.size() == 2 && ai->data[0].size() == 2 &&
                  std::fabs(ai->data[0][0] - .6) < 1e-15 && std::fabs(ai->data[0][1] + .2) < 1e-15 &&
                  std::fabs(ai->data[1][0] + .2) < 1e-15 && std::fabs(ai->data[1][1] - .4) < 1e-15 &&
                  std::fabs(num(s, "scalar(ai11)") - .6) < 1e-15,
              "W2(getmata): mata_block_nonsubset -- invsym((2,1\\1,3)) = (.6,-.2\\-.2,.4) and ai11 = .6");
      // StataBE's own bits (impl_A probe6, %21x): -.2 and .4 sit one ulp
      // below the nearest doubles, as the sweep of the larger pivot first
      // with unfused arithmetic leaves them.
      w2check(ai && ai->data.size() == 2 && ai->data[0][0] == 0x1.3333333333333p-1 &&
                  ai->data[0][1] == -0x1.9999999999999p-3 && ai->data[1][0] == -0x1.9999999999999p-3 &&
                  ai->data[1][1] == 0x1.9999999999999p-2,
              "W2(getmata): invsym((2,1\\1,3)) reproduces StataBE's bits");
    }
    {
      // A 4x4 from impl_A probe3: StataBE's invsym() %21x bits, all 16 cells.
      static const double want[16] = {
          0x1.565daeebf885cp-1, 0x1.6ad71ad48a836p-2, -0x1.6139ea62e1c98p-2, 0x1.37359b68ab0efp-3,
          0x1.6ad71ad48a836p-2, 0x1.227f6a5a86adep-1, -0x1.3053462715cc2p-2, 0x1.321b0cc0dfb5cp-2,
          -0x1.6139ea62e1c98p-2, -0x1.3053462715cc2p-2, 0x1.854ef8b1ccceep-1, -0x1.1ff22306a1015p-2,
          0x1.37359b68ab0efp-3, 0x1.321b0cc0dfb5cp-2, -0x1.1ff22306a1015p-2, 0x1.071c108bad213p+0};
      const bool ran = rcOf(s, "mata: Bi = invsym((2.5, -1.3, 0.7, 0.2 \\ -1.3, 3.1, 0.4, -0.6 \\ "
                               "0.7, 0.4, 1.9, 0.3 \\ 0.2, -0.6, 0.3, 1.2))") == "0";
      const auto* bi = mataM(s, "Bi");
      bool same = ran && bi && bi->data.size() == 4;
      for (size_t i = 0; same && i < 4; ++i)
        for (size_t j = 0; same && j < 4; ++j) same = bi->data[i].size() == 4 && bi->data[i][j] == want[i * 4 + j];
      w2check(same, "W2(getmata): invsym of a 4x4 reproduces StataBE's bits in every cell");
    }
    // StataBE 19.5 invsym() on singular, indefinite, negative and missing input
    // (impl_A stata_probe/probe6.do): omitted columns are 0, missing spreads.
    w2check(rcOf(s, "mata: Gi = invsym((1, 2 \\ 2, 1))") == "0" && mataM(s, "Gi") &&
                mataM(s, "Gi")->data[0][0] == 1 && mataM(s, "Gi")->data[0][1] == 0 &&
                mataM(s, "Gi")->data[1][1] == 0,
            "W2(getmata): invsym of an indefinite matrix sweeps column 1 and omits column 2");
    w2check(rcOf(s, "mata: Ni = invsym((-1, 0 \\ 0, 2))") == "0" && mataM(s, "Ni") &&
                mataM(s, "Ni")->data[0][0] == 0 && mataM(s, "Ni")->data[1][1] == .5,
            "W2(getmata): invsym omits a column with a negative diagonal");
    w2check(rcOf(s, "mata: Ui = invsym((2, 1 \\ 99, 3))") == "0" && mataM(s, "Ui") &&
                std::fabs(mataM(s, "Ui")->data[0][0] - .6) < 1e-15,
            "W2(getmata): invsym reads the upper triangle");
    w2check(rcOf(s, "mata: Mi = invsym((., 0 \\ 0, 1))") == "0" && mataM(s, "Mi") &&
                std::isnan(mataM(s, "Mi")->data[1][1]),
            "W2(getmata): a missing cell makes the invsym result missing");
    {
      piet::SessionState t(workdir);
      auto e = errorOf(t, "mata:\x1D" "A = (2, 1 \\ 1, 3)\x1D" "Ai = rowsum(A)");
      const std::string msg = e.is_null() ? std::string() : e.value("message", std::string());
      w2check(!e.is_null() && piet::eval::coarseRc(e) == "199" &&
                  msg.find("rowsum") != std::string::npos && e.value("token", "") == "mata" &&
                  mataM(t, "A") == nullptr,
              "W2(getmata): a block with a function outside the subset (rowsum) is still refused as a "
              "unit, naming `mata` and the function (got: " + msg + ")");
    }
  }

  // ---- ml_fv_rowjoin (log :408): st_matrix of a 1x1 e(b), row-joined ----------
  {
    piet::SessionState s(workdir);
    runSrc(s,
           "clear\nset obs 10\ngenerate x = _n\ngenerate y = 2*x + mod(_n,3)\n"
           "quietly regress y x, noconstant\n"
           "matrix rh = e(b)\n"
           "capture noisily mata: rh2 = (J(1,1,.) \\ st_matrix(\"rh\"))\n"
           "local __rc = _rc\n");
    const piet::eval::EvalMatrix* rh2 = mataM(s, "rh2");
    w2check(rcLocal(s) == "0" && rh2 != nullptr && rh2->data.size() == 2 &&
                std::isnan(rh2->data[0][0]) && !std::isnan(rh2->data[1][0]),
            "W2(getmata): ml_fv_rowjoin -- `mata: rh2 = (J(1,1,.) \\ st_matrix(\"rh\"))` after "
            "`matrix rh = e(b)` is rc 0 with rows(rh2)=2 (log :408), the first row missing. Got rc " +
                rcLocal(s));
  }

  // ---- D3: two namespaces under one name (030 Wage_Setting.do:70-71) ---------
  {
    piet::SessionState s(workdir);
    runSrc(s,
           "matrix rchange = (9)\n"
           "mata: rchange = J(1,1,.)\n"
           "mata: rchange = (rchange \\ st_matrix(\"rchange\"))\n");
    const piet::eval::EvalMatrix* mr = mataM(s, "rchange");
    const piet::eval::EvalMatrix* sr = s.eval.findMatrix("rchange");
    w2check(mr != nullptr && mr->data.size() == 2 && std::isnan(mr->data[0][0]) &&
                mr->data[1][0] == 9,
            "W2(getmata): D3 -- Mata rchange is the 2x1 [., 9]: the bare name read the MATA "
            "vector and st_matrix() read the STATA matrix (a shared store gives [9 \\ 9])");
    w2check(sr != nullptr && sr->data.size() == 1 && sr->data[0].size() == 1 &&
                sr->data[0][0] == 9,
            "W2(getmata): D3 -- the Stata matrix rchange is still 1x1 = 9 (Mata never wrote it)");
    // getmata reads the Mata store only: a Stata matrix of that name is invisible.
    runSrc(s, "matrix onlystata = (1 \\ 2)\nclear\ncapture noisily getmata onlystata\n"
              "local __rc = _rc\n");
    w2check(rcLocal(s) == "111",
            "W2(getmata): D3 -- `getmata onlystata` with only a STATA matrix of that name is "
            "r(111), not a read of the Stata store. Got " + rcLocal(s));
  }

  // ---- the shipped block defect (BUG-696): probe1.do's shape ------------------
  // A `mata:` block whose lines look like Stata commands. Before: `mata:` was
  // refused on its own and every body line was dispatched as a STATA command,
  // `result = 1` stopping the file as "Command `result` is not supported".
  {
    piet::SessionState s(workdir);
    runSrc(s,
           "clear\nset obs 4\ngenerate x = _n\n"
           "capture noisily mata:\n"
           "display \"P|mata_block_open|\" _rc\n"
           "result = 1\n"
           "display \"P|mata_block_line|\" _rc\n"
           "end\n"
           "local __rc = _rc\n"
           "local __end = 1\n");
    w2check(rcLocal(s) == "199" && s.eval.locals.get("__end", "") == "1",
            "W2(getmata): BUG-696 -- probe1's block is refused as ONE unit at r(199) and the "
            "do-file reaches the line after `end`. Got rc " + rcLocal(s));
    w2check(mataM(s, "result") == nullptr && rcOf(s, "confirm variable result") == "111",
            "W2(getmata): BUG-696 -- no `result` exists afterwards, neither as a Mata matrix nor "
            "as a Stata variable (nothing in the block ran, not even the M0-legal line)");
  }
  {
    // ...and an all-M0 block outside capture no longer stops the file (p2).
    piet::SessionState s(workdir);
    runSrc(s,
           "matrix all_results = J(3, 3, 1)\n"
           "mata:\n"
           "    result = st_matrix(\"all_results\")\n"
           "end\n"
           "local __end = 1\n");
    w2check(s.eval.locals.get("__end", "") == "1" && mataM(s, "result") != nullptr,
            "W2(getmata): BUG-696 -- the 087 block shape runs outside capture and the file "
            "continues past `end`");
  }

  // ---- clear mata / clear all empty the Mata store; bare clear does not -----
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("mata: keep1 = J(2,1,1)", s);
    (void)piet::dispatch("clear", s);
    w2check(mataM(s, "keep1") != nullptr,
            "W2(getmata): bare `clear` leaves the Mata store alone (it drops data only)");
    w2check(rcOf(s, "clear mata") == "0" && s.eval.mataMatrices.empty(),
            "W2(getmata): `clear mata` empties the Mata store");
    (void)piet::dispatch("mata: keep2 = J(2,1,1)", s);
    (void)piet::dispatch("matrix S = (1)", s);
    (void)piet::dispatch("clear all", s);
    w2check(s.eval.mataMatrices.empty(), "W2(getmata): `clear all` empties the Mata store too");
  }

  // ---- refusals by name, each with a real rc (never rc 1) --------------------
  {
    piet::SessionState s(workdir);
    auto e = errorOf(s, "mata rchange = J(1,1,.)");
    w2check(!e.is_null() && piet::eval::coarseRc(e) == "199" &&
                e.value("reason", std::string()).find("mata_without_colon") != std::string::npos,
            "W2(getmata): `mata` without the colon is refused by name at r(199)");
    w2check(rcOf(s, "mata: X = rowsum(J(2,2,1))") == "199",
            "W2(getmata): a one-liner outside M0 (rowsum) is r(199)");
    w2check(rcOf(s, "mata: st_numscalar(\"x\", 1)") == "0" &&
                num(s, "scalar(x)") == 1.0,
            "W2(getmata): st_numscalar stores its real scalar result");
    w2check(rcOf(s, "mata: st_numscalar(\"bad name\", 1)") != "0",
            "W2(getmata): st_numscalar rejects an invalid scalar name");
    w2check(rcOf(s, "mata: bad = ((1, 2) \\ 3)") == "3200",
            "W2(getmata): a non-conformable row join is Mata's r(3200)");
    w2check(rcOf(s, "mata: bad = nosuch :+ 1") == "3499",
            "W2(getmata): an undefined Mata name is Mata's r(3499)");
    (void)piet::dispatch("mata: v = J(2,1,5)", s);
    w2check(rcOf(s, "getmata v, update") == "198" && rcOf(s, "getmata v, id(k)") == "198",
            "W2(getmata): getmata's update and id() options are refused by name at r(198)");
    w2check(rcOf(s, "getmata") == "100", "W2(getmata): a bare `getmata` is r(100) (getlist "
                                         "required, getmata.ado)");
  }

  // ---- the svmat extension path (BUG-697; TASKS T6a) --------------------------
  {
    piet::SessionState s(workdir);
    runSrc(s,
           "clear\nset obs 6\ngenerate k = _n\n"
           "matrix M2 = (1\\2\\3\\4\\5\\6\\7\\8)\n"
           "capture noisily svmat M2\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "_N") == 8 && num(s, "M21[8]") == 8 &&
                std::isnan(num(s, "k[7]")),
            "W2(getmata): svmat_extends -- an 8-row matrix onto 6 obs is rc 0, _N 8, M21[8]=8, "
            "and the old variable is MISSING on the new rows (was rc 1 `cobra produced non-JSON "
            "output`). Got rc " + rcLocal(s) + ", _N " + fmt(num(s, "_N")));
    runSrc(s, "matrix M3 = (1\\2\\3)\ncapture noisily svmat M3\nlocal __rc = _rc\n");
    w2check(rcLocal(s) == "0" && num(s, "M31[3]") == 3 && std::isnan(num(s, "M31[4]")),
            "W2(getmata): svmat_fewer -- a 3-row matrix onto 8 obs pads MISSING, never 0");
  }
  {
    // BUG-697 (3): extending a dataset that holds a STRING variable keeps it a
    // string (the unflagged append would have promoted it to f64).
    piet::SessionState s(workdir);
    runSrc(s, "clear\nset obs 2\ngenerate s = \"a\"\ngenerate n = _n\nset obs 4\n");
    const std::string isStr = rcOf(s, "confirm string variable s");
    (void)piet::dispatch("count if s == \"\"", s);
    const double blanks = rval(s, "N");
    w2check(num(s, "_N") == 4 && isStr == "0" && blanks == 2 && std::isnan(num(s, "n[4]")),
            "W2(getmata): BUG-697 -- `set obs 4` on a 2-obs dataset with a string variable keeps "
            "it a string (confirm string rc " + isStr + ", 2 blank rows: " + fmt(blanks) +
                ") and pads the numeric one with missing");
  }

  // ---- BUG-698: a /* */ comment that spans lines joins them -----------------
  // Stata treats the comment, newline included, as white space: the text after
  // a `*/` on a later line continues the command the comment opened in (oracle
  // w2c_ml_getmata.do:324-325 / log :1216-1225, and 087
  // gen_sensitivity_stats_1.do:185-194). eval/tokenizer.cpp stripComments joins
  // it onto the opening line with one space and leaves empty placeholders, so
  // doexec's line numbers still name source lines. Before, the tail ran as a
  // command of its own (`Command + is not supported`).
  {
    // (b) the tail completes an expression.
    piet::SessionState s(workdir);
    const auto r = piet::doexec::executeDoSource(
        "clear\nset obs 1\ngenerate a = 1 /*\n*/ + 2\n", s, piet::dispatch);
    w2check(!r.stoppedAtLine && num(s, "a[1]") == 3,
            "W2(getmata): BUG-698 (b) -- `generate a = 1 /*` + `*/ + 2` is ONE command, a = 3 "
            "(was a stop at `Command + is not supported`). Got a = " + fmt(num(s, "a[1]")));
  }
  {
    // (c) both comment forms run, and the multi-line comment adds nothing to
    // the line count: a later error stops at the same line as in the file
    // written with each comment on one line. (doexec's `stopped at line` counts
    // statements -- blank and `*` lines are not counted -- so the reference is
    // that one-line twin, not the raw source line.) A dataset is loaded first:
    // with none, an unknown command stops on "No dataset is loaded" instead.
    piet::SessionState s(workdir);
    const auto r = piet::doexec::executeDoSource(
        "clear\nset obs 1\ndisplay 1 /*\nx */\ndisplay 2\ndisplay 3 /*\ny */ + 1\n"
        "nosuchcmd_bug698\n",
        s, piet::dispatch);
    piet::SessionState t(workdir);
    const auto ref = piet::doexec::executeDoSource(
        "clear\nset obs 1\ndisplay 1 /* x */\ndisplay 2\ndisplay 3 /* y */ + 1\n"
        "nosuchcmd_bug698\n",
        t, piet::dispatch);
    const std::string shown = joinText(displayed(r));
    w2check(shown == "1,2,4" && r.stoppedAtLine && ref.stoppedAtLine &&
                *r.stoppedAtLine == *ref.stoppedAtLine &&
                r.stoppedAtReason.value_or("").find("nosuchcmd_bug698") != std::string::npos,
            "W2(getmata): BUG-698 (c) -- `display 1 /*`+`x */` and `display 3 /*`+`y */ + 1` "
            "print 1, 2, 4, and the stop is at the unknown command on the same line number as "
            "the one-line twin (got `" + shown + "`, stopped at " +
                (r.stoppedAtLine ? std::to_string(*r.stoppedAtLine) : std::string("-")) + " vs " +
                (ref.stoppedAtLine ? std::to_string(*ref.stoppedAtLine) : std::string("-")) + ")");
  }
  {
    // (d) preprocessor-corpus case 25's shape: a `//` after the joined tail
    // still ends it.
    const auto lines = piet::eval::preprocessDo("/* a */ reg y x /* b\nstill in b */ sum z // done");
    w2check(lines.size() == 2 && trimmed(lines[0]) == "reg y x sum z" && lines[1].empty(),
            "W2(getmata): BUG-698 (d) -- `reg y x /* b` + `still in b */ sum z // done` is one "
            "line `reg y x sum z` plus an empty placeholder (got " +
                std::to_string(lines.size()) + " line(s), first `" +
                (lines.empty() ? std::string() : lines[0]) + "`)");
    piet::SessionState s(workdir);
    const auto r = piet::doexec::executeDoSource(
        "clear\nset obs 1\n/* a */ generate d = 5 /* b\nstill in b */ + 1 // + 100\n", s,
        piet::dispatch);
    w2check(!r.stoppedAtLine && num(s, "d[1]") == 6,
            "W2(getmata): BUG-698 (d) -- through the do-file path d = 6: the tail's `// + 100` is "
            "dropped. Got d = " + fmt(num(s, "d[1]")));
  }
  {
    // (e) unchanged: a `/*` that never closes blanks to the end of the file.
    const auto lines = piet::eval::preprocessDo("display 1\n/* never closed\ndisplay 2\nnosuch");
    w2check(lines.size() == 4 && lines[0] == "display 1" && lines[1].empty() &&
                lines[2].empty() && lines[3].empty(),
            "W2(getmata): BUG-698 (e) -- an unclosed `/*` still blanks every later line");
    piet::SessionState s(workdir);
    const auto r = piet::doexec::executeDoSource(
        "display 1\n/* never closed\ndisplay 2\nnosuchcmd_bug698\n", s, piet::dispatch);
    w2check(joinText(displayed(r)) == "1" && !r.stoppedAtLine,
            "W2(getmata): BUG-698 (e) -- only `display 1` runs and nothing stops");
  }
  {
    // (f) opened mid-line, closed two lines later: joined across the placeholder.
    const auto lines =
        piet::eval::preprocessDo("generate f = 1 /* start\nmiddle\nend */ + 10\nnosuch");
    w2check(lines.size() == 4 && lines[0] == "generate f = 1 + 10" && lines[1].empty() &&
                lines[2].empty() && lines[3] == "nosuch",
            "W2(getmata): BUG-698 (f) -- a comment closing two lines down joins its tail onto "
            "the opening line; both covered lines are empty placeholders (got first `" +
                (lines.empty() ? std::string() : lines[0]) + "`)");
    piet::SessionState s(workdir);
    const auto r = piet::doexec::executeDoSource(
        "clear\nset obs 1\ngenerate f = 1 /* start\nmiddle\nend */ + 10\nnosuchcmd_bug698\n",
        s, piet::dispatch);
    piet::SessionState t(workdir);
    const auto ref = piet::doexec::executeDoSource(
        "clear\nset obs 1\ngenerate f = 1 /* start middle end */ + 10\nnosuchcmd_bug698\n", t,
        piet::dispatch);
    w2check(num(s, "f[1]") == 11 && r.stoppedAtLine && ref.stoppedAtLine &&
                *r.stoppedAtLine == *ref.stoppedAtLine &&
                r.stoppedAtReason.value_or("").find("nosuchcmd_bug698") != std::string::npos,
            "W2(getmata): BUG-698 (f) -- f = 11 and the unknown command stops at the same line "
            "number as the one-line twin (got f = " + fmt(num(s, "f[1]")) + ", stopped at " +
                (r.stoppedAtLine ? std::to_string(*r.stoppedAtLine) : std::string("-")) + " vs " +
                (ref.stoppedAtLine ? std::to_string(*ref.stoppedAtLine) : std::string("-")) + ")");
  }

  // ---- the fold itself: order and pass-through ------------------------------
  {
    const std::vector<std::string> in = {
        "program define pg", "  display 1", "end",   "input x", "1", "end",
        "quietly mata:",     "a = 1",       "",      "end",     "display 2"};
    const std::vector<std::string> out = piet::mata::captureMataBlocks(in);
    const bool ok = out.size() == 8 && out[0] == "program define pg" && out[2] == "end" &&
                    out[3] == "input x" && out[5] == "end" &&
                    out[6] == std::string("quietly mata:\x1D") + "a = 1" && out[7] == "display 2";
    w2check(ok,
            "W2(getmata): captureMataBlocks passes `program ... end` and `input ... end` through "
            "untouched and folds a prefixed `mata:` block into one line");
  }

  // ---- auto-parallel: the Mata store never crosses a worker boundary ---------
  // A worker's spec carries globals, scalars, locals and programs, never
  // mataMatrices, and nothing comes back. Review repro (serial a[1] = 5,
  // PIET_PARALLEL_AUTO=1 gave 1 at rc 0): `mata : a = ...` x3 was approved as a
  // region once the table had a `mata` row. These legs ask planRegion directly,
  // as testP9AutoRecognizer does, so no worker is spawned.
  {
    auto plan = [&](const std::vector<std::string>& stmts,
                    const std::vector<std::string>& after) {
      piet::SessionState session(workdir);
      return piet::parallel::planRegion(stmts, after, session);
    };
    const auto pm = plan({"mata : a = J(1,1,5)", "mata : b = J(1,1,6)", "mata : c = J(1,1,7)"},
                         {"getmata a, force", "display \"RESULT a[1] = \" a[1]"});
    w2check(!pm.approved && pm.refusalReason.find("Mata store") != std::string::npos,
            "W2(getmata): auto-parallel refuses a run of `mata :` one-liners by name (got " +
                (pm.approved ? std::string("approved") : pm.refusalReason) + ")");
    const auto pc = plan({"mata: a = J(1,1,5)", "mata: b = J(1,1,6)", "mata: c = J(1,1,7)"}, {});
    w2check(!pc.approved && pc.refusalReason.find("Mata store") != std::string::npos,
            "W2(getmata): auto-parallel refuses `mata:` with the colon attached, by the same name "
            "(got " + (pc.approved ? std::string("approved") : pc.refusalReason) + ")");
    const auto pg = plan({"getmata q, force", "display \"x\"", "display \"y\""}, {});
    w2check(!pg.approved && pg.refusalReason.find("getmata") != std::string::npos,
            "W2(getmata): auto-parallel refuses getmata -- a worker has no Mata store to read "
            "(got " + (pg.approved ? std::string("approved") : pg.refusalReason) + ")");
  }
  // BUG-699: a table row that writes session state (wms = 2) names no target,
  // and planRegion only decides session-globals by name, so each of these was
  // approved as a write-free reader. Measured before the fix: `renvars CD \ cd`
  // in an approved region left the variable named CD (serial: cd), rc 0.
  // `notes` is absent on purpose: the table lists it twice and the map keeps
  // the FIRST row, a dataset writer that the successor walk already handles.
  {
    const std::vector<std::string> writers = {
        "renvars CD \\ cd",       "tolower AB",        "move AB CD",
        "recast double AB",       "labmask AB, values(CD)",
        "renvarlab AB, lower",    "mkmat AB, matrix(M)"};
    std::string approved;
    for (const auto& w : writers) {
      piet::SessionState session(workdir);
      const auto p = piet::parallel::planRegion({w, "display \"x\"", "display \"y\""}, {}, session);
      const std::string head = w.substr(0, w.find(' '));
      if (p.approved || p.refusalReason.find("`" + head + "`") == std::string::npos ||
          p.refusalReason.find("not tracked by name") == std::string::npos)
        approved += (approved.empty() ? "" : ", ") + head;
    }
    w2check(approved.empty(),
            "W2(getmata): BUG-699 -- every session-state writer the effect table cannot name "
            "refuses its region by name (not refused: " + approved + ")");
    piet::SessionState session(workdir);
    const auto ok =
        piet::parallel::planRegion({"clear", "global g 1", "clear"}, {}, session);
    w2check(ok.approved,
            "W2(getmata): BUG-699 control -- a NAMED session-global write is still decided by "
            "name and approved (got " + ok.refusalReason + ")");
  }
  // BUG-699 (review round 2): `clear` was looked up by its head alone, so every
  // subcommand rode the bare-`clear` self-seeding row. Review repro (serial
  // r(111) "vector or matrix a not found"; PIET_PARALLEL_AUTO=1 approved lines
  // 2-4 and printed a1 = 1 at rc 0): `mata: a = J(1,1,1)` / `clear mata` /
  // `clear` x3 / `getmata a, force`. `clear all` gave the same, and on main
  // `clear all` let `matrix list M` succeed after a region that cleared M.
  {
    const std::vector<std::string> subs = {"mata", "all", "*", "matrix", "results", "programs"};
    std::string approved;
    for (const auto& sub : subs) {
      piet::SessionState session(workdir);
      const auto p =
          piet::parallel::planRegion({"clear " + sub, "clear", "clear"}, {}, session);
      if (p.approved || p.refusalReason.find("`clear " + sub + "`") == std::string::npos)
        approved += (approved.empty() ? "" : ", ") + ("clear " + sub);
    }
    w2check(approved.empty(),
            "W2(getmata): BUG-699 -- every `clear` subcommand refuses its region by name, it "
            "clears a store a worker never gives back (not refused: " + approved + ")");
    piet::SessionState session(workdir);
    const auto bare = piet::parallel::planRegion({"clear", "clear", "clear"}, {}, session);
    w2check(bare.approved,
            "W2(getmata): BUG-699 control -- bare `clear` keeps its self-seeding row and is "
            "still approved (got " + bare.refusalReason + ")");
  }
  // BUG-699 sibling: `scalar drop` / `matrix drop` were effect-free management
  // forms. Measured on main: `clear` / `scalar drop s` / `clear` x2 approved as
  // a region, then `capture confirm scalar s` gave rc 0 (serial r(111)). A drop
  // now writes each name it lists, so a later mention keeps it serial; `_all`
  // refuses by name; `global drop x` defines $drop and is decided on `drop`.
  {
    auto plan = [&](const std::vector<std::string>& stmts,
                    const std::vector<std::string>& after) {
      piet::SessionState session(workdir);
      return piet::parallel::planRegion(stmts, after, session);
    };
    const auto sd = plan({"clear", "scalar drop s", "clear"}, {"clear", "confirm scalar s"});
    w2check(!sd.approved && sd.refusalReason.find("`s`") != std::string::npos,
            "W2(getmata): BUG-699 -- `scalar drop s` in a region is decided by the name s, and a "
            "later `confirm scalar s` keeps it serial (got " +
                (sd.approved ? std::string("approved") : sd.refusalReason) + ")");
    const auto md = plan({"clear", "matrix drop M", "clear"}, {"clear", "matrix list M"});
    w2check(!md.approved && md.refusalReason.find("`M`") != std::string::npos,
            "W2(getmata): BUG-699 -- `matrix drop M` is decided by the name M (got " +
                (md.approved ? std::string("approved") : md.refusalReason) + ")");
    const auto da = plan({"clear", "scalar drop _all", "clear"}, {});
    w2check(!da.approved && da.refusalReason.find("`scalar drop _all`") != std::string::npos,
            "W2(getmata): BUG-699 -- `scalar drop _all` refuses by name (got " +
                (da.approved ? std::string("approved") : da.refusalReason) + ")");
    const auto gd = plan({"clear", "global drop x", "clear"}, {"display \"$drop\""});
    w2check(!gd.approved && gd.refusalReason.find("`drop`") != std::string::npos,
            "W2(getmata): BUG-699 -- `global drop x` writes the global named drop (got " +
                (gd.approved ? std::string("approved") : gd.refusalReason) + ")");
    const auto un = plan({"clear", "scalar drop s", "clear"}, {"clear", "display \"x\""});
    w2check(un.approved,
            "W2(getmata): BUG-699 control -- a drop nobody reads afterwards is still approved "
            "(got " + un.refusalReason + ")");
  }

  // Mata exp() has Stata's underflow edge: 0 for an argument below -709, libm's
  // subnormal from -709 up (StataBE 19.5 %21x, 2026-10-27, scratch-r150/tail_fixes/
  // stata_probe/il.do, ex3.do: `mata: exp(-717.5)` 0, exp(-709) +0.8bfe55de02338X-3ff).
  {
    piet::SessionState s(workdir);
    runSrc(s, "clear\nmata: __tl_m = (exp(-717.5) \\ exp(-709))\n");
    const piet::eval::EvalMatrix* m = mataM(s, "__tl_m");
    w2check(m != nullptr && m->data.size() == 2 && m->data[0][0] == 0.0 &&
                m->data[1][0] == 0x0.8bfe55de02338p-1022,
            "W2(getmata): Mata exp() is 0 below -709 and libm's subnormal at -709, as in StataBE");
  }

  fs::remove_all(workdir, ec);
}
