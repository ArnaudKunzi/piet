// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `predictframe` -- BUG-516 (a second `predict` REPLACED the live frame
// with the estimation-time frame, at rc 0), BUG-517 (`predict ... if` filled
// every row), BUG-518 (the fit-time frame of a model fitted on a dirty virtual
// head was never written, so `generate` then `test`/`lincom`/`predict` opened a
// file nobody had made) and the review follow-up that `if e(sample)` -- which
// this build could not evaluate then -- was accepted and filled every row
// (served since e(sample) T2, section 8); and BUG-519
// (predict after a WEIGHTED fit wrote the unweighted model's predictions). Every arm
// reports through w2check() and names the oracle arm it rests on; every arm
// says what main answered before the fix.
//
// Oracles (StataBE 19.5, frozen):
//   tests/eval/golden/aer_w2/w2_smalls.log  section C  predict_twice / predict_twice_r1
//   tests/eval/golden/aer_b4/predict-e-xbd.log         P_res_* + Q_idc, C_if_half
//   tests/eval/golden/aer_b4/esttab-beta.log:488       `predict ... if e(sample)` served
//   tests/eval/golden/aer_b4/predict-e-xbd.log         J_build_areg (predict after [aw=] served)
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

// rc + message of the LAST error block of one dispatch ("0" when there is none).
// Shape-safe: a non-object block is skipped, never indexed.
std::pair<std::string, std::string> w2pfRun(const std::string& cmd, piet::SessionState& session) {
  std::string rc = "0", msg;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) != "error") continue;
      rc = piet::eval::coarseRc(b);
      msg = b.value("message", std::string());
    }
  } catch (const std::exception& e) {
    rc = "EXC";
    msg = e.what();
  }
  return {rc, msg};
}

bool w2pfHas(piet::SessionState& session, const std::string& name) {
  try {
    piet::Datastore* ds = session.datastore();
    return ds != nullptr && ds->has(name);
  } catch (...) {
    return false;
  }
}

// A numeric column of the LIVE head, read straight from the Datastore -- the bytes
// a save would persist. false when the column is absent or not numeric.
bool w2pfCol(piet::SessionState& session, const std::string& name, std::vector<double>& out) {
  out.clear();
  try {
    piet::Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has(name)) return false;
    if (ds->dtype(name) != "f64") return false;
    const double* v = ds->col(name).f64();
    if (v == nullptr) return false;
    out.assign(v, v + ds->nRows());
    return true;
  } catch (...) {
    return false;
  }
}

long long w2pfRows(piet::SessionState& session) {
  try {
    piet::Datastore* ds = session.datastore();
    return ds == nullptr ? -1 : static_cast<long long>(ds->nRows());
  } catch (...) {
    return -1;
  }
}

int w2pfCols(piet::SessionState& session) {
  try {
    piet::Datastore* ds = session.datastore();
    return ds == nullptr ? -1 : ds->nCols();
  } catch (...) {
    return -1;
  }
}

// w2_smalls.do's own synthetic frame, line for line (12 rows; x is missing in rows
// 5 and 11 but is not a regressor of section C, so its fit uses all 12).
void w2pfSmallsData(piet::SessionState& session) {
  for (const char* s : {"clear", "set obs 12", "generate id = ceil(_n/3)",
                        "generate t  = mod(_n-1,3)+1", "generate x  = mod(_n*7,11)",
                        "generate x2 = mod(_n*3,5)", "generate y  = 2*x - x2 + mod(_n,4)",
                        "replace x = . in 5", "replace x = . in 11"}) {
    (void)piet::dispatch(s, session);
  }
}

}  // namespace

