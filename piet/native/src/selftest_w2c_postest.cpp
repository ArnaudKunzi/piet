// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2C lane `postest` -- r(table) after estimation, and test / testparm / lincom /
// nlcom on factor-level coefficients. Oracle: StataBE 19.5 capture
// tests/eval/golden/aer_w2/w2c_postest.{do,log}, families (1)-(5), one check block
// per PREC arm. Family (6) (grouped `i.(c g)`) belongs to another task.
//
// Registered like every other W2 lane: testW2cPostest() is declared in
// selftest_w2.h, called from runSelftest() and compiled into piet_selftest, so
// `piet_native --selftest` runs it (the separate piet_selftest_w2c executable of
// attempts 1-5 is gone). Each check also reports through w2check(), and the block
// prints its own count.
//
// FIXTURE. The .do generates its data with plain `generate`, i.e. FLOAT storage:
// Stata computes each value in double and stores float(value). PIET stores
// doubles and has no float() function, so the same `generate` lines give values
// that differ in the 8th digit and every oracle number moves at ~1e-7 — outside
// TOLERANCE.md's T2 1e-9. TOLERANCE.md §3.3 (float-storage variables are compared
// after rounding to float) is honoured by building the float-rounded columns here,
// in C++, exactly as Stata does (double arithmetic on the already-stored floats,
// then a cast to float), and importing them as exact decimal doubles. Measured:
// test_2c F 4.78021791070026 vs the oracle's 4.780217910700236 (5e-15).
//
// This is an EMULATED-FLOAT fixture: the literal .do run through PIET stores doubles
// and lands ~3e-7 away on every numeric arm (the default storage type of `generate`
// is a separate gap, named in the task report). A PASS here means "reproduces the
// oracle on the data Stata actually stored", not "the literal .do reproduces".
//
// READ SURFACES. The oracle reads its results through the macro/arith layer
// (`display rowsof(r(table))`, `display %18.0g r(table)[1,1]`, `r(b)[1,1]`,
// `: rowfullnames`/`: colfullnames`, `local x : display ...` rc 198/109). Section
// (E) below reads them the same way; sections (1)-(5) read the posted matrices
// through the r()/e() stores. `display %18.0g _b[A]` reads _b[A] at full precision
// since BUG-554 (display_hardening.cpp), and (E) checks it within T2.
//
// NOT REPRODUCED HERE, by design and named in the task report:
//   * the logit arms' VALUES. Adjudicated by w2e-probes-oracle-land-adjudicate
//     (tests/eval/golden/aer_w2/w2e_probes.log section A; TOLERANCE.md NOT relaxed):
//     the golden 8.375366030592314 stands (lp_default reproduces it bit for bit), and
//     Stata's own tightened fit (lp_tight, 8.375367164027917) is NOT the all-60-row
//     MLE 8.375368201 an earlier round pinned here as truth. Stata leaves out of the
//     likelihood the observations it reports as "completely determined"; the engine's
//     logit does not, so its _b[x1] (8.375368071) is 2.4e-7 from the oracle, outside
//     T3. That is an engine logit defect with its own owner, not this task's; the two
//     arms below check structure and hold the value only as a drift sentinel against
//     the ORACLE (5e-7 / 5e-6), which is NOT a parity claim.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "selftest_w2.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& label) {
  if (cond) ++g_pass;
  else ++g_fail;
  w2check(cond, label);
}

// rc and the last display text of one line, read the way the do-file reads it.
std::pair<std::string, std::string> shown(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0", text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") rc = piet::eval::coarseRc(b);
    if (type == "display") text = b.value("text", std::string());
  }
  return {rc, text};
}

std::string trimmed(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return std::string();
  return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

double numOf(const std::string& s) {
  const std::string t = trimmed(s);
  if (t.empty()) return std::nan("");
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  return (end != nullptr && *end == '\0') ? v : std::nan("");
}

std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object() || b.value("type", std::string()) != "error") continue;
    rc = piet::eval::coarseRc(b);
  }
  return rc;
}

double rv(piet::SessionState& s, const std::string& name) {
  const double* v = s.eval.rValues.find(name);
  return v ? *v : std::nan("");
}

std::string num(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.16g", v);
  return buf;
}

// TOLERANCE.md: T2 (1e-9) for derived post-estimation quantities; T1 (1e-12) for
// a lincom point value; p-values carry atol 1e-8 only (§3.1).
bool rel(double got, double want, double rtol) {
  return std::isfinite(got) && std::fabs(got - want) <= rtol * std::fabs(want);
}
bool pAtol(double got, double want) { return std::isfinite(got) && std::fabs(got - want) <= 1e-8; }

const piet::eval::EvalMatrix* rTable(piet::SessionState& s) {
  return s.eval.findClassMatrix('r', "table");
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? " " : "") + v[i];
  return out;
}

float f32(double v) { return static_cast<float>(v); }
double smod(double a, double b) { return a - b * std::floor(a / b); }

// The .do's data, stored the way StataBE stored it (see the header).
void writeFixture(const std::string& path) {
  std::ofstream out(path);
  out << "id,t,x1,x2,z1,c,g,y,d\n";
  char buf[64];
  auto w = [&](double v, bool last) {
    std::snprintf(buf, sizeof buf, "%.17g", v);
    out << buf << (last ? "\n" : ",");
  };
  for (int n = 1; n <= 60; ++n) {
    const double id = f32(std::ceil(n / 6.0));
    const double t = f32(smod(n - 1, 6) + 1);
    const double x1 = f32(smod(n * 7, 11) + smod(n, 5) / 10.0);
    const double x2 = f32(smod(n * 3, 5) + smod(n * 13, 7) / 10.0);
    const double z1 = f32(smod(n * 5, 13) + x1 / 2.0);
    const double c = smod(n, 4);
    const double g = smod(n, 5);
    const double y = f32(1 + 2 * x1 - x2 + 0.7 * c + 0.4 * g + id / 3.0 + smod(n * 11, 17) / 5.0);
    const double d = (y > 12) ? 1 : 0;
    w(id, false); w(t, false); w(x1, false); w(x2, false); w(z1, false);
    w(c, false); w(g, false); w(y, false); w(d, true);
  }
}

void loadFixture(piet::SessionState& s, const std::string& csv) {
  (void)piet::dispatch("clear all", s);
  (void)piet::dispatch("import delimited using \"" + csv + "\", clear", s);
  (void)piet::dispatch("xtset id t", s);
}

// One `test`-family arm: r(F) at T2, r(p) at atol, r(df)/r(df_r) exact.
void fArm(piet::SessionState& s, const std::string& arm, const std::string& cmd, double F, double p,
          double df, double dfr) {
  const std::string rc = rcOf(s, cmd);
  const double gF = rv(s, "F"), gp = rv(s, "p"), gdf = rv(s, "df"), gdfr = rv(s, "df_r");
  check(rc == "0" && rel(gF, F, 1e-9) && pAtol(gp, p) && gdf == df && gdfr == dfr,
        "W2C(postest): " + arm + " -- `" + cmd + "` rc 0, r(F) " + num(F) + " r(p) " + num(p) +
            " r(df) " + num(df) + " r(df_r) " + num(dfr) + " (w2c_postest.log PREC|" + arm +
            "|). Got rc " + rc + ", F " + num(gF) + ", p " + num(gp) + ", df " + num(gdf) +
            ", df_r " + num(gdfr));
}

// A fit's _b[x1]/_se[x1], read off its estimation block's coefficient table.
struct Reply1 {
  std::string rc = "0";
  double b = std::nan(""), se = std::nan("");
};
Reply1 fitOf(piet::SessionState& session, const std::string& cmd) {
  Reply1 r;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") r.rc = piet::eval::coarseRc(b);
    if (type != "estimation" || !b.contains("coeftable") || !b["coeftable"].is_array()) continue;
    for (const auto& row : b["coeftable"]) {
      if (!row.is_object() || row.value("name", std::string()) != "x1") continue;
      if (row.contains("b") && row["b"].is_number()) r.b = row["b"].get<double>();
      if (row.contains("se") && row["se"].is_number()) r.se = row["se"].get<double>();
    }
  }
  return r;
}

}  // namespace

