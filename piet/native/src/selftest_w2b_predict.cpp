// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `predictstats` -- the predict statistic floors after regress / areg /
// xtreg,fe / reghdfe (BUG-596/597/598). This TU supplies the scaffold's
// testW2bPredictStats() (selftest_w2.h; the stub TU it replaces is no longer in
// the CMake source list).
//
// Oracle: tests/eval/golden/aer_w2/w2b_estim.log, the predict_* arms (do-file
// lines 50-108), on the do-file's own fixture; aer_b4/predict-e-xbd.log for the
// estimation-sample rule (OOS: a starred statistic is missing off e(sample)).
// w2b_estim.do generates FLOAT columns and PIET has no float storage, so each
// value is rounded to float here and loaded as a literal (selftest_w2_estimopts'
// recipe). Stata stores regress/areg/xtreg predictions as float and reghdfe's as
// double, so the float-typed columns are rounded to float before summing
// (TOLERANCE.md §3 rule 3); every sum/sd is then held to T2 (rtol 1e-9, with the
// near-zero floor atol = 1e-9 x sum|v|).
//
// Every arm FAILS on main before this change: the statistic was refused, `r` was
// refused, xb after an absorbed-FE fit was the within prediction (sum ~0), `e`/`d`
// refused under PIET's own wording, `predict double` read back as float, and
// `predict z` over an existing z replaced it at rc 0.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

struct PReply {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, concatenated
};