void testW2PredictFrame() {
  std::printf("[selftest] --- W2 predictframe: predict ADDS a column to the LIVE frame "
              "(w2_smalls.log section C) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2predictframe").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // == 1. section C verbatim ==================================================
  // MAIN TODAY: `confirm variable r1 foo p2` is r(111) -- the second predict ran
  // the engine on the estimation-time frame and adopted ITS output as the head,
  // so r1 and foo were gone (describe: id t x x2 y p2) while predict said rc 0.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t", session);
    auto p1 = w2pfRun("predict r1, residuals", session);
    std::vector<double> r1Before;
    const bool gotR1 = w2pfCol(session, "r1", r1Before);
    (void)piet::dispatch("generate foo = 1", session);
    const int colsBefore = w2pfCols(session);
    auto p2 = w2pfRun("predict p2, xb", session);
    auto conf = w2pfRun("confirm variable r1 foo p2", session);
    w2check(p1.first == "0" && p2.first == "0" && conf.first == "0",
            "W2 predictframe: regress / predict r1, residuals / generate foo = 1 / predict p2, xb "
            "/ confirm variable r1 foo p2 is rc 0 (w2_smalls.log C: PREC|predict_twice|0|). Main "
            "answered r(111): the second predict replaced the live frame with the "
            "estimation-time one. Got predict rc " + p1.first + "/" + p2.first + ", confirm rc " +
                conf.first + " `" + conf.second + "`");

    std::vector<double> r1After, foo, p2v, y;
    const bool haveAll = w2pfCol(session, "r1", r1After) && w2pfCol(session, "foo", foo) &&
                         w2pfCol(session, "p2", p2v) && w2pfCol(session, "y", y);
    // predict_twice_r1: Stata's r(sum) 4.68455255032e-07 is FLOAT-storage noise
    // around an exact zero (OLS residuals with a constant sum to 0). Since BUG-924
    // PIET stores the same floats, so the sum is Stata's own, at rtol 1e-9.
    double sum = 0.0;
    size_t nonMiss = 0;
    for (double v : r1After) {
      if (!std::isnan(v)) {
        sum += v;
        ++nonMiss;
      }
    }
    w2check(haveAll && nonMiss == 12 &&
                std::fabs(sum - 4.68455255032e-07) <= 1e-9 * 4.68455255032e-07,
            "W2 predictframe: after the second predict r1 still summarizes over N 12 with "
            "Stata's float sum (w2_smalls.log C: PREC|predict_twice_r1| 4.68455255032e-07|12| -- "
            "float noise around 0, which PIET's float column reproduces). Main: `variable r1 "
            "not found`. Got N " + std::to_string(nonMiss) + " sum " + std::to_string(sum));

    bool r1Same = gotR1 && r1Before.size() == r1After.size() && !r1After.empty();
    for (size_t i = 0; r1Same && i < r1After.size(); ++i) r1Same = r1Before[i] == r1After[i];
    bool fooOnes = foo.size() == 12;
    for (size_t i = 0; fooOnes && i < foo.size(); ++i) fooOnes = foo[i] == 1.0;
    w2check(r1Same && fooOnes && w2pfCols(session) == colsBefore + 1,
            "W2 predictframe: ...and nothing else moved -- r1 is bit-identical to what the first "
            "predict wrote, foo is still 1 in every row, and the frame grew by exactly ONE "
            "column (section C's `confirm variable r1 foo p2`; the one-column rule is BUG-155's). "
            "Main: both columns absent. Got cols " + std::to_string(colsBefore) + " -> " +
                std::to_string(w2pfCols(session)));

    // xb + residual = y in any least-squares fit: the second predict answered from
    // the FITTED coefficients, over the live rows. Both are float columns (BUG-924),
    // so the identity holds to float rounding of each, not to 1e-9.
    bool identity = haveAll && p2v.size() == y.size() && r1After.size() == y.size();
    for (size_t i = 0; identity && i < y.size(); ++i) {
      identity = std::fabs((p2v[i] + r1After[i]) - y[i]) < 1e-6 * (1.0 + std::fabs(y[i]));
    }
    w2check(identity,
            "W2 predictframe: p2 + r1 == y row for row -- the second predict used the fitted "
            "model on the live frame (section C's two columns, side by side). Main: r1 is gone, "
            "so the identity cannot even be formed");
  }

  // == 2. rows dropped since the fit do not come back =========================
  // Same adoption as section C, seen from the row side (no separate oracle arm:
  // it is the C mechanism). MAIN TODAY (measured): `drop if id == 4` -> N 9, then
  // `predict p3, xb` -> N 12 again, rc 0 -- the dropped rows were resurrected.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t", session);
    (void)piet::dispatch("predict r1, residuals", session);
    (void)piet::dispatch("drop if id == 4", session);
    const long long nDropped = w2pfRows(session);
    auto p3 = w2pfRun("predict p3, xb", session);
    std::vector<double> p3v;
    const bool got = w2pfCol(session, "p3", p3v);
    size_t filled = 0;
    for (double v : p3v) filled += std::isnan(v) ? 0 : 1;
    w2check(p3.first == "0" && nDropped == 9 && w2pfRows(session) == 9 && got && filled == 9 &&
                w2pfHas(session, "r1"),
            "W2 predictframe: a predict after `drop if id == 4` leaves _N at 9 and fills those 9 "
            "rows (w2_smalls.log C is the mechanism: predict adopted the estimation-time frame). "
            "Main: _N went 9 -> 12, the dropped observations back in the data at rc 0. Got rc " +
                p3.first + " N " + std::to_string(nDropped) + " -> " +
                std::to_string(w2pfRows(session)) + " filled " + std::to_string(filled));
  }

  // == 3. a restricted fit, head moved: `--fit-data` is the FITTED sample ======
  // MAIN TODAY: r1/foo lost, as in section C. The values half is BUG-163's rule:
  // coefficients from the fitted rows, a value for every live row.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t if id < 4", session);
    auto a = w2pfRun("predict r1, residuals", session);
    (void)piet::dispatch("generate foo = 1", session);
    auto b = w2pfRun("predict p2, xb", session);
    std::vector<double> r1, p2v, y, id;
    const bool have = w2pfCol(session, "r1", r1) && w2pfCol(session, "p2", p2v) &&
                      w2pfCol(session, "y", y) && w2pfCol(session, "id", id) &&
                      w2pfHas(session, "foo");
    bool ok = have && p2v.size() == 12;
    for (size_t i = 0; ok && i < p2v.size(); ++i) {
      ok = !std::isnan(p2v[i]);                                        // every live row
      // the FITTED betas; float columns (BUG-924), so to float rounding
      if (ok && id[i] < 4) ok = std::fabs((p2v[i] + r1[i]) - y[i]) < 1e-6 * (1.0 + std::fabs(y[i]));
    }
    w2check(a.first == "0" && b.first == "0" && ok,
            "W2 predictframe: after `regress ... if id < 4`, predict / generate / predict keeps "
            "r1 and foo (w2_smalls.log C) and the second predict still answers from the "
            "RESTRICTED fit over all 12 rows (p2 + r1 == y on the fitted rows). Main: r1 and foo "
            "gone. Got rc " + a.first + "/" + b.first);
  }

  // == 4. absorbed FE: successive predicts keep the earlier ones ===============
  // aer_b4/predict-e-xbd.log: P_res_xb 0, P_res_residuals 0, then Q_idc 0 -- a
  // column predicted EARLIER in the loop is still there for the `gen` after it.
  // MAIN TODAY (measured): after areg, `confirm variable d1 foo d2` is r(111).
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly areg y x2, absorb(id)", session);
    auto a = w2pfRun("predict d1, residuals", session);
    std::vector<double> d1Before;
    (void)w2pfCol(session, "d1", d1Before);
    (void)piet::dispatch("generate foo = 1", session);
    auto b = w2pfRun("predict d2, xb", session);
    std::vector<double> d1After;
    const bool have = w2pfCol(session, "d1", d1After) && w2pfHas(session, "foo") &&
                      w2pfHas(session, "d2");
    bool same = have && !d1After.empty() && d1Before.size() == d1After.size();
    for (size_t i = 0; same && i < d1After.size(); ++i) {
      same = d1Before[i] == d1After[i] || (std::isnan(d1Before[i]) && std::isnan(d1After[i]));
    }
    w2check(a.first == "0" && b.first == "0" && same,
            "W2 predictframe: after an absorbed-FE fit a second predict keeps the first and the "
            "column made in between (predict-e-xbd.log P_res_xb/P_res_residuals rc 0, then "
            "Q_idc rc 0 reads an earlier predict). Main: d1 and foo gone at rc 0. Got rc " +
                a.first + "/" + b.first + " `" + b.second + "`");

    // The absorbed-FE path re-fits the LIVE frame, which is the fitted model only
    // while the model's columns are what they were. Once one moved, Stata still
    // answers (rc 0, from e(b)); this build cannot, and says so BY NAME instead of
    // predicting from a model nobody fitted. MAIN TODAY: rc 0, and the `replace`
    // itself silently undone (the estimation-time frame came back).
    (void)piet::dispatch("replace x2 = 99 if id == 1 & t == 1", session);
    auto c = w2pfRun("predict d3, xb", session);
    std::vector<double> x2;
    const bool gotX2 = w2pfCol(session, "x2", x2);
    w2check(c.first == "198" && c.second.find("absorbed fixed effects") != std::string::npos &&
                !w2pfHas(session, "d3") && w2pfHas(session, "d1") && w2pfHas(session, "foo") &&
                gotX2 && !x2.empty() && x2[0] == 99.0,
            "W2 predictframe: ...and once a model variable has been replaced, predict after the "
            "absorbed-FE fit is a NAMED r(198) that writes nothing and leaves the replace in "
            "place (a PIET limit, stated: Stata is rc 0 here). Main: rc 0 with the edit to x2 "
            "reverted. Got rc " + c.first + " `" + c.second + "`");
  }

  // == 5. the ML branch shares the adoption ===================================
  // Same handler, same section C mechanism. MAIN TODAY (measured): after logit,
  // `confirm variable a1 foo a2` is r(111).
  {
    piet::SessionState session(workdir);
    for (const char* s : {"clear", "set obs 40", "generate x1 = mod(_n*7,11)",
                          "generate x2 = mod(_n*3,5)", "generate d  = mod(_n*7,3) > 0"}) {
      (void)piet::dispatch(s, session);
    }
    (void)piet::dispatch("quietly logit d x1 x2", session);
    auto a = w2pfRun("predict a1", session);
    std::vector<double> a1Before;
    (void)w2pfCol(session, "a1", a1Before);
    (void)piet::dispatch("generate foo = 1", session);
    auto b = w2pfRun("predict a2, xb", session);
    std::vector<double> a1After, a2;
    const bool have = w2pfCol(session, "a1", a1After) && w2pfCol(session, "a2", a2) &&
                      w2pfHas(session, "foo");
    // pr = invlogit(xb): the second predict came from the SAME fitted logit. Both
    // are float columns (BUG-924): the link holds to float rounding.
    bool link = have && a1After.size() == 40 && a2.size() == 40 && a1Before == a1After;
    for (size_t i = 0; link && i < a2.size(); ++i) {
      link = std::fabs(a1After[i] - 1.0 / (1.0 + std::exp(-a2[i]))) < 1e-6;
    }
    w2check(a.first == "0" && b.first == "0" && link,
            "W2 predictframe: logit / predict a1 / generate foo / predict a2, xb keeps a1 and "
            "foo, and a1 == invlogit(a2) (w2_smalls.log C mechanism on the ML branch of the same "
            "handler). Main: a1 and foo gone. Got rc " + a.first + "/" + b.first + " `" +
                b.second + "`");
  }

  // == 6. BUG-518: a column made BEFORE the first predict ======================
  // The fit read a dirty VIRTUAL head straight from the worker; the resident
  // `generate` then advanced that frame in place, and every replay of the
  // fit-time path -- `predict`, `test`, `lincom` -- opened a file nobody had
  // written. Stata is rc 0 with both columns (w2_smalls.log C's shape with the
  // `generate` moved in front of the first predict; BUG-518.do). setLastModel now
  // pins the frame to disk at the fit. MAIN TODAY (measured 2026-09-19):
  // `[data_io] cannot open .../bysort_gen_*.hbin`, rc 1, for all three; the
  // lane's first cut still refused predict BY NAME (r(198)), which this arm
  // accepted -- it no longer does: Stata serves this.
  {
    // The reference: the same fit and the same predict with NO generate between.
    std::vector<double> ref;
    {
      piet::SessionState s0(workdir);
      w2pfSmallsData(s0);
      (void)piet::dispatch("quietly regress y x2 t", s0);
      (void)piet::dispatch("predict p1, xb", s0);
      (void)w2pfCol(s0, "p1", ref);
    }
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t", session);
    (void)piet::dispatch("generate foo = 1", session);
    auto t = w2pfRun("test x2", session);
    auto l = w2pfRun("lincom x2 + t", session);
    auto a = w2pfRun("predict p1, xb", session);
    std::vector<double> p1;
    const bool got = w2pfCol(session, "p1", p1);
    bool same = got && ref.size() == 12 && p1.size() == 12;
    for (size_t i = 0; same && i < p1.size(); ++i) same = std::fabs(p1[i] - ref[i]) < 1e-9;
    w2check(a.first == "0" && t.first == "0" && l.first == "0" && w2pfHas(session, "foo") &&
                same,
            "W2 predictframe (BUG-518): regress / generate foo / test x2 / lincom x2 + t / "
            "predict p1, xb are ALL rc 0, foo is kept, and p1 equals the prediction of the same "
            "fit with no generate in between (Stata rc 0: w2_smalls.log C's columns with the "
            "generate moved first; BUG-518.do). Main: `[data_io] cannot open` on all three. Got "
            "rc test " + t.first + " lincom " + l.first + " predict " + a.first + " `" +
                a.second + "`");
  }

  // == 7. BUG-517: `predict ... if` ===========================================
  // aer_b4/predict-e-xbd.log C_if_half: `predict double P_e_half if half==1, e` is
  // rc 0 with "(135 missing values generated)" -- the rows failing `if` are
  // MISSING. MAIN TODAY (measured): the qualifier was dropped; 0 missing.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t", session);
    auto full = w2pfRun("predict pall, xb", session);
    auto part = w2pfRun("predict pif if t == 1, xb", session);
    std::vector<double> pall, pif, t;
    const bool have = w2pfCol(session, "pall", pall) && w2pfCol(session, "pif", pif) &&
                      w2pfCol(session, "t", t);
    bool rule = have && pif.size() == 12 && pall.size() == 12;
    size_t missing = 0;
    for (size_t i = 0; rule && i < pif.size(); ++i) {
      if (t[i] == 1.0) {
        rule = std::fabs(pif[i] - pall[i]) < 1e-9;  // passing rows: the ordinary prediction
      } else {
        rule = std::isnan(pif[i]);  // the rest are missing
        ++missing;
      }
    }
    w2check(full.first == "0" && part.first == "0" && rule && missing == 8,
            "W2 predictframe: `predict pif if t == 1, xb` fills the 4 rows with t==1 and leaves "
            "the other 8 missing (predict-e-xbd.log C_if_half: rc 0, `(135 missing values "
            "generated)`). Main: all 12 filled -- the `if` never reached the handler. Got rc " +
                part.first + " missing " + std::to_string(missing) + " `" + part.second + "`");

    // A bad `if` creates nothing (the mask runs before the output is adopted).
    auto bad = w2pfRun("predict pbad if nosuchvar == 1, xb", session);
    w2check(bad.first != "0" && !w2pfHas(session, "pbad") && w2pfHas(session, "pif"),
            "W2 predictframe: a `predict ... if` whose expression fails writes NO column and "
            "keeps the earlier ones (C_if_half's qualifier is evaluated, so it can fail). Main: "
            "rc 0, pbad filled in every row. Got rc " + bad.first);
  }

  // == 8. `if e(sample)` is SERVED from the fit's own rows (e(sample) T2) =======
  // Stata's e(sample) is a per-ROW mask of the estimation sample (esttab-beta.
  // log:488 `predict double D_ry if e(sample), residuals` is the served shape).
  // History: on 2026-09-19 main filled all 12 rows (e(sample) read as an absent
  // scalar, `.`, which is true in every row); this section then pinned a named
  // r(198) for every form. e(sample) T2 (internal notes esample)
  // installs the engine's admitted rows as a hidden mask column, so after
  // `regress y x2 t if id < 4` (rows 1-9) the forms are SERVED: predict fills 9,
  // count 9, generate leaves 3 missing, summarize N 9, and `local s = e(sample)`
  // is observation 1's value, 1 (w2_esample.log:337 B13, :168 B1, :240 B7, :288
  // B11, :376 B17 -- the same shapes on the oracle's own frame). The frozen main
  // binary answers r(198) for each (the pin this section used to hold).
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t if id < 4", session);
    const int cols = w2pfCols(session);
    auto p = w2pfRun("predict p if e(sample), xb", session);
    std::vector<double> pv;
    long long pFilled = -1;
    if (w2pfCol(session, "p", pv)) {
      pFilled = 0;
      for (double v : pv) pFilled += std::isnan(v) ? 0 : 1;
    }
    auto c = w2pfRun("count if e(sample)", session);
    (void)piet::dispatch("scalar __w2pf_n = r(N)", session);
    const std::string cN = session.eval.scalars.find("__w2pf_n") != nullptr
                               ? *session.eval.scalars.find("__w2pf_n")
                               : std::string("<unset>");
    auto g = w2pfRun("generate z = 1 if e(sample)", session);
    std::vector<double> zv;
    long long zMissing = -1;
    if (w2pfCol(session, "z", zv)) {
      zMissing = 0;
      for (double v : zv) zMissing += std::isnan(v) ? 1 : 0;
    }
    auto sm = w2pfRun("summarize y if e(sample)", session);
    (void)piet::dispatch("scalar __w2pf_sn = r(N)", session);
    const std::string sN = session.eval.scalars.find("__w2pf_sn") != nullptr
                               ? *session.eval.scalars.find("__w2pf_sn")
                               : std::string("<unset>");
    auto m = w2pfRun("local s = e(sample)", session);
    const std::string sv = session.eval.locals.get("s", "<unset>");
    w2check(p.first == "0" && pFilled == 9 && c.first == "0" && (cN == "9" || cN == "9.0") &&
                g.first == "0" && zMissing == 3 && sm.first == "0" &&
                (sN == "9" || sN == "9.0") && m.first == "0" && sv == "1" &&
                w2pfCols(session) == cols + 2,
            "W2 predictframe (e(sample) T2): after `regress y x2 t if id < 4`, `predict p if "
            "e(sample), xb` fills the 9 fitted rows, `count if e(sample)` is 9, `generate z = 1 "
            "if e(sample)` leaves 3 missing, `summarize y if e(sample)` has N 9 and `local s = "
            "e(sample)` is observation 1's 1 (esttab-beta.log:488; w2_esample.log B13/B1/B7/B11/"
            "B17). Main: r(198) for each. Got predict " + p.first + " `" + p.second + "` filled " +
                std::to_string(pFilled) + ", count " + c.first + " r(N) " + cN + ", generate " +
                g.first + " missing " + std::to_string(zMissing) + ", summarize " + sm.first +
                " N " + sN + ", local " + m.first + " s=" + sv);
  }
  // The ONE kept refusal: a model PIET records no mask for (pca: the handler is
  // not wired for the engine's sample) is a NAMED r(198) that writes nothing --
  // never a guess, and never the mask an earlier fit left on the rows. PIET
  // limit by design (SPEC §5.5/§7); no Stata arm reads e(sample) after pca.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("quietly regress y x2 t if id < 4", session);
    auto before = w2pfRun("count if e(sample)", session);
    (void)piet::dispatch("quietly pca x2 t", session);
    const int cols = w2pfCols(session);
    auto g = w2pfRun("generate z = 1 if e(sample)", session);
    auto c = w2pfRun("count if e(sample)", session);
    const bool named = g.second.find("e(sample)") != std::string::npos &&
                       g.second.find("pca") != std::string::npos;
    w2check(before.first == "0" && g.first == "198" && named && !w2pfHas(session, "z") &&
                c.first == "198" && w2pfCols(session) == cols,
            "W2 predictframe (e(sample) T2): after `pca` -- a fit with no recorded sample mask "
            "-- `generate z = 1 if e(sample)` and `count if e(sample)` are a NAMED r(198) naming "
            "pca, and nothing is written, although the regress before it left its mask on the "
            "rows. Main: r(198) for the regress too (count before = " + before.first +
                "). Got generate " + g.first + " `" + g.second + "`, count " + c.first);
  }

  // == 9. BUG-519: predict after a WEIGHTED fit is never the unweighted model ===
  // The engine's predict re-fits with no weight input, so every branch of runPredict
  // re-estimated the UNWEIGHTED model. MAIN (measured 2026-09-22): `regress y x2 t
  // [aw=w]` / `predict p` rc 0 with p off the model's own _b[] xb by up to 0.38
  // (12 rows); the same under [fw=]; `logit yb x2 [pw=w], vce(robust)` / `predict pl`
  // rc 0. BUG-1260: after a weighted regress the statistics built from e(b) and y
  // alone are served without the engine (StataBE 19.5 scratch-laneA/agents/
  // p-margins/s6.do: `regress y x [aw=wv]` predict xb/residuals rc 0) -- on the
  // first predict (head unmoved) and after a `generate` (head moved) alike, each
  // the model's own _b[] xb; after the weighted logit the refusal stays (r(198),
  // named, nothing written). The control: an unweighted refit predicts again.
  {
    piet::SessionState session(workdir);
    w2pfSmallsData(session);
    (void)piet::dispatch("generate w = mod(_n, 3) + 1", session);
    (void)piet::dispatch("generate yb = mod(_n*5, 3) == 0", session);
    const int cols0 = w2pfCols(session);
    auto fa = w2pfRun("quietly regress y x2 t [aw=w]", session);
    auto pa = w2pfRun("predict pa", session);
    (void)piet::dispatch("generate foo = 1", session);
    auto pm = w2pfRun("predict pm, residuals", session);
    (void)piet::dispatch("generate double xa = _b[_cons] + _b[x2]*x2 + _b[t]*t", session);
    auto ff = w2pfRun("quietly regress y x2 t [fw=w]", session);
    auto pf = w2pfRun("predict pf, xb", session);
    (void)piet::dispatch("generate double xf = _b[_cons] + _b[x2]*x2 + _b[t]*t", session);
    auto fl = w2pfRun("quietly logit yb x2 [pw=w], vce(robust)", session);
    auto pl = w2pfRun("predict pl", session);
    std::vector<double> vpa, vpm, vxa, vpf, vxf, vy;
    const bool read = w2pfCol(session, "pa", vpa) && w2pfCol(session, "pm", vpm) &&
                      w2pfCol(session, "xa", vxa) && w2pfCol(session, "pf", vpf) &&
                      w2pfCol(session, "xf", vxf) && w2pfCol(session, "y", vy);
    // `predict` stores c(type) float: compared at float precision.
    auto near = [](double a, double b) {
      return std::isnan(a) ? std::isnan(b) : std::fabs(a - b) <= 1e-6 * (1.0 + std::fabs(b));
    };
    bool own = read;
    for (size_t r = 0; own && r < vpa.size(); ++r)
      own = near(vpa[r], vxa[r]) && near(vpm[r], vy[r] - vxa[r]) && near(vpf[r], vxf[r]);
    const bool named = pl.second.find("weighted fit") != std::string::npos && !w2pfHas(session, "pl");
    w2check(fa.first == "0" && ff.first == "0" && fl.first == "0" && pa.first == "0" &&
                pm.first == "0" && pf.first == "0" && own && pl.first == "198" && named &&
                w2pfCols(session) == cols0 + 6,
            "W2 predictframe: predict after regress [aw=] (head unmoved and moved) and regress "
            "[fw=] is the weighted model's own xb / residual (BUG-1260), after logit [pw=] a "
            "NAMED r(198) that writes nothing (BUG-519). Main: rc 0 with the UNWEIGHTED "
            "model's predictions written. Got fits " + fa.first + "/" + ff.first + "/" + fl.first +
                ", predicts " + pa.first + "/" + pm.first + "/" + pf.first + "/" + pl.first +
                (own ? "" : " (values not the model's own)") + " `" + pl.second + "`");

    auto fu = w2pfRun("quietly regress y x2 t", session);
    auto pu = w2pfRun("predict pu, xb", session);
    w2check(fu.first == "0" && pu.first == "0" && w2pfHas(session, "pu"),
            "W2 predictframe: an UNWEIGHTED refit after the refused weighted predicts predicts "
            "again (the refusal keys on the model's weight, not on the session). Got rc " +
                fu.first + "/" + pu.first);
  }

  fs::remove_all(workdir, ec);
}