void testW2cPostest() {
  std::printf("[selftest] --- W2c postest: r(table), test/testparm/lincom/nlcom on factor "
              "levels (w2c_postest.log families 1-5) ---\n");
  g_pass = 0;
  g_fail = 0;
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2c").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string csv = workdir + "/w2c_fixture.csv";
  writeFixture(csv);

  piet::SessionState s(workdir);
  loadFixture(s, csv);

  // ---- (1) r(table) after estimation -------------------------------------
  // Old behaviour for every arm in this family: no estimator posted r(table) at
  // all (only sktest did), so each read below found nothing.
  {
    const std::string rc = rcOf(s, "regress y x1 x2 i.c");
    const auto* T = rTable(s);
    check(rc == "0" && T != nullptr && T->data.size() == 9 && T->data[0].size() == 7,
          "W2C(postest): rtable_regress_dims -- r(table) after `regress y x1 x2 i.c` is 9 x 7 "
          "(PREC|rtable_regress_dims|0|9|7|)");
    check(T != nullptr && join(T->rownames) == "b se t pvalue ll ul df crit eform",
          "W2C(postest): rtable_regress_rownames -- `b se t pvalue ll ul df crit eform`. Got `" +
              (T ? join(T->rownames) : std::string("<none>")) + "`");
    check(T != nullptr && join(T->colnames) == "x1 x2 0b.c 1.c 2.c 3.c _cons",
          "W2C(postest): rtable_regress_colnames -- `x1 x2 0b.c 1.c 2.c 3.c _cons`: the base "
          "level is a column, in e(b) order. Got `" +
              (T ? join(T->colnames) : std::string("<none>")) + "`");
    const bool vals = T != nullptr && T->data.size() == 9 && T->data[0].size() == 7 &&
                      rel(T->data[0][0], 1.968531863579259, 1e-9) &&
                      rel(T->data[1][0], .0625802909948136, 1e-9) &&
                      pAtol(T->data[3][0], 2.09092890124e-36) && T->data[0][2] == 0.0 &&
                      std::isnan(T->data[1][2]) && T->data[6][0] == 54.0;
    check(vals, "W2C(postest): rtable_regress_vals -- b 1.968531863579259, se "
                ".0625802909948136, p 2.09e-36 (x1); the base column is b 0 / se missing; df 54" +
                    (T && !T->data.empty() ? " (got b " + num(T->data[0][0]) + ", se " +
                                                 num(T->data[1][0]) + ")"
                                           : std::string()));
    // `matrix NAME = r(table)` is how the corpus reads it (paper 077).
    check(rcOf(s, "matrix rt_names = r(table)") == "0" && s.eval.findMatrix("rt_names") != nullptr &&
              join(s.eval.findMatrix("rt_names")->colnames) == "x1 x2 0b.c 1.c 2.c 3.c _cons",
          "W2C(postest): rtable_regress_rownames/colnames -- `matrix rt_names = r(table)` copies "
          "the posted table with its stripes");
  }
  {
    const std::string rc = rcOf(s, "regress y x1 x2 i.c, level(90)");
    const auto* T = rTable(s);
    check(rc == "0" && T != nullptr && rel(T->data[0][0], 1.968531863579259, 1e-9) &&
              rel(T->data[4][0], 1.863799684741032, 1e-9) &&
              rel(T->data[5][0], 2.073264042417485, 1e-9),
          "W2C(postest): rtable_regress_level90 -- b unchanged, ll 1.863799684741032, ul "
          "2.073264042417485 (PREC|rtable_regress_level90|)" +
              (T ? " got ll " + num(T->data[4][0]) + " ul " + num(T->data[5][0]) : std::string()));
  }
  {
    const std::string rc = rcOf(s, "regress y x1 x2 i.c, vce(cluster id)");
    const auto* T = rTable(s);
    check(rc == "0" && T != nullptr && T->data.size() == 9 &&
              rel(T->data[0][0], 1.968531863579259, 1e-9) &&
              rel(T->data[1][0], .0777737515156292, 1e-9),
          "W2C(postest): rtable_regress_cluster -- 9 rows, b 1.968531863579259, cluster se "
          ".0777737515156292 (PREC|rtable_regress_cluster|)" +
              (T ? " got se " + num(T->data[1][0]) : std::string()));
  }
  struct FeArm { const char* arm; const char* cmd; double b11; };
  const FeArm feArms[] = {
      {"rtable_areg", "areg y x1 x2 i.c, absorb(id)", 1.989433760870929},
      {"rtable_reghdfe", "reghdfe y x1 x2 i.c, absorb(id)", 1.989433760870928},
      {"rtable_xtreg_fe", "xtreg y x1 x2 i.c, fe", 1.989433760870929},
      {"rtable_ivregress", "ivregress 2sls y x2 i.c (x1 = z1)", 1.930129137534934},
  };
  for (const auto& a : feArms) {
    const std::string rc = rcOf(s, a.cmd);
    const auto* T = rTable(s);
    check(rc == "0" && T != nullptr && T->data.size() == 9 && T->data[0].size() == 7 &&
              rel(T->data[0][0], a.b11, 1e-9),
          std::string("W2C(postest): ") + a.arm + " -- `" + a.cmd + "` posts a 9 x 7 r(table), "
          "[1,1] " + num(a.b11) + " (PREC|" + a.arm + "|)" +
              (T && !T->data.empty() ? " got [1,1] " + num(T->data[0][0]) : std::string()));
  }
  {
    // ivregress (no `small`) is normal-referenced: `z` row, df missing.
    const auto* T = rTable(s);
    check(T != nullptr && T->rownames.size() == 9 && T->rownames[2] == "z" &&
              std::isnan(T->data[6][0]) && rel(T->data[7][0], 1.959963984540054, 1e-12),
          "W2C(postest): rtable_ivregress -- normal reference: row 3 is `z`, df missing, crit "
          "invnormal(.975)");
  }
  {
    const std::string rc = rcOf(s, "logit d x1 x2 i.c");
    const auto* T = rTable(s);
    const auto& ct = s.lastEstBlock.has_value() ? (*s.lastEstBlock)["coeftable"]
                                                : nlohmann::ordered_json::array();
    double bx1 = std::nan("");
    for (const auto& row : ct)
      if (row.value("name", std::string()) == "x1") bx1 = row.value("b", std::nan(""));
    const double t11 = (T && !T->data.empty() && !T->data[0].empty()) ? T->data[0][0] : std::nan("");
    check(rc == "0" && T != nullptr && T->data.size() == 9 && T->data[0].size() == 7 &&
              t11 == bx1 && T->rownames[2] == "z" && std::isnan(T->data[6][0]) &&
              rel(t11, 8.375366030592314, 5e-7),
          "W2C(postest): rtable_logit -- 9 x 7, [1,1] IS the fit's own _b[x1], z row, df missing; "
          "[1,1] within the 5e-7 drift sentinel of the oracle 8.375366030592314 (outside T3 by "
          "adjudication, see header; PREC|rtable_logit|0|9|7|). Got " +
              num(t11));
  }
  {
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const bool posted = rTable(s) != nullptr;
    (void)piet::dispatch("quietly summarize x1", s);
    // The read is the oracle's own `r(table)[1,1]` form (198). `matrix Z =
    // r(table)` is NOT a read that fails: an absent r(NAME) in a matrix
    // definition is the 1x1 missing matrix at rc 0 (w2n_followups.log:938 E05).
    check(posted && rTable(s) == nullptr && rcOf(s, "local zz1 = r(table)[1,1]") != "0",
          "W2C(postest): rtable_survives_summarize -- `summarize` (r-class) replaces r(), so "
          "r(table) is gone (oracle rc 198 on the read)");
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const bool posted2 = rTable(s) != nullptr;
    (void)piet::dispatch("quietly test 2.c", s);
    check(posted2 && rTable(s) == nullptr && rcOf(s, "local zz2 = r(table)[1,1]") != "0",
          "W2C(postest): rtable_replaced_by_test -- `test` is r-class and clears r(table) "
          "(oracle rc 109 on the read)");
    // Non-r-class commands leave it alone (the generic stamp must not over-clear).
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("matrix keepme = r(table)", s);
    (void)piet::dispatch("local foo = 1", s);
    check(rTable(s) != nullptr,
          "W2C(postest): r(table) survives non-r-class commands (`matrix`, `local`)");
  }

  // ---- r(table) belongs to the e() that is live (review findings, attempt 4) --
  // Old: the table was re-posted only when the returned block EQUALLED the stored
  // e() block, so an estimation whose block a dispatcher arm transformed afterwards
  // (`logistic`, `logit, or`) re-served the PREVIOUS model's table at rc 0.
  auto colsOf = [](const piet::eval::EvalMatrix* T) {
    return T ? join(T->colnames) : std::string("<none>");
  };
  {
    (void)piet::dispatch("quietly regress y x1 z1 x2 g", s);
    (void)piet::dispatch("quietly logistic d x1 x2", s);
    const auto* T = rTable(s);
    double bx1 = std::nan("");
    if (s.lastEstBlock.has_value())
      for (const auto& row : (*s.lastEstBlock)["coeftable"])
        if (row.value("name", std::string()) == "x1") bx1 = row.value("b", std::nan(""));
    check(T != nullptr && colsOf(T) == "x1 x2 _cons" && T->rownames[2] == "z" &&
              rel(T->data[0][0], std::exp(bx1), 1e-12) && T->data[8][0] == 1.0,
          "W2C(postest): r(table) after `logistic` (following a regress) is the logistic table: "
          "columns `x1 x2 _cons`, b = exp(_b[x1]), eform 1. Got columns `" + colsOf(T) + "`");
  }
  {
    (void)piet::dispatch("quietly regress y x1 x2", s);
    (void)piet::dispatch("quietly logit d x1 x2 i.c, or", s);
    const auto* T = rTable(s);
    check(T != nullptr && colsOf(T) == "x1 x2 0b.c 1.c 2.c 3.c _cons" && T->data[0][2] == 1.0 &&
              std::isnan(T->data[1][2]) && T->data[8][2] == 1.0 &&
              rel(T->data[4][0], T->data[0][0] * std::exp(-1.959963984540054 * T->data[1][0] /
                                                           T->data[0][0]), 1e-9),
          "W2C(postest): `logit ..., or` posts an eform table: base column b 1, eform 1, ll = "
          "exp(b - crit*se) on the coefficient metric. Got columns `" + colsOf(T) + "`");
  }
  {
    // Old: `margins` left the estimation's table in place. Stata's margins posts its
    // own 9 x 1 r(table) named by the dydx() variable (w2o_rnames.log:436 N26 reads
    // rowsof(r(table)) at rc 0; help margins); after a linear fit its b
    // and se are the coefficient's.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const auto* T0 = rTable(s);
    const double bx1 = T0 ? T0->data[0][0] : std::nan("");
    const double sx1 = T0 ? T0->data[1][0] : std::nan("");
    const std::string rc = rcOf(s, "quietly margins, dydx(x1)");
    const auto* T = rTable(s);
    check(T0 != nullptr && rc == "0" && T != nullptr && colsOf(T) == "x1" &&
              T->rownames.size() == 9 && rel(T->data[0][0], bx1, 1e-12) &&
              rel(T->data[1][0], sx1, 1e-9),
          "W2C(postest): `margins, dydx(x1)` replaces the estimation's r(table) with its own "
          "9 x 1 table (column `x1`, b = _b[x1], se = _se[x1] after regress). margins rc " + rc +
              ", columns `" + colsOf(T) + "`");
  }
  {
    // Old: after `bootstrap ...: regress` r(table) was the LAST REPLICATE's regress
    // table (the inner command runs through dispatch() once per replicate); then
    // none was served. Lane BS re-pin (uni-bs): Stata's bootstrap posts its OWN
    // normal-based table, one column per exp_list name, b = the observed value,
    // rows b se z pvalue ll ul df crit eform with df missing and crit
    // invnormal(.975) (StataBE scratch-r150/lee/bs/o1.log T3|TROWS, T3|T).
    (void)piet::dispatch("quietly regress y x1 x2", s);
    const auto* T0 = rTable(s);
    const double bx1 = T0 ? T0->data[0][0] : std::nan("");
    (void)piet::dispatch("quietly bootstrap b1=_b[x1], reps(3) seed(1): regress y x1 x2", s);
    const auto* T = rTable(s);
    check(T != nullptr && colsOf(T) == "b1" && T->rownames.size() == 9 && T->rownames[2] == "z" &&
              rel(T->data[0][0], bx1, 1e-15) && std::isnan(T->data[6][0]) &&
              T->data[7][0] == 1.959963984540054,
          "W2C(postest): after `bootstrap b1=_b[x1], ...: regress` r(table) is bootstrap's own "
          "(column b1, b = _b[x1] of the full-sample fit, z row, df missing; o1.log T3). Got "
          "columns `" + colsOf(T) + "`");
  }
  {
    // e() replaced by a copy of the SAME model (`estadd` re-assigns e() with a scalar
    // added) keeps the table.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const std::string rc = rcOf(s, "estadd scalar w2c_extra = 1");
    check(rc == "0" && colsOf(rTable(s)) == "x1 x2 0b.c 1.c 2.c 3.c _cons",
          "W2C(postest): `estadd scalar` keeps the estimation's r(table). estadd rc " + rc +
              ", columns `" + colsOf(rTable(s)) + "`");
  }
  {
    // Old: poisson's table was labelled `z` but carried t(dof_resid) p-values and
    // intervals, and a level inferred from them (crit 2.0047). Stata: normal.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly poisson y x1 x2 i.c", s);
    const auto* T = rTable(s);
    bool ok = T != nullptr && colsOf(T) == "x1 x2 0b.c 1.c 2.c 3.c _cons" && T->rownames[2] == "z";
    for (size_t j = 0; ok && j < T->colnames.size(); ++j) {
      if (j == 2) continue;
      const double b = T->data[0][j], se = T->data[1][j], z = T->data[2][j];
      ok = rel(z, b / se, 1e-12) && std::fabs(T->data[3][j] - std::erfc(std::fabs(z) / std::sqrt(2.0))) <= 1e-12 &&
           rel(T->data[4][j], b - 1.959963984540054 * se, 1e-12) &&
           rel(T->data[5][j], b + 1.959963984540054 * se, 1e-12) && std::isnan(T->data[6][j]) &&
           T->data[7][j] == 1.959963984540054;
    }
    check(ok, "W2C(postest): r(table) after `poisson` is normal-referenced throughout: p = "
              "2*normal(-|z|), ll/ul = b -/+ invnormal(.975)*se, crit 1.959964, df missing");
  }
  {
    // A block whose intervals disagree with its declared reference posts nothing
    // (tobit prints t intervals and publishes no df_r) -- and the previous model's
    // table is not left behind.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly tobit y x1 x2, ll(5)", s);
    check(rTable(s) == nullptr,
          "W2C(postest): after `tobit` no r(table) is served (not the preceding regress table). "
          "Got columns `" + colsOf(rTable(s)) + "`");
  }
  {
    // The level is declared, not inferred: under `set level 90` a posted table must be
    // the 90% one (regress's engine interval ignores `set level`, so nothing is posted).
    (void)piet::dispatch("set level 90", s);
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const auto* T = rTable(s);
    const bool ok = T == nullptr ||
                    (T->data[7][0] < 1.7 && rel(T->data[4][0], T->data[0][0] - T->data[7][0] * T->data[1][0], 1e-9));
    (void)piet::dispatch("set level 95", s);
    check(ok, "W2C(postest): under `set level 90` r(table) is the 90% table or absent, never a "
              "95% table labelled otherwise");
  }

  // ---- (2) test on factor levels ------------------------------------------
  (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
  fArm(s, "test_2c", "test 2.c", 4.780217910700236, .0331418104435503, 1, 54);
  // Old: rc 1 "variable 'c' not among regressors" (`1.` read as the number 1).
  fArm(s, "test_1c_eq_2c", "test 1.c = 2.c", 1.359896184620174, .2486801553563908, 1, 54);
  // Old: parens stripped, `0.c` unknown. `0.c` is the OMITTED base: coefficient 0.
  fArm(s, "test_2c_eq_avg_base", "test 2.c = 0.5*(1.c+0.c)", 3.744827017651374,
       .0582156163549912, 1, 54);
  fArm(s, "test_2c_3c", "test 2.c 3.c", 6.302427399551597, .0034671497280403, 2, 54);
  fArm(s, "test_paren_pair", "test (2.c=0) (3.c=0)", 6.302427399551597, .0034671497280403, 2, 54);
  {
    const std::string rc = rcOf(s, "test 3.c");
    check(rc == "0" && rel(rv(s, "F"), 12.36422091149351, 1e-9) && rv(s, "df") == 1,
          "W2C(postest): test_3c_ctl -- F 12.36422091149351, df 1. Got " + num(rv(s, "F")));
  }
  // Old: `accumulate` refused as unimplemented, rc 198.
  fArm(s, "test_2c_accumulate", "test 2.c, accumulate", 6.302427399551597, .0034671497280403, 2,
       54);
  {
    const std::string rc = rcOf(s, "test 9.c");
    check(rc == "111", "W2C(postest): test_level_not_in_model -- `test 9.c` is `9.c not found`, "
                       "r(111) (old rc 1). Got rc " + rc);
  }
  {
    // The accumulate set is bound to the estimation: a new fit starts empty.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly test 3.c", s);
    (void)piet::dispatch("quietly regress y x1 x2 i.c i.g", s);
    const std::string rc = rcOf(s, "test 2.c, accumulate");
    check(rc == "0" && rv(s, "df") == 1,
          "W2C(postest): accumulate does not carry restrictions across estimations (df 1). Got df " +
              num(rv(s, "df")));
  }
  (void)piet::dispatch("quietly reghdfe y x1 x2 i.c, absorb(id)", s);
  fArm(s, "test_2c_after_reghdfe", "test 2.c", 8.08226560237993, .0066983431319311, 1, 45);
  {
    (void)piet::dispatch("quietly logit d x1 x2 i.c", s);
    const std::string rc = rcOf(s, "test 2.c");
    // The value inherits the logit gap (header): oracle drift sentinels only.
    check(rc == "0" && rel(rv(s, "chi2"), .0050963123383602, 5e-6) &&
              std::fabs(rv(s, "p") - .9430885931095653) <= 1e-6 &&
              rv(s, "df") == 1 && s.eval.rValues.find("df_r") == nullptr &&
              s.eval.rValues.find("F") == nullptr,
          "W2C(postest): test_2c_after_logit -- chi2 form: r(df) 1, no r(F)/r(df_r); r(chi2)/r(p) "
          "within the drift sentinels of the oracle .0050963123383602 / .9430885931095653 (outside "
          "T3 by adjudication, see header). Got chi2 " +
              num(rv(s, "chi2")) + ", p " + num(rv(s, "p")));
  }

  // ---- (3) testparm -------------------------------------------------------
  (void)piet::dispatch("quietly regress y x1 x2 i.c i.g", s);
  fArm(s, "testparm_ic", "testparm i.c", 4.919188193260892, .0045152870686122, 3, 50);
  // Old: rc 1 -- `i(2/3).c` matched no coefficient.
  fArm(s, "testparm_i23_c", "testparm i(2/3).c", 6.753225367030235, .0025339851893971, 2, 50);
  fArm(s, "testparm_i24_g", "testparm i(2/4).g", .9495440012222118, .4238640083229918, 3, 50);
  fArm(s, "testparm_2c_3c", "testparm 2.c 3.c", 6.753225367030235, .0025339851893971, 2, 50);
  fArm(s, "testparm_star_ctl", "testparm x1 x2", 525.1026877269014, 2.73782727243e-34, 2, 50);
  // Old: df 3 -- the glob also swept in the LEVELS 1.c and 1.g.
  fArm(s, "testparm_wildcard_substr", "testparm *1*", 1046.479377217365, 3.42183347416e-35, 1, 50);

  // ---- (4) lincom ---------------------------------------------------------
  (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
  {
    const std::string rc = rcOf(s, "lincom 2.c - 3.c");
    check(rc == "0" && rel(rv(s, "estimate"), -.7348207790066308, 1e-12) &&
              rel(rv(s, "se"), .554456366806204, 1e-9),
          "W2C(postest): lincom_2c_minus_3c -- estimate -.7348207790066308, se .554456366806204. "
          "Got " + num(rv(s, "estimate")) + " / " + num(rv(s, "se")));
  }
  {
    // Old: rc 1 -- lincom's parser had no `_b[...]`.
    const std::string rc = rcOf(s, "lincom _b[2.c] + _b[x1]");
    check(rc == "0" && rel(rv(s, "estimate"), 3.183334592562747, 1e-12) &&
              rel(rv(s, "se"), .5544680813153977, 1e-9),
          "W2C(postest): lincom_bexpr -- estimate 3.183334592562747, se .5544680813153977. Got rc " +
              rc + ", " + num(rv(s, "estimate")) + " / " + num(rv(s, "se")));
  }

  // ---- (5) nlcom ----------------------------------------------------------
  auto rb11 = [&s]() {
    const auto* m = s.eval.findClassMatrix('r', "b");
    return (m && !m->data.empty() && !m->data[0].empty()) ? m->data[0][0] : std::nan("");
  };
  (void)piet::dispatch("quietly regress y x1 x2", s);
  {
    // Old: r(b) was never posted.
    const std::string rc = rcOf(s, "nlcom _b[x1]/_b[x2]");
    check(rc == "0" && rel(rb11(), -2.270183068802698, 1e-9),
          "W2C(postest): nlcom_ratio -- r(b)[1,1] -2.270183068802698. Got " + num(rb11()));
  }
  {
    const std::string rc = rcOf(s, "nlcom (Info: _b[_cons])");
    const auto* m = s.eval.findClassMatrix('r', "b");
    check(rc == "0" && rel(rb11(), 6.145575962727019, 1e-9) && m && m->colnames.size() == 1 &&
              m->colnames[0] == "Info",
          "W2C(postest): nlcom_label_only -- r(b)[1,1] 6.145575962727019, column `Info`. Got " +
              num(rb11()));
  }
  (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
  {
    // Old: rc 1 -- the three groups shipped as one expression.
    const std::string rc =
        rcOf(s, "nlcom (Info: _b[_cons]) (Click: _b[_cons]+_b[1.c]) (Viral: _b[_cons]+_b[2.c])");
    (void)piet::dispatch("matrix nlt = r(table)", s);
    const auto* T = s.eval.findMatrix("nlt");
    check(rc == "0" && T != nullptr && T->data.size() == 9 && T->data[0].size() == 3 &&
              rel(T->data[0][0], 5.26541054853662, 1e-9) &&
              rel(T->data[0][1], 5.833426885669602, 1e-9) && T->rownames[2] == "z" &&
              join(T->colnames) == "Info Click Viral",
          "W2C(postest): nlcom_multi_nopost_rtable -- r(table) 9 x 3 (z-referenced), [1,1] "
          "5.26541054853662, [1,2] 5.833426885669602. Got rc " + rc +
              (T && !T->data.empty() && T->data[0].size() > 1
                   ? ", " + num(T->data[0][0]) + " / " + num(T->data[0][1])
                   : std::string()));
  }
  {
    const std::string rc = rcOf(s, "nlcom (A: _b[x1]/_b[x2]) (B: _b[2.c]), post");
    (void)piet::dispatch("scalar w2c_ba = _b[A]", s);
    (void)piet::dispatch("matrix nlcom_b = e(b)", s);
    const auto* eb = s.eval.findMatrix("nlcom_b");
    const double ba = std::atof(s.eval.scalars.get("w2c_ba", "nan").c_str());
    check(rc == "0" && rel(ba, -2.262364835627125, 1e-9) && eb != nullptr &&
              join(eb->colnames) == "A B",
          "W2C(postest): nlcom_multi_post -- _b[A] -2.262364835627125, e(b) colnames `A B`. Got rc " +
              rc + ", _b[A] " + num(ba) + ", colnames `" + (eb ? join(eb->colnames) : "") + "`");
    const std::string rc2 = rcOf(s, "estimates store nlcom_ab");
    check(rc2 == "0", "W2C(postest): nlcom_post_estimates_store -- rc 0. Got " + rc2);
  }

  // ---- (E) the oracle's own read surfaces (macro/arith layer) --------------
  // Layer: piet/native/src/eval/macros.cpp only (the bare-r() pass, the matrix
  // operand substitution, the extended macro functions); not the shadow CiParser,
  // not the engine. Old behaviour, each arm: the bare-r() pass refused every read
  // of a posted r-class matrix outside the `matrix` verb and el() ("r(table) is not
  // set", rc 1); `local x = r(table)[1,1]` stored its own text at rc 0; inline
  // `: rowfullnames` tripped "does not expand macros" (rc 198); `_b[2.c]` was
  // "_b not found" / "unknown variable _b".
  {
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const auto dims = shown(s, "display rowsof(r(table))");
    const auto cols = shown(s, "display colsof(r(table))");
    check(dims.first == "0" && trimmed(dims.second) == "9" && cols.first == "0" &&
              trimmed(cols.second) == "7",
          "W2C(postest/E): rtable_regress_dims -- `display rowsof(r(table))` 9, colsof 7 "
          "(PREC|rtable_regress_dims|0|9|7|). Got rc " + dims.first + "/" + cols.first + ", `" +
              dims.second + "`/`" + cols.second + "`");
    const auto b = shown(s, "display %18.0g r(table)[1,1]");
    const auto se = shown(s, "display %18.0g r(table)[2,1]");
    const auto p = shown(s, "display %18.0g r(table)[4,1]");
    check(b.first == "0" && rel(numOf(b.second), 1.968531863579259, 1e-9) &&
              rel(numOf(se.second), .0625802909948136, 1e-9) &&
              pAtol(numOf(p.second), 2.09092890124e-36),
          "W2C(postest/E): rtable_regress_vals -- `display %18.0g r(table)[1,1]`/[2,1]/[4,1] "
          "(PREC|rtable_regress_vals|). Got rc " + b.first + ", `" + b.second + "` `" + se.second +
              "` `" + p.second + "`");
    (void)piet::dispatch("local w2c_x = r(table)[1,1]", s);
    const std::string lx = s.eval.locals.get("w2c_x", "<unset>");
    check(rel(numOf(lx), 1.968531863579259, 1e-9),
          "W2C(postest/E): `local x = r(table)[1,1]` stores the cell (was: its own text at rc 0). "
          "Got `" + lx + "`");
    (void)piet::dispatch("matrix rt_names = r(table)", s);
    const auto rn = shown(s, "display \"`: rowfullnames rt_names'\"");
    const auto cn = shown(s, "display \"`: colfullnames rt_names'\"");
    check(rn.first == "0" && rn.second == "b se t pvalue ll ul df crit eform" && cn.first == "0" &&
              cn.second == "x1 x2 0b.c 1.c 2.c 3.c _cons",
          "W2C(postest/E): rtable_regress_rownames/colnames -- inline `: rowfullnames`/`: "
          "colfullnames` of the copy (PREC|...|b se t pvalue ll ul df crit eform| and "
          "|x1 x2 0b.c 1.c 2.c 3.c _cons|). Got rc " + rn.first + "/" + cn.first + ", `" +
              rn.second + "` / `" + cn.second + "`");
    (void)piet::dispatch("local w2c_cf : colfullnames r(table)", s);
    check(s.eval.locals.get("w2c_cf", "<unset>") == "x1 x2 0b.c 1.c 2.c 3.c _cons",
          "W2C(postest/E): `local c : colfullnames r(table)` reads the posted matrix directly. Got `" +
              s.eval.locals.get("w2c_cf", "<unset>") + "`");
    const auto bare = shown(s, "display r(table)");
    check(bare.first == "109",
          "W2C(postest/E): a bare `display r(table)` (matrix in scalar context) is still refused, "
          "now at r(109) type mismatch (w2f_residuals.log:1625 PREC|e_rtable_display|0|109|; was "
          "rc 1). Got rc " +
              bare.first);
  }
  {
    (void)piet::dispatch("quietly regress y x1 x2 i.c, level(90)", s);
    const auto ll = shown(s, "display %18.0g r(table)[5,1]");
    const auto ul = shown(s, "display %18.0g r(table)[6,1]");
    check(ll.first == "0" && rel(numOf(ll.second), 1.863799684741032, 1e-9) &&
              rel(numOf(ul.second), 2.073264042417485, 1e-9),
          "W2C(postest/E): rtable_regress_level90 -- r(table)[5,1]/[6,1] read by display. Got `" +
              ll.second + "` `" + ul.second + "`");
  }
  {
    // w2c_postest.log:363-370 / 377-384: the literal negative arms.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rc1 = rcOf(s, "local rtval1 : display %18.0g r(table)[1,1]");
    const std::string rc1b = rcOf(s, "local w2c_y = r(table)[1,1]");
    check(rc1 == "198" && rc1b == "198" && s.eval.locals.get("w2c_y", "<unset>") == "<unset>",
          "W2C(postest/E): rtable_survives_summarize -- `local v : display %18.0g "
          "r(table)[1,1]` after summarize is r(198) (PREC|rtable_survives_summarize|198||); "
          "`local y = r(table)[1,1]` too, and stores nothing. Got rc " + rc1 + " / " + rc1b);
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly test 2.c", s);
    const std::string rc2 = rcOf(s, "local rtcols : display colsof(r(table))");
    const std::string rc2b = rcOf(s, "display rowsof(r(table))");
    check(rc2 == "109" && rc2b == "109",
          "W2C(postest/E): rtable_replaced_by_test -- `local c : display colsof(r(table))` after "
          "test is r(109) (PREC|rtable_replaced_by_test|109||). Got rc " + rc2 + " / " + rc2b);
  }
  {
    (void)piet::dispatch("quietly regress y x1 x2", s);
    (void)piet::dispatch("quietly nlcom _b[x1]/_b[x2]", s);
    const auto r1 = shown(s, "display %18.0g r(b)[1,1]");
    (void)piet::dispatch("quietly nlcom (Info: _b[_cons])", s);
    const auto r2 = shown(s, "display %18.0g r(b)[1,1]");
    check(r1.first == "0" && rel(numOf(r1.second), -2.270183068802698, 1e-9) && r2.first == "0" &&
              rel(numOf(r2.second), 6.145575962727019, 1e-9),
          "W2C(postest/E): nlcom_ratio / nlcom_label_only -- `display %18.0g r(b)[1,1]` "
          "(PREC|nlcom_ratio|0|-2.270183068802698|, |nlcom_label_only|0| 6.145575962727019|). "
          "Got `" + r1.second + "` / `" + r2.second + "`");
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly nlcom (A: _b[x1]/_b[x2]) (B: _b[2.c]), post", s);
    (void)piet::dispatch("matrix nlcom_b2 = e(b)", s);
    const auto nb = shown(s, "display \"`: colfullnames nlcom_b2'\"");
    const auto ba = shown(s, "display %18.0g _b[A]");
    check(nb.first == "0" && nb.second == "A B" && ba.first == "0" &&
              rel(numOf(ba.second), -2.262364835627125, 1e-9),
          "W2C(postest/E): nlcom_multi_post -- `: colfullnames` of the posted e(b) is `A B` "
          "(PREC|nlcom_multi_post|...|A B|); `display %18.0g _b[A]` within T2 of the oracle's "
          "-2.262364835627125 (BUG-554: was 9 digits, -2.26236484, rel 1.9e-9). Got `" +
              nb.second + "`, `" + ba.second + "`");
  }
  {
    // Factor-level coefficient names on the macro and row surfaces.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    double b2 = std::nan(""), se2 = std::nan("");
    if (s.lastEstBlock.has_value())
      for (const auto& row : (*s.lastEstBlock)["coeftable"])
        if (row.value("name", std::string()) == "2.c") {
          b2 = row.value("b", std::nan(""));
          se2 = row.value("se", std::nan(""));
        }
    (void)piet::dispatch("local w2c_q = _b[2.c]", s);
    (void)piet::dispatch("local w2c_s = _se[2.c]", s);
    const std::string rcg = rcOf(s, "generate double w2c_g = _b[2.c] + 0*x1");
    (void)piet::dispatch("quietly summarize w2c_g", s);
    // `local v = exp` stores Stata's strtrim(string(x, "%18.0g")) text (main's macro
    // layer, stataLocalRealText; w2f_residuals.log lt_01..lt_35), so the locals carry
    // 15-16 significant decimals of b and se, not the double: compared at 1e-15.
    check(std::isfinite(b2) && rel(numOf(s.eval.locals.get("w2c_q", "")), b2, 1e-15) &&
              rel(numOf(s.eval.locals.get("w2c_s", "")), se2, 1e-15) && rcg == "0" &&
              rv(s, "min") == b2 && rv(s, "max") == b2,
          "W2C(postest/E): `_b[2.c]` / `_se[2.c]` resolve in `local =` and `generate` (was: `_b "
          "not found` r(111) / `unknown variable _b`). Want b " + num(b2) + " se " + num(se2) +
              "; got `" + s.eval.locals.get("w2c_q", "<unset>") + "` / `" +
              s.eval.locals.get("w2c_s", "<unset>") + "`, generate rc " + rcg + ", min " +
              num(rv(s, "min")) + " max " + num(rv(s, "max")));
    // display_hardening.cpp's reBRef/reSeRef twin (display half of BUG-629) and its
    // full-precision substitution (BUG-554): `display _b[2.c]` was "matrix _b not
    // found" at rc 1, and a wide format printed nine digits.
    const auto db = shown(s, "display %18.0g _b[2.c]");
    const auto ds = shown(s, "display %18.0g _se[2.c]");
    check(db.first == "0" && ds.first == "0" && rel(numOf(db.second), b2, 1e-14) &&
              rel(numOf(ds.second), se2, 1e-14),
          "W2C(postest/E): `display %18.0g _b[2.c]` / `_se[2.c]` resolve at full precision (was: "
          "rc 1 `matrix _b not found`; BUG-554 9 digits). Want " + num(b2) + " / " + num(se2) +
              "; got rc " + db.first + "/" + ds.first + ", `" + db.second + "` / `" + ds.second +
              "`");
    const std::string rcn = rcOf(s, "local w2c_n = _b[9.c]");
    check(rcn == "111", "W2C(postest/E): `_b[9.c]` (a level not in the model) is r(111). Got rc " +
                            rcn);
    // The display twin of the same read carries the same code (review finding, defect
    // class 2): `display _b[9.c]` was rc 1 while `display _b[9.c]+0` was r(111). StataNow
    // 19.5 BE capture of 2026-09-29 (not yet a landed golden): all three rc 111, and the
    // text brackets the name, `[9.c] not found` (was `9.c not found`).
    // The base level is NOT pinned to 111: Stata answers `_b[0b.c]` with 0 at rc 0
    // (w2f_residuals.log:1650 PREC|e_b_0bc|, :1665), which belongs to the e(b)-order work
    // (base levels have no coeftable row here yet). Only the defect class is held: never
    // the anonymous rc 1.
    auto rcMsg = [&](const std::string& cmd) {
      std::string rc = "0", msg;
      for (const auto& b : piet::dispatch(cmd, s))
        if (b.is_object() && b.value("type", std::string()) == "error") {
          rc = piet::eval::coarseRc(b);
          msg = b.value("message", std::string());
        }
      return std::make_pair(rc, msg);
    };
    const auto d9 = rcMsg("display _b[9.c]");
    const auto s9 = rcMsg("display %18.0g _se[9.c]");
    const auto l9 = rcMsg("local w2c_n9 = _b[9.c]");
    const std::string di = rcOf(s, "display _b[1.c#c.x1]");
    const std::string d0 = rcOf(s, "display _b[0b.c]");
    const std::string l0 = rcOf(s, "local w2c_0 = _b[0b.c]");
    check(d9.first == "111" && s9.first == "111" && di == "111" && d0 != "1" && l0 != "1" &&
              d9.second.find("[9.c] not found") != std::string::npos &&
              s9.second.find("[9.c] not found") != std::string::npos &&
              l9.second.find("[9.c] not found") != std::string::npos,
          "W2C(postest/E): `display _b[9.c]` / `%18.0g _se[9.c]` / `_b[1.c#c.x1]` are r(111) like "
          "the macro twin (was rc 1), the message is Stata's `[9.c] not found` on both layers, and "
          "the base level `_b[0b.c]` is never rc 1. Got rc " + d9.first + "/" + s9.first + "/" +
              di + "/" + d0 + ", local " + l0 + "; text `" + d9.second + "` / `" + s9.second +
              "` / `" + l9.second + "`");
    (void)piet::dispatch("drop w2c_g", s);
  }
  {
    // F8: after a model whose r(table) this build does not post, a read is a NAMED
    // refusal (r(111), the reason in the message), not Stata's no-table 198/109 --
    // and the note dies with r() like a table does. The model was `i.c#c.x1` beside
    // x1 until BUG-1476 gave the engine Stata's parameterization of it, whose table
    // is now posted; jackknife's resampling table is the refusal still standing.
    (void)piet::dispatch("quietly regress y x1 x2 i.c#c.x1", s);
    {
      const std::string rcTab = rcOf(s, "matrix w2c_FI = r(table)");
      const std::string rcEb = rcOf(s, "matrix w2c_EI = e(b)");
      const std::string same = rcOf(s, "assert colsof(w2c_FI) == colsof(w2c_EI)");
      check(rcTab == "0" && rcEb == "0" && same == "0",
            "W2C(postest/E): after `regress y x1 x2 i.c#c.x1` (Stata's parameterization since "
            "BUG-1476) r(table) is posted with e(b)'s columns. Got rc " + rcTab + " / " + rcEb +
                " / " + same);
    }
    (void)piet::dispatch("quietly jackknife w2c_j=_b[x1]: regress y x1 x2", s);
    const std::string rcm = rcOf(s, "matrix w2c_F = r(table)");
    auto disp = piet::dispatch("display r(table)[1,1]", s);
    std::string rcd = "0", msg;
    for (const auto& b : disp)
      if (b.is_object() && b.value("type", std::string()) == "error") {
        rcd = piet::eval::coarseRc(b);
        msg = b.value("message", std::string());
      }
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rcAfter = rcOf(s, "display r(table)[1,1]");
    check(rcm == "111" && rcd == "111" && msg.find("resampling") != std::string::npos &&
              rcAfter == "198",
          "W2C(postest/E): after jackknife r(table) is refused by name at r(111) (the resampling "
          "table), and after `summarize` the read is Stata's r(198). Got rc " + rcm + " / " +
              rcd + " / " + rcAfter + " `" + msg + "`");
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly margins, dydx(x1)", s);
    const std::string rcMg = rcOf(s, "display colsof(r(table))");
    const auto* TMg = rTable(s);
    check(rcMg == "0" && TMg != nullptr && TMg->colnames.size() == 1,
          "W2C(postest/E): after `margins, dydx(x1)` its own r(table) reads: colsof is 1 at rc 0 "
          "(w2o_rnames.log:436 N26 reads r(table) after margins at rc 0). Got rc " + rcMg);
  }

  {
    // `assert` is not r-class: it leaves r() -- r(N) and a posted r(table) -- as the
    // last r-class command left it. Oracle: w2f_residuals.log:1741
    // `PREC|e_assert_then_rn|0|32|1|`; the r(table) half follows from the same rule
    // (no oracle arm reads r(table) after regress+assert: :1752 reads it after
    // summarize, which had already retired it). Old behaviour: assert reset r(N) to
    // _N (60) and the changed r() retired r(table).
    (void)piet::dispatch("quietly summarize x1 if x1>5", s);
    const double n0 = rv(s, "N");
    const std::string rcA = rcOf(s, "assert x1>=0");
    const double n1 = rv(s, "N");
    const std::string rcF = rcOf(s, "assert x1>5");
    const double n2 = rv(s, "N");
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("assert x1>=0", s);
    const auto rt = shown(s, "display colsof(r(table))");
    check(std::isfinite(n0) && n0 < 60 && rcA == "0" && n1 == n0 && rcF == "9" && n2 == n0 &&
              rt.first == "0" && trimmed(rt.second) == "7",
          "W2C(postest/E): `assert` leaves r(N) (" + num(n0) + ") and r(table) alone, on success "
          "and on r(9). Got rc " + rcA + "/" + rcF + ", r(N) " + num(n1) + " / " + num(n2) +
              ", colsof(r(table)) rc " + rt.first + " `" + rt.second + "`");
  }

  {
    // r(table) retirement is an EVENT (an r-class command ran), not a change of r()
    // VALUES (review r7, BUG-629). PIET's regress leaves r() in place, so an r-class
    // command that writes the values r() already held looked like no command at all
    // and the stale table was read at rc 0. Oracle: w2c_postest.log:370
    // PREC|rtable_survives_summarize|198|| (any r-class command replaces r()), and
    // w2f_residuals.log:1741/:1752 for `assert` (not r-class) leaving r() alone.
    // Layer: eval/session.h's store generations, read by handlers_aer_estim.cpp.
    auto readCell = [&]() { return shown(s, "display %18.0g r(table)[1,1]"); };
    double bx1 = std::nan("");
    auto liveBx1 = [&]() {
      bx1 = std::nan("");
      if (s.lastEstBlock.has_value())
        for (const auto& row : (*s.lastEstBlock)["coeftable"])
          if (row.value("name", std::string()) == "x1") bx1 = row.value("b", std::nan(""));
    };
    // (a) summarize x1 / regress / summarize x1 (the same r() values) -> r(198).
    (void)piet::dispatch("quietly summarize x1", s);
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const bool postedA = rTable(s) != nullptr;
    (void)piet::dispatch("quietly summarize x1", s);
    const auto a = readCell();
    check(postedA && a.first == "198",
          "W2C(postest/E): `summarize x1` / `regress` / `summarize x1` (identical r() values) "
          "retires r(table): the read is r(198) (PREC|rtable_survives_summarize|198||; was the "
          "stale _b[x1] at rc 0). Got rc " + a.first + " `" + a.second + "`");
    // (b) count / regress / count (the same r(N)) -> r(198).
    (void)piet::dispatch("quietly count", s);
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    const bool postedB = rTable(s) != nullptr;
    (void)piet::dispatch("quietly count", s);
    const auto b = readCell();
    check(postedB && b.first == "198",
          "W2C(postest/E): `regress` / `count` (r(N) unchanged) retires r(table): the read is "
          "r(198) (was the stale _b[x1] at rc 0). Got rc " + b.first + " `" + b.second + "`");
    // (c) regress / assert <true> / read -> the value at rc 0 (no r-class command ran;
    // the scope guard puts back the saved r() and with it the stores' generations).
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    liveBx1();
    const std::string rcAs = rcOf(s, "assert x1 >= 0");
    const auto c = readCell();
    check(rcAs == "0" && c.first == "0" && rel(numOf(c.second), bx1, 1e-15),
          "W2C(postest/E): `regress` / `assert x1 >= 0` / `r(table)[1,1]` is _b[x1] at rc 0 "
          "(assert is not r-class; w2f_residuals.log:1741). Want " + num(bx1) + "; got rc " +
              rcAs + "/" + c.first + " `" + c.second + "`");
    // (d) regress / estadd scalar / read -> the same-model rebind still holds.
    const std::string rcEa = rcOf(s, "estadd scalar w2c_extra2 = 2");
    const auto d = readCell();
    check(rcEa == "0" && d.first == "0" && rel(numOf(d.second), bx1, 1e-15),
          "W2C(postest/E): `regress` / `estadd scalar` / `r(table)[1,1]` is still _b[x1] at rc 0 "
          "(e() re-assigned with the same model keeps its table). Got rc " + rcEa + "/" +
              d.first + " `" + d.second + "`");
    // (e) the refusal note (a model whose table this build does not post) also dies
    // with an r-class command that writes identical values: Stata's no-table r(198),
    // not the note's r(111).
    (void)piet::dispatch("quietly summarize x1", s);
    (void)piet::dispatch("quietly jackknife w2c_j=_b[x1]: regress y x1 x2", s);
    const std::string rcNote = rcOf(s, "display r(table)[1,1]");
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rcGone = rcOf(s, "display r(table)[1,1]");
    check(rcNote == "111" && rcGone == "198",
          "W2C(postest/E): after jackknife the read is the named r(111); after "
          "`summarize x1` writing the same r() values it is r(198) (was r(111): the note "
          "outlived the r-class command). Got rc " + rcNote + " / " + rcGone);
    // (f) an rclass program replaces r() when it ENDS (its `return` set is committed
    // after its last inner command), so the check must also run for the container
    // frame; an sclass program (s() is not r()) leaves the table. Review r8: `myr`
    // with `return scalar k = 5` kept the table at rc 0 (also on 65b6d113).
    try {
      (void)piet::doexec::executeDoSource("program define w2c_myr, rclass\n"
                                          "  return scalar k = 5\n"
                                          "end\n"
                                          "program define w2c_mys, sclass\n"
                                          "  sreturn local foo \"bar\"\n"
                                          "end\n",
                                          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    (void)piet::dispatch("quietly w2c_mys", s);
    const auto fs = readCell();
    (void)piet::dispatch("quietly w2c_myr", s);
    const auto fr = readCell();
    check(fs.first == "0" && rel(numOf(fs.second), bx1, 1e-15) && fr.first == "198" &&
              rv(s, "k") == 5.0,
          "W2C(postest/E): an sclass program leaves r(table) (rc 0); an rclass program's "
          "`return scalar k = 5` replaces r() and retires it (r(198); was the stale _b[x1] at "
          "rc 0). Got rc " + fs.first + " / " + fr.first + ", r(k) " + num(rv(s, "k")));
  }

  {
    // r() is REPLACED wholesale by every r-class command and by every e-class fit
    // (DIAGNOSIS.md postest-rtable F, design W2: dispatch()'s exit erases every r()
    // entry older than the command when the command wrote r()). Old behaviour (8dc9d335
    // and main 39c2a68d): r-class handlers MERGED into r(), `label language` cleared
    // rStrings (and with it the r(table) stamp and s()), a fit left r() in place, and an
    // absent r(NAME) read was a refusal at rc 1.
    auto cell = [&]() { return shown(s, "display %18.0g r(table)[1,1]"); };
    // (a) regress / `label language` / r(table)[1,1] -> r(198). Oracle:
    // w2n_followups.log:1027 (E10, rc_local 198) and the rule of w2c_postest.log:370.
    // Was `0|1.9730462161250437` on 8dc9d335 (rStrings.clear() deleted the stamp).
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    const bool postedA = rTable(s) != nullptr;
    (void)piet::dispatch("label language", s);
    const auto a = cell();
    const std::string rcLa = rcOf(s, "local w2c_la = r(table)[1,1]");
    const std::string la = s.eval.locals.get("w2c_la", "<unset>");
    check(postedA && a.first == "198" && rcLa == "198" && la == "<unset>",
          "W2C(postest/E10): `regress` / `label language` / `r(table)[1,1]` is r(198) "
          "(w2n_followups.log:1027; was the stale _b[x1] at rc 0). Got rc " + a.first + " `" +
              a.second + "`, local rc " + rcLa + " `" + la + "`");
    // (b) ... then `summarize x1` -> still r(198) (E11, w2n_followups.log:1043).
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("label language", s);
    (void)piet::dispatch("quietly summarize x1", s);
    const auto b = cell();
    check(b.first == "198",
          "W2C(postest/E11): `regress` / `label language` / `summarize x1` / `r(table)[1,1]` is "
          "r(198) (w2n_followups.log:1043; was rc 0: a table with no stamp was never retired). "
          "Got rc " + b.first + " `" + b.second + "`");
    // (c) control: `label language` itself (w2_cmdtriage.log:331 lang_bare
    // `0|default|default|`), and r() holds exactly its two macros.
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rcL = rcOf(s, "label language");
    check(rcL == "0" && s.eval.rStrings.get("language") == "default" &&
              s.eval.rStrings.get("languages") == "default" && !s.eval.rValues.contains("N"),
          "W2C(postest): `label language` rc 0, r(language) = r(languages) = default, and the "
          "summarize's r(N) is gone (w2_cmdtriage.log:331). Got rc " + rcL + ", `" +
              s.eval.rStrings.get("language") + "`/`" + s.eval.rStrings.get("languages") + "`");
    // (d) s() is not r(): an sclass program's s(foo) survives `label language` and a
    // summarize. NO ORACLE (w2o_rlifecycle arm S01 not captured): it pins the
    // `return clear` convention eval::replaceRClass shares. Was empty on 8dc9d335 (the
    // whole rStrings dict, s() included, was cleared).
    try {
      (void)piet::doexec::executeDoSource("program define w2c_mys2, sclass\n"
                                          "  sreturn local foo \"bar\"\n"
                                          "end\n",
                                          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    (void)piet::dispatch("w2c_mys2", s);
    (void)piet::dispatch("label language", s);
    const auto d1 = shown(s, "display \"S|`s(foo)'|\"");
    (void)piet::dispatch("quietly summarize x1", s);
    const auto d2 = shown(s, "display \"S|`s(foo)'|\"");
    check(d1.second == "S|bar|" && d2.second == "S|bar|",
          "W2C(postest): s(foo) of an sclass program survives `label language` and `summarize` "
          "(no oracle; the `return clear` convention). Got `" + d1.second + "` / `" + d2.second +
              "`");
    // (e) E07: `summarize x1` / `regress` / r(N), r(mean) read `.` at rc 0, in display
    // and in `local v = r(N)` (w2n_followups.log:968 `0|.|.`; FU-91). Was 60 / 5.25
    // (and on 8dc9d335 too: the fit wrote no r()).
    (void)piet::dispatch("quietly summarize x1", s);
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    const auto e1 = shown(s, "display \"E|\" r(N) \"|\" r(mean) \"|\"");
    const std::string rcE = rcOf(s, "local w2c_en = r(N)");
    const std::string en = s.eval.locals.get("w2c_en", "<unset>");
    check(e1.first == "0" && e1.second == "E|.|.|" && rcE == "0" && en == "." &&
              rTable(s) != nullptr,
          "W2C(postest/E07): after `summarize x1` / `regress` r(N) and r(mean) read `.` at rc 0 "
          "and `local v = r(N)` stores `.`; the fit's own r(table) is posted "
          "(w2n_followups.log:968). Got rc " + e1.first + " `" + e1.second + "`, local rc " +
              rcE + " `" + en + "`");
    // (f) a MERGING r-class handler replaces r() too: regress / summarize / correlate /
    // r(mean) is `.` (DIAGNOSIS B row 3; correlate is r-class, r(rho) its own).
    (void)piet::dispatch("quietly summarize x1", s);
    (void)piet::dispatch("quietly correlate y x1", s);
    const auto f = shown(s, "display \"F|\" r(mean) \"|\" (r(rho) > .9) \"|\"");
    check(f.first == "0" && f.second == "F|.|1|",
          "W2C(postest): `summarize x1` / `correlate y x1` leaves r(rho) and no r(mean) (was the "
          "summarize's 5.25: correlate merged into r()). Got rc " + f.first + " `" + f.second +
              "`");
    // (g) FU-49 shape: regress / lincom / `quietly count` / r(estimate) is `.`.
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("quietly lincom x1", s);
    const bool hadEst = s.eval.rValues.contains("estimate");
    (void)piet::dispatch("quietly count", s);
    const auto g = shown(s, "display \"G|\" r(estimate) \"|\" r(N) \"|\"");
    check(hadEst && g.first == "0" && g.second == "G|.|60|",
          "W2C(postest): `lincom x1` / `count` leaves r(N) 60 and no r(estimate) (FU-49; was "
          "lincom's estimate: count merged into r()). Got rc " + g.first + " `" + g.second + "`");
    // (h) R6 + DIAGNOSIS B row 1: `levelsof c` / `summarize x1` / `r(levels)' expands to
    // nothing (an unset delimited r() is the empty string, as an unset `e(NAME)' is).
    (void)piet::dispatch("quietly levelsof c", s);
    (void)piet::dispatch("quietly summarize x1", s);
    const auto h = shown(s, "display \"H|`r(levels)'|\"");
    check(h.first == "0" && h.second == "H||",
          "W2C(postest): `levelsof c` / `summarize x1` / `r(levels)' is empty at rc 0 (was the "
          "levelsof list: summarize left r() macros in place). Got rc " + h.first + " `" +
              h.second + "`");
    // (l) DIAGNOSIS C R4a / R6 / R8: with no r(table), `display r(table)` prints `.`;
    // `r(nosuch)' is empty; `count if x1 > r(nosuch)` counts 0 (`x1 > .` is false).
    // All three were rc 1 ("r(...) is not set").
    (void)piet::dispatch("quietly summarize x1", s);
    const auto l1 = shown(s, "display r(table)");
    const auto l2 = shown(s, "display \"L|`r(nosuch)'|\"");
    const std::string rcC = rcOf(s, "quietly count if x1 > r(nosuch)");
    const double nC = rv(s, "N");
    check(l1.first == "0" && trimmed(l1.second) == "." && l2.first == "0" &&
              l2.second == "L||" && rcC == "0" && nC == 0.0,
          "W2C(postest): absent r() reads are Stata's missing/empty at rc 0: `display r(table)` "
          "`.`, `r(nosuch)' empty, `count if x1 > r(nosuch)` r(N) 0. Got rc " + l1.first + " `" +
              l1.second + "`, rc " + l2.first + " `" + l2.second + "`, rc " + rcC + " r(N) " +
              num(nC));
    // E08 / E09: a FAILING r-class command (parse error, r(111)) writes nothing and so
    // leaves r() and r(table) as they were (w2n_followups.log:988 `111|0|..`, :1000
    // `111|60|5.25...`).
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    const std::string rc8 = rcOf(s, "summarize w2c_nosuchvar");
    const auto e8 = cell();
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rc9 = rcOf(s, "summarize w2c_nosuchvar");
    check(rc8 == "111" && e8.first == "0" && rc9 == "111" && rv(s, "N") == 60.0 &&
              rel(rv(s, "mean"), 5.25, 1e-6),
          "W2C(postest/E08-E09): a failing `summarize nosuchvar` (r(111)) leaves r(table) and "
          "r(N)/r(mean). Got rc " + rc8 + ", table read rc " + e8.first + ", rc " + rc9 +
              ", r(N) " + num(rv(s, "N")) + ", r(mean) " + num(rv(s, "mean")));
    // (i) a plain (non-class) program's inner `count` reaches the caller, also when the
    // program then fails: r(N) is the count's. PIET's current answer, NO ORACLE
    // (w2o_rlifecycle arm P07 not captured; DIAGNOSIS B row 21 predicts the leak).
    try {
      (void)piet::doexec::executeDoSource("program define w2c_myp\n"
                                          "  quietly count if x1 > 0\n"
                                          "  error 198\n"
                                          "end\n",
                                          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    (void)piet::dispatch("quietly count if x1 > 0", s);
    const double nPos = rv(s, "N");
    (void)piet::dispatch("quietly summarize x1", s);
    const std::string rcP2 = rcOf(s, "w2c_myp");
    check(std::isfinite(nPos) && nPos < 60 && rcP2 == "198" && rv(s, "N") == nPos,
          "W2C(postest): a plain program's inner `count` leaves its r(N) (" + num(nPos) +
              ") behind, also when the program fails r(198) (no oracle). Got rc " + rcP2 +
              ", r(N) " + num(rv(s, "N")));
    // RULING.md (postest-rtable/independent) C.1 / E.2: an absent r(NAME) is `.`
    // (bare) / empty (delimited) only OUTSIDE the posted set of the live r-class
    // command. A name Stata posts there and PIET does not is a NAMED r(111) in every
    // form; after an undeclared (class U) command every absent bare/delimited read
    // is r(111). On 58881bb4 every one of these absent reads was `.` / empty at rc 0
    // (review 10, rev10/p3: lincom r(level) `.` where w2h_postest_refit.log:191
    // pins 95), and on main rc 1.
    auto rcDo = [&](const std::string& src) {
      std::string rc = "0";
      try {
        const auto out = piet::doexec::executeDoSource(src, s, piet::dispatch);
        for (const auto& b : out.blocks)
          if (b.is_object() && b.value("type", std::string()) == "error")
            rc = piet::eval::coarseRc(b);
      } catch (const std::exception&) {
        rc = "throw";
      }
      return rc;
    };
    // 1. regress / lincom x1 / r(level) is 95 in every form (w2h_postest_refit.log:191
    // R_ctl_lincomdiff, r(level) field 95). Was `.` / `.` / empty / not taken.
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("quietly lincom x1", s);
    const auto lv1 = shown(s, "display r(level)");
    s.eval.locals.erase("w2c_lv");
    const std::string rcLv = rcOf(s, "local w2c_lv = r(level)");
    const std::string lvLocal = s.eval.locals.get("w2c_lv", "<unset>");
    const auto lvDelim = shown(s, "display \"`r(level)'\"");
    s.eval.locals.erase("w2c_lt");
    const std::string rcLt = rcDo("if r(level) == 95 {\n  local w2c_lt = 1\n}\n");
    const std::string lt = s.eval.locals.get("w2c_lt", "<unset>");
    check(lv1.first == "0" && trimmed(lv1.second) == "95" && rcLv == "0" && lvLocal == "95" &&
              lvDelim.first == "0" && numOf(lvDelim.second) == 95.0 && rcLt == "0" && lt == "1",
          "W2C(postest/rlevel-1): `regress` / `lincom x1`: r(level) is 95 in display, `local`, "
          "`r(level)' and an `if` condition (w2h_postest_refit.log:191). Got rc " + lv1.first +
              " `" + lv1.second + "`, local rc " + rcLv + " `" + lvLocal + "`, delimited rc " +
              lvDelim.first + " `" + lvDelim.second + "`, if rc " + rcLt + " w2c_lt `" + lt + "`");
    // 2. lincom x1, level(90): r(level) 90 (help lincom).
    (void)piet::dispatch("quietly lincom x1, level(90)", s);
    check(rv(s, "level") == 90.0,
          "W2C(postest/rlevel-2): `lincom x1, level(90)` posts r(level) 90 (help lincom). "
          "Got " + num(rv(s, "level")));
    // 3. set level 90 / lincom x1 / r(level): 90, the interval at 90 too
    // (w2o_rnames.log:111 N03).
    (void)piet::dispatch("set level 90", s);
    (void)piet::dispatch("quietly lincom x1", s);
    const auto lv3 = shown(s, "display r(level)");
    (void)piet::dispatch("set level 95", s);
    check(lv3.first == "0" && trimmed(lv3.second) == "90",
          "W2C(postest/rlevel-3): `set level 90` / `lincom x1` / `display r(level)` is 90 "
          "(w2o_rnames.log:111 N03). Got rc " + lv3.first + " `" + lv3.second + "`");
    // 4. ttest x1 == 5: r(level) 95, and 90 under `set level 90` (help ttest).
    (void)piet::dispatch("quietly ttest x1 == 5", s);
    const double tl95 = rv(s, "level");
    (void)piet::dispatch("set level 90", s);
    (void)piet::dispatch("quietly ttest x1 == 5", s);
    const double tl90 = rv(s, "level");
    (void)piet::dispatch("set level 95", s);
    const auto tl = shown(s, "display r(level)");
    check(tl95 == 95.0 && tl90 == 90.0 && tl.first == "0" && trimmed(tl.second) == "90",
          "W2C(postest/rlevel-4): `ttest x1 == 5` posts r(level) 95, and 90 after `set level 90` "
          "(help ttest). Got " + num(tl95) + " / " + num(tl90) + ", display rc " + tl.first +
              " `" + tl.second + "`");
    // 5. two-way tabulate with chi2: r(chi2) and r(c) are Stata's, and since BUG-1012
    // PIET's handler posts them (until then it posted neither and this arm pinned the
    // named r(111), FU-b): `tab g d, chi2` is rc 0 with r(N) r(r) r(c) r(chi2) r(p)
    // (w2as_summary_stats_family.log:1222 sx_tab_two_chi2). Without chi2, r(chi2) is
    // outside the set (`.`, :1159 sx_tab_two_u); one-way, r(c) is outside it (`.`,
    // :1076 sx_tab_one_u). (help tabulate twoway, help tabulate oneway.)
    (void)piet::dispatch("quietly tabulate c d, chi2", s);
    const double t5chi2 = rv(s, "chi2"), t5cols = rv(s, "c");
    const auto t5a = shown(s, "display r(chi2)");
    const auto t5b = shown(s, "display r(c)");
    const std::string t5if = rcDo("if r(chi2) > 0 {\n  local w2c_t5 = 1\n}\n");
    (void)piet::dispatch("quietly tabulate c d", s);
    const auto t5c = shown(s, "display r(chi2)");
    (void)piet::dispatch("quietly tabulate c", s);
    const auto t5d = shown(s, "display r(c)");
    check(t5a.first == "0" && std::isfinite(t5chi2) && t5chi2 > 0 &&
              rel(numOf(t5a.second), t5chi2, 1e-6) && t5b.first == "0" && t5cols == 2.0 &&
              numOf(t5b.second) == t5cols && t5if == "0" && t5c.first == "0" &&
              trimmed(t5c.second) == "." && t5d.first == "0" && trimmed(t5d.second) == ".",
          "W2C(postest/rlevel-5): `tabulate c d, chi2` posts r(chi2) and r(c) 2, read at rc 0 in "
          "display and in `if r(chi2) > 0` (w2as_summary_stats_family.log:1222); `tabulate c d`: "
          "r(chi2) `.` (:1159); `tabulate c`: r(c) `.` (:1076). Got rc " + t5a.first + " `" +
              t5a.second + "` (posted " + num(t5chi2) + ") / " + t5b.first + " `" + t5b.second +
              "` / if " + t5if + " / " + t5c.first + " `" + t5c.second + "` / " + t5d.first +
              " `" + t5d.second + "`");
    // 6. nlcom posts r(V) (w2h_postest_refit.log:229 reads sqrt(el(r(V),1,1)) at rc 0;
    // FU-410): for one expression it is the table's se squared. r(level) is posted
    // (w2o_rnames.log:340 N20 `...|95|`).
    (void)piet::dispatch("quietly regress y x1 x2", s);
    (void)piet::dispatch("quietly nlcom _b[x1]/_b[x2]", s);
    const auto n6a = shown(s, "display el(r(V),1,1)");
    const auto n6b = shown(s, "display r(level)");
    const auto* n6V = s.eval.findClassMatrix('r', "V");
    const auto* n6T = rTable(s);
    const double n6se = n6T ? n6T->data[1][0] : std::nan("");
    check(n6a.first == "0" && n6V != nullptr && n6V->data.size() == 1 &&
              rel(n6V->data[0][0], n6se * n6se, 1e-12) && n6b.first == "0" &&
              trimmed(n6b.second) == "95",
          "W2C(postest/rlevel-6): after `nlcom`, `el(r(V),1,1)` reads at rc 0 and equals the "
          "table's se squared (w2h_postest_refit.log:229), and r(level) is 95 "
          "(w2o_rnames.log:340 N20). Got rc " + n6a.first + " `" + n6a.second + "`, rc " +
              n6b.first + " `" + n6b.second + "`");
    // 7. testparm i.c posts r(drop) 0 (w2h_postest_refit.log:173, last field).
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("quietly testparm i.c", s);
    const auto t7 = shown(s, "display r(drop)");
    check(rv(s, "drop") == 0.0 && t7.first == "0" && trimmed(t7.second) == "0",
          "W2C(postest/rlevel-7): `testparm i.c` posts r(drop) 0 (w2h_postest_refit.log:173). "
          "Got " + num(rv(s, "drop")) + ", display rc " + t7.first + " `" + t7.second + "`");
    // 8. describe posts r(changed), as Stata does (w2b_dataio.log:173 `changed=1`,
    // FU-i; BUG-1447 -- this check pinned its r(111) refusal until then), and its
    // own r(k) reads.
    (void)piet::dispatch("quietly describe", s);
    const auto d8a = shown(s, "display r(changed)");
    const auto d8b = shown(s, "display r(k)");
    check(d8a.first == "0" && trimmed(d8a.second) == "1" && d8b.first == "0" &&
              numOf(d8b.second) == rv(s, "k") && rv(s, "k") >= 9.0,
          "W2C(postest/rlevel-8): after `describe` r(changed) is 1 (w2b_dataio.log:173) "
          "and its own r(k) reads (the variable count). Got rc " + d8a.first + " `" + d8a.second + "`, rc " + d8b.first +
              " `" + d8b.second + "`");
    // 9. margins declares and posts its own r() (FU-276): after a linear fit bare
    // `margins` posts r(N) (w2o_rnames.log:436 N26: r(N) 60, r(mean) `.`).
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("capture margins", s);
    const auto m9 = shown(s, "display r(N)");
    const auto m9m = shown(s, "display r(mean)");
    check(m9.first == "0" && trimmed(m9.second) == "60" && m9m.first == "0" &&
              trimmed(m9m.second) == ".",
          "W2C(postest/rlevel-9): `regress` / `capture margins` / `display r(N)` is 60 and "
          "r(mean) is `.` (w2o_rnames.log:436). Got rc " + m9.first + " `" + m9.second +
              "`, rc " + m9m.first + " `" + m9m.second + "`");
    // 10. bare `label language` posts r(k), the number of languages
    // (w2ao_label_family.log lb_lang_bare `k=+1.0000000000000X+000`; LANG-RK).
    (void)piet::dispatch("label language", s);
    const auto k10 = shown(s, "display r(k)");
    check(k10.first == "0" && trimmed(k10.second) == "1",
          "W2C(postest/rlevel-10): `label language` / `display r(k)` is 1 "
          "(w2ao_label_family lb_lang_bare). Got rc " + k10.first + " `" + k10.second + "`");
    // 11. an rclass program's return set IS r() (class X): r(nosuch) is `.`.
    try {
      (void)piet::doexec::executeDoSource("program define w2c_myr11, rclass\n"
                                          "  return scalar k = 5\n"
                                          "end\n",
                                          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    (void)piet::dispatch("quietly w2c_myr11", s);
    const auto x11 = shown(s, "display r(nosuch)");
    check(rv(s, "k") == 5.0 && x11.first == "0" && trimmed(x11.second) == ".",
          "W2C(postest/rlevel-11): after an rclass program `return scalar k = 5`, r(nosuch) is "
          "`.` at rc 0 (class X). Got r(k) " + num(rv(s, "k")) + ", rc " + x11.first + " `" +
              x11.second + "`");
    // 12. `return clear` OUTSIDE a program is rc 0 and leaves r() alone
    // (w2o_rnames.log:528 N34 `0|60|60`): r(N) is the summarize's.
    (void)piet::dispatch("quietly summarize x1", s);
    const auto x12a = shown(s, "display r(N)");
    (void)piet::dispatch("return clear", s);
    const auto x12 = shown(s, "display r(N)");
    check(x12.first == "0" && trimmed(x12.second) == trimmed(x12a.second),
          "W2C(postest/rlevel-12): top-level `return clear` / `display r(N)` keeps r(N) "
          "(w2o_rnames.log:528 N34). Got rc " + x12.first + " `" + x12.second + "`");
    // 13. correlate's r(sum_w) (help correlate) is `.` after an unweighted
    // correlate: w2as_summary_stats_family.log:6088 (sx_corr_two) and :6133
    // (sx_corr_one) read it as +1.0000000000000X+3ff at rc 0. PIET posted no r(sum_w)
    // before BUG-1014 and this arm pinned the named r(111) (FU-k); the handler now
    // posts correlate's set, so the read is Stata's `.`. Arm (f)'s r(mean) `.` stands:
    // mean is outside correlate's set.
    (void)piet::dispatch("quietly correlate y x1", s);
    const auto c13 = shown(s, "display r(sum_w)");
    check(c13.first == "0" && trimmed(c13.second) == ".",
          "W2C(postest/rlevel-13): `correlate y x1` / `display r(sum_w)` is `.` at rc 0 "
          "(w2as_summary_stats_family.log:6088). Got rc " + c13.first + " `" + c13.second + "`");
    // 14. `timer list` adds to r() and is not an r() event: the live identity stays
    // lincom's and r(level) still reads 95.
    (void)piet::dispatch("quietly regress y x1 i.c", s);
    (void)piet::dispatch("quietly lincom x1", s);
    (void)piet::dispatch("timer clear 1", s);
    (void)piet::dispatch("timer on 1", s);
    (void)piet::dispatch("timer off 1", s);
    (void)piet::dispatch("quietly timer list 1", s);
    const auto t14 = shown(s, "display r(level)");
    check(s.eval.rClassCmd == "lincom" && t14.first == "0" && trimmed(t14.second) == "95",
          "W2C(postest/rlevel-14): `lincom x1` / `timer list 1`: the live r-class command is "
          "still lincom and r(level) reads 95. Got `" + s.eval.rClassCmd + "`, rc " + t14.first +
              " `" + t14.second + "`");
  }

  {
    // Unit level: the two halves of the r() replacement (eval/session.h,
    // handlers::replaceStaleRClass, eval::replaceRClass). A write of the SAME value is a
    // write; putting back a saved copy (runAssert's guard) is not; erasing an r() matrix
    // alone is a write; reserved \x01 keys (s()) are never erased; a table with no
    // bookkeeping at all (the 10-11 shape: a stamp wiped, the table left) still goes at
    // the next r() write.
    using piet::eval::EvalMatrix;
    piet::eval::EvalSession ev;
    ev.rValues.set("N", 60);
    ev.rValues.set("mean", 5.25);
    ev.rStrings.set("levels", "0 1");
    ev.rStrings.set("\x01s:foo", "bar");
    ev.rMatrices.emplace_back("table", EvalMatrix{});
    const auto m0 = ev.rMark();
    const bool w0 = piet::handlers::replaceStaleRClass(ev, m0);
    {
      const auto savedV = ev.rValues;
      const auto savedS = ev.rStrings;
      ev.rValues.clear();
      ev.rValues.set("N", 7);
      ev.rValues = savedV;
      ev.rStrings = savedS;
    }
    ev.rStrings.set("\x01s:foo", "baz");
    const bool w1 = piet::handlers::replaceStaleRClass(ev, m0);
    ev.rValues.set("N", 60);
    const bool w2 = piet::handlers::replaceStaleRClass(ev, m0);
    const bool afterW2 = ev.rValues.items().size() == 1 && rel(*ev.rValues.find("N"), 60, 0) &&
                         !ev.rStrings.contains("levels") && ev.rStrings.get("\x01s:foo") == "baz" &&
                         ev.rMatrices.empty();
    ev.rMatrices.emplace_back("S", EvalMatrix{});
    ev.rValues.set("k", 1);
    const auto m1 = ev.rMark();
    ev.rMatrices.clear();
    const bool w3 = piet::handlers::replaceStaleRClass(ev, m1);
    const bool afterW3 = ev.rValues.items().empty() && ev.rMatrices.empty();
    ev.rStrings.set("x", "y");
    piet::eval::replaceRClass(ev);
    check(!w0 && !w1 && w2 && afterW2 && w3 && afterW3 && ev.rStrings.items().size() == 1 &&
              ev.rStrings.get("\x01s:foo") == "baz" && ev.rValues.items().empty(),
          std::string("W2C(postest/unit): r() replacement -- no write / a restored copy / an "
                      "s() write erase nothing; a same-value write replaces r() and its "
                      "matrices; an r() matrix erase alone is a write; replaceRClass keeps s(). "
                      "Got ") +
              (w0 ? "w0 " : "") + (w1 ? "w1 " : "") + (w2 ? "" : "!w2 ") +
              (afterW2 ? "" : "!afterW2 ") + (w3 ? "" : "!w3 ") + (afterW3 ? "" : "!afterW3"));
  }

  {
    // display's VERBATIM fallback (the line is not one expression) prints a resolved
    // e()/_b[] value as it did before BUG-554 (`%.9g`): `60`, not `60.0`; nine
    // significant digits, not seventeen. Full precision is for expression and %fmt
    // contexts only. Old (this branch at 65b6d113): `_col(3) 60.0`, `E|1.97...0437`.
    // BUG-1333: the juxtaposition is two expressions, each printed at display's
    // default and back to back -- StataBE 19.5 `display "E|" _b[x1] _b[2.c]` is
    // `E|1.61488551.213187` on its own fit. This leg held the verbatim fallback's
    // `E|<%.9g> <%.9g>` before, the defect itself.
    (void)piet::dispatch("quietly regress y x1 x2 i.c", s);
    double bx1 = std::nan(""), b2 = std::nan(""), nObs = std::nan("");
    if (s.lastEstBlock.has_value()) {
      for (const auto& row : (*s.lastEstBlock)["coeftable"]) {
        if (row.value("name", std::string()) == "x1") bx1 = row.value("b", std::nan(""));
        if (row.value("name", std::string()) == "2.c") b2 = row.value("b", std::nan(""));
      }
      nObs = s.lastEstBlock->value("n_obs", std::nan(""));
    }
    auto g9 = [](double v) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "%.9g", v);
      return std::string(buf);
    };
    auto g17 = [](double v) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "%.17g", v);
      return std::string(buf);
    };
    const auto n = shown(s, "display _col(3) e(N)");
    const auto e = shown(s, "display \"E|\" _b[x1] _b[2.c]");
    const std::string nTail = " " + g9(nObs);
    const bool nOk = n.first == "0" && n.second.size() >= nTail.size() &&
                     n.second.compare(n.second.size() - nTail.size(), nTail.size(), nTail) == 0 &&
                     n.second.find(g9(nObs) + ".0") == std::string::npos;
    // display's default for an unformatted item over these coefficients'
    // magnitudes (1e-4 <= |v| < 1e8): eight significant digits in nine
    // characters, a fraction's leading 0 dropped.
    auto d8 = [](double v) {
      char buf[48];
      for (int prec = 8; prec >= 1; --prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        std::string t(buf);
        const size_t sign = (!t.empty() && t[0] == '-') ? 1 : 0;
        if (t.compare(sign, 2, "0.") == 0) t.erase(sign, 1);
        if (t.size() - sign <= 9) return t;
      }
      return std::string(buf);
    };
    const bool eOk = e.first == "0" && e.second == "E|" + d8(bx1) + d8(b2) &&
                     e.second.find(g17(bx1)) == std::string::npos &&
                     e.second.find(g17(b2)) == std::string::npos;
    check(std::isfinite(nObs) && std::isfinite(b2) && nOk && eOk,
          "W2C(postest/E): verbatim `display _col(3) e(N)` ends in `" + g9(nObs) +
              "` (no `.0`), `display \"E|\" _b[x1] _b[2.c]` is `E|" + d8(bx1) + d8(b2) +
              "` (two expressions at the default, back to back). Got `" + n.second + "` / `" +
              e.second + "`");
  }

  {
    // No estimation in memory: Stata's `display _b[x]` is r(111) "_b not allowed when e(b)
    // is not present" (StataNow 19.5 BE capture of 2026-09-29, not yet a landed golden:
    // _b and _se both rc 111). Was "last estimates not found" at rc 1.
    piet::SessionState s0(workdir);
    const std::string nb = rcOf(s0, "display _b[x1]");
    const std::string ns = rcOf(s0, "display _se[x1]");
    check(nb == "111" && ns == "111",
          "W2C(postest/E): `display _b[x1]` / `_se[x1]` with no estimation are r(111). Got rc " +
              nb + "/" + ns);
  }

  {
    // FU-131: postestimation after a fit made without a constant answers from the
    // no-constant model (StataBE, tests/eval/golden/aer_w2/w2p_estim_ev.log section G).
    // Main refused every verb at rc 198 (BUG-603's interim guard) because the engine
    // refit always appended the constant. The fixture is the .do's own, every
    // variable but d stored double, so the literal lines reproduce it.
    piet::SessionState g(workdir);
    for (const char* line :
         {"clear", "set obs 60", "generate double id = ceil(_n/6)",
          "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
          "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
          "generate double y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5",
          "generate byte d = (y > 12)"})
      (void)piet::dispatch(line, g);
    auto el = [&g](const std::string& expr) { return numOf(shown(g, "display %18.0g " + expr).second); };
    auto pair2 = [](const std::string& arm, const std::string& rc, double a, double wa, double b,
                    double wb, double rtolA, double rtolB) {
      check(rc == "0" && rel(a, wa, rtolA) && rel(b, wb, rtolB),
            "W2C(postest/FU-131): " + arm + " -- rc 0, " + num(wa) + " / " + num(wb) +
                " (w2p_estim_ev.log PREC|" + arm + "|; main: rc 198). Got rc " + rc + ", " +
                num(a) + " / " + num(b));
    };

    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    std::string rc = rcOf(g, "lincom x1");
    pair2("G_nc_lincom_x1", rc, rv(g, "estimate"), 2.356553697480766, rv(g, "se"),
          .0783607746432686, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "lincom x1 + x2");
    pair2("G_nc_lincom_sum", rc, rv(g, "estimate"), 2.133177809016356, rv(g, "se"),
          .1283009449957656, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "test x1");
    check(rc == "0" && rel(rv(g, "F"), 904.3930979008784, 1e-9) && rv(g, "df") == 1 &&
              rv(g, "df_r") == 58 && pAtol(rv(g, "p"), 4.50379007618e-37),
          "W2C(postest/FU-131): G_nc_test -- rc 0, r(F) 904.3930979008784 r(df) 1 r(df_r) 58 "
          "(N - 2: no constant) (w2p_estim_ev.log:1329; main: rc 198). Got rc " + rc + ", F " +
              num(rv(g, "F")) + ", df_r " + num(rv(g, "df_r")));
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "nlcom 2*_b[x1]");
    pair2("G_nc_nlcom", rc, el("el(r(b),1,1)"), 4.713107394961533, el("el(r(V),1,1)"),
          .0245616440111472, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "margins, at(x1=5)");
    pair2("G_nc_margins_at", rc, el("el(r(b),1,1)"), 11.26677018505104, el("el(r(V),1,1)"),
          .0790691558659889, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "predict double xb_nc, xb");
    std::string src = rcOf(g, "summarize xb_nc");
    check(rc == "0" && src == "0" && rv(g, "N") == 60 &&
              rel(rv(g, "mean"), 11.85590860942123, 1e-9) && rel(rv(g, "sd"), 7.468862389658671, 1e-9),
          "W2C(postest/FU-131): G_nc_predict_xb -- rc 0, N 60, mean 11.85590860942123, sd "
          "7.468862389658671 (w2p_estim_ev.log:1387; main: rc 198). Got rc " + rc + "/" + src +
              ", mean " + num(rv(g, "mean")) + ", sd " + num(rv(g, "sd")));
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    rc = rcOf(g, "predict double res_nc, residuals");
    src = rcOf(g, "summarize res_nc");
    check(rc == "0" && src == "0" && rv(g, "N") == 60 && rel(rv(g, "mean"), .8174247239120984, 1e-9),
          "W2C(postest/FU-131): G_nc_predict_res -- rc 0, N 60, mean .8174247239120984 (the "
          "no-constant residuals do not average to 0; w2p_estim_ev.log:1402; main: rc 198). "
          "Got rc " + rc + "/" + src + ", mean " + num(rv(g, "mean")));

    // Controls, with the constant (they matched on main and must keep matching).
    (void)piet::dispatch("quietly regress y x1 x2", g);
    rc = rcOf(g, "lincom x1");
    pair2("G_c_lincom_x1", rc, rv(g, "estimate"), 1.959601560760043, rv(g, "se"),
          .0568880588246571, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2", g);
    rc = rcOf(g, "test x1");
    check(rc == "0" && rel(rv(g, "F"), 1186.569890873353, 1e-9) && rv(g, "df_r") == 57,
          "W2C(postest/FU-131): G_c_test -- control, r(F) 1186.569890873353 r(df_r) 57 "
          "(w2p_estim_ev.log:1483). Got rc " + rc + ", F " + num(rv(g, "F")));
    // FU-502: nlcom's r(V) and margins' r(b)/r(V), with the constant too
    // (w2p_estim_ev.log:1502 G_c_nlcom, :1526 G_c_margins_at; r(111) on d73dd22c).
    (void)piet::dispatch("quietly regress y x1 x2", g);
    rc = rcOf(g, "nlcom 2*_b[x1]");
    pair2("G_c_nlcom", rc, el("el(r(b),1,1)"), 3.919203121520086, el("el(r(V),1,1)"),
          .0129450049474129, 1e-9, 1e-9);
    (void)piet::dispatch("quietly regress y x1 x2", g);
    rc = rcOf(g, "margins, at(x1=5)");
    pair2("G_c_margins_at", rc, el("el(r(b),1,1)"), 12.18343294314332, el("el(r(V),1,1)"),
          .0320938394163614, 1e-9, 1e-9);

    // probit, noconstant. lincom x1 is the fit's own _b[x1]/_se[x1] (1e-12: the refit
    // is the fit), and both sit on the oracle within T3 (iterative MLE).
    Reply1 pf = fitOf(g, "probit d x1 x2, noconstant");
    rc = rcOf(g, "lincom x1");
    check(pf.rc == "0" && rc == "0" && rel(rv(g, "estimate"), pf.b, 1e-12) &&
              rel(rv(g, "se"), pf.se, 1e-12),
          "W2C(postest/FU-131): G_pnc_lincom_x1 -- r(estimate)/r(se) equal the probit "
          "noconstant fit's _b[x1]/_se[x1] to 1e-12. Got rc " + rc + ", " +
              num(rv(g, "estimate")) + "/" + num(rv(g, "se")) + " vs " + num(pf.b) + "/" +
              num(pf.se));
    pair2("G_pnc_lincom_x1", rc, rv(g, "estimate"), .4484790061399406, rv(g, "se"),
          .1036892925039664, 1e-7, 1e-7);
    (void)piet::dispatch("quietly probit d x1 x2, noconstant", g);
    rc = rcOf(g, "margins, dydx(x1)");
    pair2("G_pnc_margins_dydx", rc, el("el(r(b),1,1)"), .0826720054343255, el("el(r(V),1,1)"),
          .0000196420060519, 1e-7, 2e-7);
    (void)piet::dispatch("quietly probit d x1 x2", g);
    rc = rcOf(g, "margins, dydx(x1)");
    pair2("G_pc_margins_dydx", rc, el("el(r(b),1,1)"), .089805000869499, el("el(r(V),1,1)"),
          .0000147564915036, 1e-7, 2e-7);
  }

  {
    // FU-326: margins, dydx() after a linear model the coefficient table does not
    // answer (tobit here) is a refusal by name at r(198), message unchanged. Main
    // passed no code, so the default engine_error made `display _rc` 1.
    piet::SessionState g(workdir);
    loadFixture(g, csv);
    (void)piet::dispatch("quietly tobit y x1 x2, ll(0)", g);
    std::string msg;
    for (const auto& b : piet::dispatch("margins, dydx(x1)", g))
      if (b.is_object() && b.value("type", std::string()) == "error")
        msg = b.value("message", std::string());
    (void)piet::dispatch("capture noisily margins, dydx(x1)", g);
    const std::string rc = shown(g, "display _rc").second;
    check(rc == "198" &&
              msg.rfind("margins: marginal effects (dydx) after a linear model are not supported.", 0) == 0,
          "W2C(postest/FU-326): `margins, dydx(x1)` after tobit -- r(198), message unchanged "
          "(main: _rc 1). Got _rc `" + rc + "` `" + msg + "`");
    // FU-437: an estat subcommand PIET does not serve after a non-csdid fit is r(198).
    (void)piet::dispatch("quietly regress y x1 x2", g);
    (void)piet::dispatch("capture quietly estat ic", g);
    const std::string rcIc = shown(g, "display _rc").second;
    check(rcIc == "198",
          "W2C(postest/FU-437): `regress` then `capture quietly estat ic` -- _rc 198 (main: 1). "
          "Got `" + rcIc + "`");
  }

  {
    // FU-504: the estat diagnostics refit least squares WITH a constant (cobra's
    // cmds_diag.cpp has no --no-intercept), so after a no-constant fit they tested
    // another model at rc 0. Main, on this fixture: `estat hettest` after
    // `regress y x1 x2, noconstant` printed F 3.0072778566276313 chi2
    // 1.9485382710886938 -- the with-constant fit's numbers -- and ovtest, vif and
    // the hettest z-list form were rc 0 too. No oracle captures them after
    // noconstant, so they refuse by name at r(198); an explicit varlist still runs.
    piet::SessionState g(workdir);
    for (const char* line :
         {"clear", "set obs 60", "generate double id = ceil(_n/6)",
          "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
          "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
          "generate double y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5"})
      (void)piet::dispatch(line, g);
    for (const char* diag : {"hettest", "ovtest", "vif", "hettest x2"}) {
      (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
      (void)piet::dispatch(std::string("capture estat ") + diag, g);
      const std::string rc = shown(g, "display _rc").second;
      check(rc == "198",
            std::string("W2C(postest/FU-504): `regress y x1 x2, noconstant` then `capture estat ") +
                diag + "` -- _rc 198 (main: 0, the with-constant model's statistic). Got `" + rc +
                "`");
    }
    (void)piet::dispatch("quietly regress y x1 x2, noconstant", g);
    (void)piet::dispatch("capture estat ovtest y x1 x2", g);
    const std::string rcExplicit = shown(g, "display _rc").second;
    check(rcExplicit == "0",
          "W2C(postest/FU-504): an explicit `estat ovtest y x1 x2` after noconstant still runs "
          "as typed -- _rc 0. Got `" + rcExplicit + "`");
    // Control: with the constant, `estat hettest` is unchanged, byte for byte as on main.
    // EM0 re-pin: the fixture's `1 + 2*x1 - x2 + id/3 + ...` now groups as Stata's
    // [U] 13.2.5 order does (1 + (2*x1 - x2) + ...; oracle w2am_expr_binary_order
    // eo_gen_pm), so y moves by an ulp in some rows and the last digits below with it.
    (void)piet::dispatch("quietly regress y x1 x2", g);
    std::string rc = "0", block;
    for (const auto& b : piet::dispatch("estat hettest", g)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
      if (b.value("type", std::string()) == "estimation") block = b.dump();
    }
    const std::string mainBlock =
        "{\"type\":\"estimation\",\"estimator\":\"hettest\",\"depvar\":\"\",\"n_obs\":60,"
        "\"F\":3.007277856627642,\"chi2\":1.948538271088701,\"p_F\":0.08820398142629103,"
        "\"df\":1,\"p\":0.16274445304847818,\"df_r\":58.0}";
    check(rc == "0" && block == mainBlock,
          "W2C(postest/FU-504): control -- `estat hettest` after `regress y x1 x2` rc 0, block "
          "byte-identical to main's `" + mainBlock + "`. Got rc " + rc + " `" + block + "`");
  }

  {
    piet::SessionState g(workdir);
    loadFixture(g, csv);
    const std::string grouped = rcOf(g, "regress y i.c##i.(g)");
    const auto groupedB = shown(g, "display %21.15g _b[1.c#1.g]");
    const std::string expanded = rcOf(g, "regress y i.c##i.g");
    const auto expandedB = shown(g, "display %21.15g _b[1.c#1.g]");
    check(grouped == "0" && expanded == "0" && groupedB == expandedB,
          "W2C grouped factor on interaction side is the expanded fitted model");
    (void)piet::dispatch("quietly regress y x1 x2", g);
    const std::string rc = rcOf(g, "predict default_prediction");
    const auto label = shown(g, "display `\"`: variable label default_prediction'\"'");
    check(rc == "0" && label.first == "0" && label.second == "Fitted values",
          "W2C default OLS predict publishes its variable label");
  }

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2c postest: %d checks, %d passed, %d failed\n", g_pass + g_fail, g_pass,
              g_fail);
}