PReply prun(piet::SessionState& session, const std::string& cmd) {
  PReply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
        r.msg += b.value("message", std::string());
      } else if (t == "display") {
        r.text += b.value("text", std::string());
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

std::string pfmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

struct ColStats {
  bool ok = false;
  double sum = std::nan("");
  double sd = std::nan("");
  double absSum = 0.0;
  long long n = 0;       // non-missing
  long long nMiss = 0;
};

// r(sum)/r(sd) of `name` as Stata's summarize computes them, over the column's
// values rounded to float when Stata stores it as float.
ColStats colStats(piet::SessionState& s, const std::string& name, bool asFloat) {
  ColStats c;
  try {
    piet::Datastore* ds = s.datastore();
    if (ds == nullptr || !ds->has(name) || ds->col(name).f64() == nullptr) return c;
    const double* p = ds->col(name).f64();
    std::vector<double> v;
    for (int64_t i = 0; i < ds->nRows(); ++i) {
      double x = p[i];
      if (std::isnan(x)) {
        ++c.nMiss;
        continue;
      }
      if (asFloat) x = static_cast<double>(static_cast<float>(x));
      v.push_back(x);
    }
    c.n = static_cast<long long>(v.size());
    if (v.size() < 2) return c;
    double sum = 0.0;
    for (double x : v) {
      sum += x;
      c.absSum += std::fabs(x);
    }
    const double mean = sum / static_cast<double>(v.size());
    double ss = 0.0;
    for (double x : v) ss += (x - mean) * (x - mean);
    c.sum = sum;
    c.sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
    c.ok = true;
  } catch (...) {
  }
  return c;
}

// T2 with the near-zero floor (TOLERANCE.md §2 T2, §3 rule 2).
bool t2(double got, double want, double scale) {
  return std::isfinite(got) && std::fabs(got - want) <= 1e-9 * (std::fabs(want) + scale);
}

struct Arm {
  const char* opt;   // the spelling the oracle typed
  double sum;        // r(sum)
  double sd;         // r(sd)
};

// Run `predict pv, <opt>` and compare with the oracle arm `label`.
void checkArm(piet::SessionState& s, const std::string& label, const Arm& a, bool asFloat) {
  (void)prun(s, "capture drop pv");
  const PReply r = prun(s, std::string("predict pv, ") + a.opt);
  const ColStats c = colStats(s, "pv", asFloat);
  w2check(r.rc == "0" && c.ok && t2(c.sum, a.sum, c.absSum) && t2(c.sd, a.sd, 0.0),
          "w2b_estim " + label + ": `predict pv, " + a.opt + "` rc 0, r(sum) " + pfmt(a.sum) +
              ", r(sd) " + pfmt(a.sd) + ". Got rc " + r.rc + " sum=" + pfmt(c.sum) +
              " sd=" + pfmt(c.sd) + " " + r.msg);
}

// w2b_estim.do lines 5-16, with every float column rounded to float.
void seedFixture(piet::SessionState& s) {
  (void)prun(s, "clear");
  (void)prun(s, "set obs 60");
  (void)prun(s, "generate double id = ceil(_n/6)");
  (void)prun(s, "generate double t = mod(_n-1,6)+1");
  (void)prun(s, "generate double x1 = .");
  (void)prun(s, "generate double x2 = .");
  (void)prun(s, "generate double y = .");
  auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
  for (int n = 1; n <= 60; ++n) {
    const double id = std::ceil(n / 6.0);
    const double x1 = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
    const double x2 = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
    const double y = fl(1.0 + 2.0 * x1 - x2 + id / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
    const std::string in = " in " + std::to_string(n);
    (void)prun(s, "quietly replace x1 = " + pfmt(x1) + in);
    (void)prun(s, "quietly replace x2 = " + pfmt(x2) + in);
    (void)prun(s, "quietly replace y = " + pfmt(y) + in);
  }
  (void)prun(s, "xtset id t");
}


// ── BUG-924/925/926: storage type, cooksd, restricted absorbed fits ────────
// Oracle: w2aa_predict_type_restricted.log (StataBE 19.5, captured 2026-10-20;
// internal capture), on its own
// fixture (w2b_estim.do's recipe under `set type float`). Each arm reads the
// record's `: type`, its missing count and the STORED values of rows 1, 3 and 60
// (%21x): exact for a float/int column -- the rounding IS the claim -- and rtol
// 1e-9 for a double one; `+1.0000000000000X+3ff` is missing.

// A `%21x` field (`+1.dda8fe0000000X+003`) as the double it prints; NaN for
// Stata's missing.
double hexField(const std::string& f) {
  if (f == "+1.0000000000000X+3ff") return std::nan("");
  const size_t x = f.find('X');
  std::string c = f.substr(0, 1) + "0x" + f.substr(1, x - 1) + "p";
  std::string e = f.substr(x + 1);
  long ex = std::strtol(e.c_str() + 1, nullptr, 16);
  c += (e[0] == '-' ? "-" : "+") + std::to_string(ex);
  return std::strtod(c.c_str(), nullptr);
}

struct TypedArm {
  const char* arm;      // the w2aa record
  int logLine;          // w2aa_predict_type_restricted.log line of the record
  const char* cmd;      // the predict as the oracle typed it (new variable `pv`)
  const char* type;     // `: type pv`
  long long nMiss;      // count if missing(pv)
  const char* row[3];   // %21x pv[1], pv[3], pv[60]
};

std::string typeOf(piet::SessionState& s, const std::string& v) {
  (void)prun(s, "local w2bt : type " + v);
  const PReply d = prun(s, "display \"T=`w2bt'\"");
  return d.text.size() > 2 ? d.text.substr(2) : std::string();
}

void checkTypedArm(piet::SessionState& s, const TypedArm& a) {
  (void)prun(s, "capture drop pv");
  const PReply r = prun(s, a.cmd);
  const std::string t = r.rc == "0" ? typeOf(s, "pv") : std::string();
  const ColStats c = colStats(s, "pv", false);
  std::string bad;
  const bool dbl = std::string(a.type) == "double";
  try {
    piet::Datastore* ds = s.datastore();
    const double* p = (ds != nullptr && ds->has("pv")) ? ds->col("pv").f64() : nullptr;
    const int rows[3] = {0, 2, 59};
    for (int i = 0; i < 3; ++i) {
      const double want = hexField(a.row[i]);
      const double got = p != nullptr ? p[rows[i]] : std::nan("1");
      const bool ok = std::isnan(want) ? std::isnan(got)
                      : dbl ? std::fabs(got - want) <= 1e-9 * std::fabs(want)
                            : got == want;
      if (!ok) bad += " row" + std::to_string(rows[i] + 1) + "=" + pfmt(got);
    }
  } catch (const std::exception& e) {
    bad += e.what();
  }
  w2check(r.rc == "0" && t == a.type && c.nMiss == a.nMiss && bad.empty(),
          std::string("w2aa ") + a.arm + " (w2aa_predict_type_restricted.log:" +
              std::to_string(a.logLine) + "): `" + a.cmd + "` rc 0, " + a.type + ", " +
              std::to_string(a.nMiss) + " missing, rows 1/3/60 " + a.row[0] + " " + a.row[1] +
              " " + a.row[2] + ". Got rc " + r.rc + " " + t + " " + std::to_string(c.nMiss) +
              " missing" + bad + " " + r.msg);
}

void testW2aaPredictTypeRestricted(const std::string& workdir) {
  piet::SessionState s(workdir);
  for (const char* c :
       {"clear", "set type float", "set obs 60", "generate id = ceil(_n/6)",
        "generate t  = mod(_n-1,6)+1", "generate x1 = mod(_n*7,11) + mod(_n,5)/10",
        "generate x2 = mod(_n*3,5) + mod(_n*13,7)/10",
        "generate y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5", "xtset id t"}) {
    (void)prun(s, c);
  }
  // (a) regress: c(type) float by default, the declared type on request, values
  // stored as that type holds them (BUG-924), cooksd (BUG-925), `residual`.
  // main: double values under a float label, `predict float`/`int` r(198),
  // `set type double` labelled float, cooksd r(198).
  (void)prun(s, "quietly regress y x1 x2");
  const TypedArm reg[] = {
      {"a_p_default", 86, "predict pv", "float", 0,
       {"+1.dda8fe0000000X+003", "+1.458d040000000X+004", "+1.0e247e0000000X+003"}},
      {"a_pf_float_xb", 133, "predict float pv, xb", "float", 0,
       {"+1.dda8fe0000000X+003", "+1.458d040000000X+004", "+1.0e247e0000000X+003"}},
      {"a_pi_int_xb", 177, "predict int pv, xb", "int", 0,
       {"+1.c000000000000X+003", "+1.4000000000000X+004", "+1.0000000000000X+003"}},
      {"a_r_residual", 289, "predict pv, residual", "float", 0,
       {"-1.964ba20000000X-001", "+1.8bae280000000X-002", "+1.3218ac0000000X+001"}},
      {"a_cooksd", 430, "predict pv, cooksd", "float", 0,
       {"+1.09b3420000000X-008", "+1.6a8f8a0000000X-009", "+1.25a9440000000X-004"}},
  };
  for (const TypedArm& a : reg) checkTypedArm(s, a);
  {
    // a_eq_p_pd (:144): the float column is not the double one (main: 60).
    (void)prun(s, "capture drop pv pd");
    (void)prun(s, "predict pv");
    (void)prun(s, "predict double pd, xb");
    (void)prun(s, "count if pv == pd");
    const PReply n = prun(s, "display r(N)");
    w2check(n.text == "0",
            "w2aa a_eq_p_pd (w2aa_predict_type_restricted.log:144): `count if p == pd` is 0 -- "
            "the default prediction is stored as float. Got `" + n.text + "`");
    (void)prun(s, "capture drop pd");
  }
  (void)prun(s, "set type double");
  checkTypedArm(s, {"a_p_settype_double", 228, "predict pv", "double", 0,
                    {"+1.dda8fe130a418X+003", "+1.458d035c8666aX+004", "+1.0e247ed554793X+003"}});
  (void)prun(s, "set type float");
  // cooksd after a restricted regress fills every row with y and x (a_if_cooksd).
  (void)prun(s, "quietly regress y x1 x2 if t>=3");
  checkTypedArm(s, {"a_if_cooksd", 486, "predict pv, cooksd", "float", 0,
                    {"+1.ef307a0000000X-008", "+1.e8ae8c0000000X-00b", "+1.04687e0000000X-003"}});

  // (b)-(d) FU-768: after a RESTRICTED absorbed fit, xb on every row (x b +
  // _cons) and the absorbed statistics on e(sample) only (rows with t < 3 are
  // missing; row 1 is one). main: xb/d/xbd r(198), residuals/e rc 1.
  (void)prun(s, "quietly areg y x1 x2 if t>=3, absorb(id)");
  const TypedArm areg[] = {
      {"b_if_xb", 784, "predict pv, xb", "float", 0,
       {"+1.e060680000000X+003", "+1.48c0920000000X+004", "+1.08e7060000000X+003"}},
      {"b_if_residuals", 808, "predict pv, residuals", "float", 20,
       {"+1.0000000000000X+3ff", "+1.0d0c1a0000000X+000", "+1.4b82880000000X-001"}},
      {"b_if_d", 858, "predict pv, d", "float", 20,
       {"+1.0000000000000X+3ff", "-1.bab30e0000000X-001", "+1.e85bdc0000000X+000"}},
      {"b_if_xbd", 882, "predict pv, xbd", "float", 20,
       {"+1.0000000000000X+3ff", "+1.3aeafa0000000X+004", "+1.45f2820000000X+003"}},
      {"d_if_q_xb", 931, "predict pv if t>=4, xb", "float", 30,
       {"+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.08e7060000000X+003"}},
  };
  for (const TypedArm& a : areg) checkTypedArm(s, a);
  {
    // FU-972: once a model variable changed, the restricted absorbed fit cannot be
    // refit on the live frame; the refusal is a named r(198), never the engine's
    // rc 1 (main: rc 1).
    (void)prun(s, "capture drop pv");
    (void)prun(s, "quietly replace x2 = 99 in 60");
    const PReply r = prun(s, "predict pv, residuals");
    w2check(r.rc == "198" && s.datastore() != nullptr && !s.datastore()->has("pv"),
            "FU-972: predict after a restricted areg whose x2 changed since the fit is a named "
            "r(198) that writes nothing. Got rc " + r.rc + " " + r.msg);
    (void)prun(s, "quietly replace x2 = mod(60*3,5) + mod(60*13,7)/10 in 60");
  }
  (void)prun(s, "quietly xtreg y x1 x2 if t>=3, fe");
  const TypedArm xt[] = {
      {"c_xt_if_xb", 1088, "predict pv, xb", "float", 0,
       {"+1.e060680000000X+003", "+1.48c0920000000X+004", "+1.08e7060000000X+003"}},
      {"c_xt_if_xbu", 1112, "predict pv, xbu", "float", 20,
       {"+1.0000000000000X+3ff", "+1.3aeafa0000000X+004", "+1.45f2820000000X+003"}},
      {"c_xt_if_u", 1137, "predict pv, u", "float", 20,
       {"+1.0000000000000X+3ff", "-1.bab30e0000000X-001", "+1.e85bdc0000000X+000"}},
      {"c_xt_if_e", 1161, "predict pv, e", "float", 20,
       {"+1.0000000000000X+3ff", "+1.0d0c1a0000000X+000", "+1.4b82880000000X-001"}},
      {"c_xt_if_ue", 1184, "predict pv, ue", "float", 0,
       {"-1.c1c23c0000000X-001", "+1.7d94960000000X-003", "+1.470e900000000X+001"}},
  };
  for (const TypedArm& a : xt) checkTypedArm(s, a);
  (void)prun(s, "quietly reghdfe y x1 x2 if t>=3, absorb(id) resid");
  const TypedArm hd[] = {
      {"c_hdfe_if_xb", 1321, "predict pv, xb", "double", 0,
       {"+1.e06067b6b2082X+003", "+1.48c092d42e588X+004", "+1.08e705e7092a0X+003"}},
      {"c_hdfe_if_xbd", 1346, "predict pv, xbd", "double", 20,
       {"+1.0000000000000X+3ff", "+1.3aeafa67833feX+004", "+1.45f28177f643bX+003"}},
      {"c_hdfe_if_d", 1371, "predict pv, d", "double", 20,
       {"+1.0000000000000X+3ff", "-1.bab30d956313cX-001", "+1.e85bdc8768cdaX+000"}},
      {"c_hdfe_if_residuals", 1396, "predict pv, residuals", "double", 20,
       {"+1.0000000000000X+3ff", "+1.0d0c1987cc01eX+000", "+1.4b8288809bc4bX-001"}},
  };
  for (const TypedArm& a : hd) checkTypedArm(s, a);
}

}  // namespace

void testW2bPredictStats() {
  std::printf("[selftest] --- W2B predict floors: regress/areg/xtreg,fe/reghdfe statistics ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bpredict").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState s(workdir);
  seedFixture(s);

  // ── after regress (w2b_estim.log:354-374) ───────────────────────────────
  // main: `r` and every influence statistic refused (r(198)/r(111)).
  (void)prun(s, "quietly regress y x1 x2");
  {
    const double rSum = -2.79396772385e-07, rSd = 1.359643323132518;
    const Arm arms[] = {
        {"r", rSum, rSd},
        {"stdp", 18.11026358604431, .0681625443929327},
        {"stdr", 80.89069700241089, .0154516425296313},
        {"stdf", 85.042635679245, .014634148695575},
        {"rsta", -.0658009052276611, 1.009555983721723},
        {"rstandard", -.0658009052276611, 1.009555983721723},
        {"rstu", -.0583417266607285, 1.021927146594942},
        {"hat", 2.99999999627471, .0217221812929585},
        {"lev", 2.99999999627471, .0217221812929585},
        {"sc", rSum, rSd},
        {"score", rSum, rSd},
    };
    for (const Arm& a : arms) checkArm(s, std::string("predict_reg_") + a.opt, a, true);
  }
  // Stata's own refusal (main: rc 198 under PIET's "not supported" wording).
  for (const char* o : {"e", "d"}) {
    (void)prun(s, "capture drop pv");
    const PReply r = prun(s, std::string("predict pv, ") + o);
    w2check(r.rc == "198" && r.msg == std::string("option ") + o + " not allowed",
            std::string("w2b_estim predict_reg_") + o + ": `predict pv, " + o +
                "` after regress is r(198) \"option " + o + " not allowed\". Got rc " + r.rc +
                " `" + r.msg + "`");
  }
  // predict_double (w2b_estim.log:395): `: type pv` is double (main: float).
  {
    (void)prun(s, "capture drop pv");
    const PReply r = prun(s, "predict double pv, xb");
    const PReply t = prun(s, "local w2bt : type pv");
    const PReply d = prun(s, "display \"T=`w2bt'\"");
    w2check(r.rc == "0" && d.text == "T=double",
            "w2b_estim predict_double: `predict double pv, xb` makes a double. Got rc " + r.rc +
                " `" + d.text + "` " + r.msg + t.msg);
  }
  // BUG-597: predict never replaces an existing variable (Stata: r(110)
  // "variable z already defined"; main: rc 0 with z overwritten by the fit).
  {
    (void)prun(s, "capture drop w2bz");
    (void)prun(s, "generate double w2bz = 7");
    const PReply r = prun(s, "predict w2bz, xb");
    const ColStats c = colStats(s, "w2bz", false);
    w2check(r.rc == "110" && c.ok && c.sum == 7.0 * 60 && c.sd == 0.0,
            "BUG-597: `predict w2bz, xb` over an existing w2bz is r(110) and leaves it untouched. "
            "Got rc " + r.rc + " sum=" + pfmt(c.sum) + " " + r.msg);
    (void)prun(s, "capture drop w2bz");
  }

  // ── after areg (w2b_estim.log:411-417) ──────────────────────────────────
  // main: xb was the within prediction (sum 1.8e-15, sd 6.196); xbd/d/r/dr/stdp refused.
  const double kXbSum = 760.4000012278557, kXbSd = 6.324230807648225;
  const double kXbdSum = 760.400000423193, kXbdSd = 6.354626833324493;
  const double kDSum = -4.47034835815e-07, kDSd = .9847485096904912;
  const double kESum = 5.82076609135e-08, kESd = .9409848936561221;
  const double kUeSum = -8.19563865662e-08, kUeSd = 1.362050734650581;
  const double kStdpSum = 13.82311514019966, kStdpSd = .05273997309116;
  (void)prun(s, "quietly areg y x1 x2, absorb(id)");
  {
    const Arm arms[] = {
        {"xb", kXbSum, kXbSd},   {"xbd", kXbdSum, kXbdSd}, {"d", kDSum, kDSd},
        {"r", kESum, kESd},      {"dr", kUeSum, kUeSd},    {"stdp", kStdpSum, kStdpSd},
    };
    for (const Arm& a : arms) checkArm(s, std::string("predict_areg_") + a.opt, a, true);
  }

  // ── after xtreg, fe (w2b_estim.log:430-436) ─────────────────────────────
  // main: xb within (sum ~0); xbu/u/e/ue/r/stdp refused.
  (void)prun(s, "quietly xtreg y x1 x2, fe");
  {
    const Arm arms[] = {
        {"xb", kXbSum, kXbSd}, {"xbu", kXbdSum, kXbdSd}, {"u", kDSum, kDSd},
        {"e", kESum, kESd},    {"ue", kUeSum, kUeSd},    {"r", kUeSum, kUeSd},
        {"stdp", kStdpSum, kStdpSd},
    };
    for (const Arm& a : arms) checkArm(s, std::string("predict_xtfe_") + a.opt, a, true);
  }
  for (const char* o : {"xbd", "d"}) {
    // predict-e-xbd.log B_stat_xbd / B_stat_d: r(198) "option xbd not allowed".
    (void)prun(s, "capture drop pv");
    const PReply r = prun(s, std::string("predict pv, ") + o);
    w2check(r.rc == "198" && r.msg == std::string("option ") + o + " not allowed",
            std::string("predict-e-xbd B_stat_") + o + ": `predict pv, " + o +
                "` after xtreg, fe is r(198) \"option " + o + " not allowed\". Got rc " + r.rc +
                " `" + r.msg + "`");
  }

  // ── after reghdfe, resid (w2b_estim.log:449-455): stored as DOUBLE ──────
  // main: xb within; xbd/d/r/stdp refused.
  (void)prun(s, "quietly reghdfe y x1 x2, absorb(id) resid");
  {
    const Arm arms[] = {
        {"xb", 760.4000010192393, 6.32423076945397},
        {"xbd", 760.4000010192394, 6.354626810403614},
        {"d", 1.05832009822e-13, .9847485074304924},
        {"r", -4.47558656802e-14, .9409848939300512},
        {"stdp", 13.82311513783313, .0527399725830837},
    };
    for (const Arm& a : arms) checkArm(s, std::string("predict_reghdfe_") + a.opt, a, false);
    (void)prun(s, "capture drop pv");
    (void)prun(s, "predict pv, xbd");
    (void)prun(s, "local w2bt : type pv");
    const PReply d = prun(s, "display \"T=`w2bt'\"");
    w2check(d.text == "T=double",
            "predict-e-xbd type_xbd: reghdfe's predictions are double. Got `" + d.text + "`");
    // predict-e-xbd.log P_res_xb/xbd/d/residuals: `predict float Q_<stat>` after
    // reghdfe is rc 0 and writes double (type_xbd reads Q_xbd). main: rc 1, nothing written.
    for (const char* o : {"xb", "xbd", "d", "residuals"}) {
      (void)prun(s, "capture drop pv");
      const PReply r = prun(s, std::string("predict float pv, ") + o);
      (void)prun(s, "local w2bt : type pv");
      const PReply t = prun(s, "display \"T=`w2bt'\"");
      w2check(r.rc == "0" && t.text == "T=double",
              std::string("predict-e-xbd P_res_") + o + ": `predict float pv, " + o +
                  "` after reghdfe is rc 0 and the column is double. Got rc " + r.rc + " `" +
                  t.text + "` " + r.msg);
    }
  }
  // predict_reghdfe_xbd_noresid (w2b_estim.log:467): r(9) (main: r(198)).
  (void)prun(s, "quietly reghdfe y x1 x2, absorb(id)");
  {
    (void)prun(s, "capture drop pv");
    const PReply r = prun(s, "predict pv, xbd");
    const ColStats c = colStats(s, "pv", false);
    w2check(r.rc == "9" && !c.ok && c.n == 0,
            "w2b_estim predict_reghdfe_xbd_noresid: `predict pv, xbd` after reghdfe without "
            "resid is r(9) and writes nothing. Got rc " + r.rc + " " + r.msg);
  }

  // ── the estimation sample (predict-e-xbd.log OOS; [R] areg postestimation) ─
  // A row outside e(sample) (y missing) gets xb (unstarred: x is there) but no d
  // and no dresiduals. main: d refused, and xb (within) was missing there.
  (void)prun(s, "capture drop pv");
  (void)prun(s, "quietly replace y = . in 60");
  (void)prun(s, "quietly areg y x1 x2, absorb(id)");
  {
    (void)prun(s, "predict pv, xb");
    const ColStats xb = colStats(s, "pv", false);
    (void)prun(s, "capture drop pv");
    (void)prun(s, "predict pv, d");
    const ColStats d = colStats(s, "pv", false);
    (void)prun(s, "capture drop pv");
    (void)prun(s, "predict pv, dr");
    const ColStats dr = colStats(s, "pv", false);
    w2check(xb.n == 60 && d.n == 59 && dr.n == 59,
            "e(sample): after `areg` with y missing in row 60, xb fills all 60 rows while d and "
            "dresiduals fill the 59 of e(sample). Got " + std::to_string(xb.n) + "/" +
                std::to_string(d.n) + "/" + std::to_string(dr.n));
  }

  testW2aaPredictTypeRestricted(workdir);

  fs::remove_all(workdir, ec);
}
