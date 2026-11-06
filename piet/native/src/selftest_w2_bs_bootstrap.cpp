// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Lane BS (uni-bs) -- selftest arms for the `bootstrap` prefix: the default _b
// exp_list, cluster() resampling, Stata-exact replicate draws and bootstrap's
// e()/r() postings.
//
// Oracle: StataBE 19.5 capture scratch-r150/lee/bs/o1.do -> o1.log (sections
// T1..T12; e(V) cells and _se[] in %21x) and o2.do -> o2.log (A1..A4: e(b_bs),
// e(ci_percentile), the Wald test), o3.log (e(sample), re-pinned in
// selftest_w2_esample.cpp) and o4.do -> o4.log (E1: an eclass program's
// equation-striped posting). The frame below is o1.do's, verbatim
// (deterministic: no RNG in the setup). The draws are Stata's, so every cell
// is pinned at 1e-12 relative; the pre-lane build (Philox draws, no default
// _b, no cluster()) fails every arm here.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

const char* const kFrame[] = {
    "clear",
    "set obs 60",
    "gen double x = mod(_n*7, 13)",
    "gen double y = 0.5*x + mod(_n*37, 11)/7",
    "gen double c = mod(_n*7, 15) + 1",
    "gen str4 cs = \"g\" + string(c)",
    "gen double x2 = x",
    "replace x2 = . if mod(_n, 9) == 0",
    "gen double cm = c",
    "replace cm = . if _n == 17",
};

struct Res {
  std::string rc = "0";
  std::string msg;
};

Res run(const std::string& cmd, piet::SessionState& s) {
  Res r;
  try {
    const ojson blocks = piet::dispatch(cmd, s);
    if (!blocks.is_array()) return r;
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      r.rc = piet::eval::coarseRc(b);
      r.msg = b.value("message", std::string());
      break;
    }
  } catch (const std::exception& e) {
    r.rc = "threw";
    r.msg = e.what();
  }
  return r;
}

// A Stata %21x literal (+1.hhhhhhhhhhhhhX+eee) as a double.
double hx(const char* s) {
  const double sign = s[0] == '-' ? -1.0 : 1.0;
  const char* p = s + 1;
  const double lead = static_cast<double>(p[0] - '0');
  double frac = 0;
  double scale = 1.0 / 16.0;
  for (const char* q = p + 2; *q != 'X'; ++q) {
    const int d = (*q >= '0' && *q <= '9') ? *q - '0' : *q - 'a' + 10;
    frac += d * scale;
    scale /= 16.0;
  }
  const char* e = std::strchr(s, 'X') + 1;
  const int ex = static_cast<int>(std::strtol(e, nullptr, 16));
  return sign * std::ldexp(lead + frac, ex);
}

bool close12(double got, double want) {
  return std::isfinite(got) && std::fabs(got - want) <= 1e-12 * std::fabs(want);
}

std::string g17(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

// e(V) of the live e() against the capture's lower triangle (row-major).
std::string checkV(piet::SessionState& s, const std::vector<std::string>& names,
                   const std::vector<const char*>& cells) {
  const auto V = s.eval.resolveEMatrix("V");
  const auto b = s.eval.resolveEMatrix("b");
  if (!V || !b) return "no e(b)/e(V)";
  if (b->colnames != names) {
    std::string got;
    for (const auto& n : b->colnames) got += n + " ";
    return "e(b) names `" + got + "`";
  }
  size_t c = 0;
  for (size_t i = 0; i < names.size(); ++i)
    for (size_t j = 0; j <= i; ++j, ++c) {
      if (c >= cells.size()) return "too few cells pinned";
      const double want = hx(cells[c]);
      if (i >= V->data.size() || j >= V->data[i].size() || !close12(V->data[i][j], want) ||
          !close12(V->data[j][i], want))
        return "e(V)[" + std::to_string(i + 1) + "," + std::to_string(j + 1) + "] " +
               (i < V->data.size() && j < V->data[i].size() ? g17(V->data[i][j]) : "?") +
               " want " + g17(want);
    }
  return "";
}

double eScalar(piet::SessionState& s, const char* name) {
  const auto* p = s.eval.eScalars.find(name);
  if (p != nullptr) return *p;
  const auto v = piet::eval::evalArith(std::string("e(") + name + ")", &s.eval);
  try {
    return v.has_value() ? std::stod(*v) : std::nan("");
  } catch (const std::exception&) {
    return std::nan("");
  }
}

std::string eStr(piet::SessionState& s, const char* name) {
  return s.eval.eStrings.get(name, std::string());
}

}  // namespace

void testW2BsBootstrap() {
  std::printf("[selftest] --- lane BS: bootstrap prefix -- default _b, cluster(), Stata draws, "
              "e()/r() (scratch-r150/lee/bs/o1.log, o2.log) ---\n");
  std::error_code ec;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2_bs").u8string();
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState s(workdir);
  for (const char* line : kFrame) (void)run(line, s);

  // T1: no exp_list => _b; seed(123); e(V) and the posted e() macros/scalars.
  {
    const Res r = run("quietly bootstrap, rep(50) seed(123): regress y x", s);
    const std::string v = r.rc == "0" ? checkV(s, {"x", "_cons"},
                                               {"+1.2b71fda710434X-00c", "-1.95bea1a4c5ec2X-00a",
                                                "+1.7d2f2be95e4a7X-007"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty(),
            "lane BS T1: `bootstrap, rep(50) seed(123): regress y x` (no exp_list) bootstraps "
            "e(b)'s columns x _cons on Stata's draws: e(V) equals StataBE's to 1e-12 "
            "(o1.log T1|V). " + v);
    const bool macros = eStr(s, "cmd") == "regress" && eStr(s, "vce") == "bootstrap" &&
                        eStr(s, "vcetype") == "Bootstrap" && eStr(s, "prefix") == "bootstrap" &&
                        eStr(s, "cmdname") == "regress" && eStr(s, "depvar") == "y" &&
                        eStr(s, "title") == "Linear regression" && eStr(s, "exp1") == "_b[x]" &&
                        eStr(s, "chi2type") == "Wald" && eStr(s, "clustvar").empty();
    const bool scalars = eScalar(s, "N") == 60 && eScalar(s, "N_reps") == 50 &&
                         eScalar(s, "N_misreps") == 0 && std::isnan(eScalar(s, "N_clust")) &&
                         eScalar(s, "k_eexp") == 2 && eScalar(s, "k_exp") == 0 &&
                         eScalar(s, "rank") == 2 && eScalar(s, "df_m") == 1 &&
                         std::isnan(eScalar(s, "df_r")) && std::isnan(eScalar(s, "F"));
    const double chi2 = eScalar(s, "chi2");
    w2check(macros && scalars && close12(chi2, hx("+1.85d76a4b78447X+009")),
            "lane BS T1: e(cmd) regress, e(vce) bootstrap, e(vcetype) Bootstrap, e(prefix) "
            "bootstrap, e(exp1) _b[x], e(N) 60, e(N_reps) 50, e(N_clust) missing, e(rank) 2, "
            "e(df_r)/e(F) gone, Wald e(chi2) 779.68293 (o1.log T1|SC/MC, o2.log A1|chi2). Got "
            "cmd `" + eStr(s, "cmd") + "` vce `" + eStr(s, "vce") + "` N_reps " +
                g17(eScalar(s, "N_reps")) + " chi2 " + g17(chi2));
    // _se[] and r(table) (z reference, df missing, crit invnormal(.975)).
    const auto se = piet::eval::evalArith("_se[x]", &s.eval);
    double sev = std::nan("");
    try {
      if (se.has_value()) sev = std::stod(*se);
    } catch (const std::exception&) {
    }
    const auto* tbl = s.eval.findClassMatrix('r', "table");
    const bool tblOk = tbl != nullptr && tbl->rownames.size() == 9 && tbl->rownames[2] == "z" &&
                       tbl->data.size() == 9 && tbl->data[0].size() == 2 &&
                       close12(tbl->data[1][0], hx("+1.14df2df3edeeeX-006")) &&
                       close12(tbl->data[3][0], hx("+1.5ce76c95ed065X-238")) &&
                       std::isnan(tbl->data[6][0]) &&
                       close12(tbl->data[7][0], hx("+1.f5c0331eeff83X+000"));
    w2check(close12(sev, hx("+1.14df2df3edeeeX-006")) && tblOk,
            "lane BS T1: _se[x] .0168989 and r(table) (rows b se z pvalue ...; se, pvalue, df "
            "missing, crit 1.959964) equal StataBE's (o1.log T1|SE, T1|T). Got _se[x] " + g17(sev) +
                (tbl == nullptr ? " r(table) absent" : ""));
    // e(ci_percentile) (float replicates, _pctile) and e(b_bs).
    const auto pc = s.eval.findClassMatrix('e', "ci_percentile");
    const auto bb = s.eval.findClassMatrix('e', "b_bs");
    w2check(pc != nullptr && bb != nullptr && pc->data.size() == 2 &&
                close12(pc->data[0][0], hx("+1.c1830c0000000X-002")) &&
                close12(pc->data[1][1], hx("+1.1100520000000X+000")) &&
                close12(bb->data[0][0], hx("+1.e2644aae147aeX-002")),
            "lane BS T1: e(ci_percentile) and e(b_bs) are Stata's (o2.log A1|ci_percentile, "
            "A1|b_bs: float-stored replicates)");
  }

  // T2: cluster(c) -- 15 clusters; e(N_clust), e(clustvar).
  {
    const Res r = run("quietly bootstrap, rep(50) seed(123) cluster(c): regress y x", s);
    const std::string v = r.rc == "0" ? checkV(s, {"x", "_cons"},
                                               {"+1.0032fe856f75aX-00c", "-1.842afa85ec020X-00a",
                                                "+1.7dd9cae010a72X-007"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eScalar(s, "N_clust") == 15 && eStr(s, "clustvar") == "c",
            "lane BS T2: `bootstrap, rep(50) seed(123) cluster(c): regress y x` draws Stata's "
            "clustered replicates (bsample with cluster resampling): e(V) to 1e-12, e(N_clust) 15, "
            "e(clustvar) c (o1.log T2). " + v);
  }

  // T3: named exp_list + cluster: e(cmd) bootstrap, names bx cons, no depvar.
  {
    const Res r = run(
        "quietly bootstrap bx=_b[x] cons=_b[_cons], rep(30) seed(7) cluster(c): regress y x", s);
    const std::string v = r.rc == "0" ? checkV(s, {"bx", "cons"},
                                               {"+1.a511c699f5c8bX-00d", "-1.45720ae8b240bX-00a",
                                                "+1.29fef6781baacX-007"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eStr(s, "cmd") == "bootstrap" && eStr(s, "depvar").empty() &&
                eScalar(s, "k_exp") == 2 && eScalar(s, "k_eexp") == 0,
            "lane BS T3: a named exp_list with cluster(c) posts e(b) bx cons, e(cmd) bootstrap, "
            "no e(depvar), e(k_exp) 2 (o1.log T3). " + v);
  }

  // T5: an r-class command; e(V) 1x1.
  {
    const Res r = run("quietly bootstrap mean=r(mean), rep(20) seed(1): summarize y", s);
    const std::string v =
        r.rc == "0" ? checkV(s, {"mean"}, {"+1.3a14c9020450dX-004"}) : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eStr(s, "title") == "Bootstrap results" && eScalar(s, "N") == 60,
            "lane BS T5: `bootstrap mean=r(mean), rep(20) seed(1): summarize y` resamples the "
            "whole data (no e(sample)); e(V) to 1e-12, e(title) Bootstrap results (o1.log T5). " + v);
  }

  // T6c: missing regressor -> e(sample) (54 rows) is the base; clustered.
  {
    const Res r = run("quietly bootstrap, rep(30) seed(11) cluster(c): regress y x2", s);
    const std::string v = r.rc == "0" ? checkV(s, {"x2", "_cons"},
                                               {"+1.3a5283581a073X-00c", "-1.bd89f7b4fd80fX-00a",
                                                "+1.a6b66a72768ccX-007"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eScalar(s, "N") == 54,
            "lane BS T6c: with a missing regressor the replicates resample e(sample) (54 rows) "
            "only: e(V) to 1e-12, e(N) 54 (o1.log T6c). " + v);
  }

  // T7: a string cluster variable sorts by its bytes ("g1" "g10" ... "g9").
  {
    const Res r = run("quietly bootstrap, rep(30) seed(3) cluster(cs): regress y x", s);
    const std::string v = r.rc == "0" ? checkV(s, {"x", "_cons"},
                                               {"+1.819cc010dc67aX-00d", "-1.dbc66b78da107X-00b",
                                                "+1.c7639c3d9a4f3X-008"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty(), "lane BS T7: cluster(cs) on a string variable (o1.log T7). " + v);
  }

  // T8: a missing cluster value in the base is r(459).
  {
    const Res r = run("bootstrap, rep(30) seed(3) cluster(cm): regress y x", s);
    w2check(r.rc == "459" && r.msg == "missing values in cluster variable cm not allowed",
            "lane BS T8: a missing cluster value is r(459) \"missing values in cluster variable cm "
            "not allowed\" (o1.log T8|RC 459). Got rc " + r.rc + " `" + r.msg + "`");
  }

  // T9: `set seed 123` then no seed() == seed(123); the next bootstrap
  // continues the stream, and so does runiform() after it.
  {
    (void)run("set seed 123", s);
    const Res r1 = run("quietly bootstrap, rep(50): regress y x", s);
    const std::string v1 = r1.rc == "0" ? checkV(s, {"x", "_cons"},
                                                 {"+1.2b71fda710434X-00c", "-1.95bea1a4c5ec2X-00a",
                                                  "+1.7d2f2be95e4a7X-007"})
                                        : "rc " + r1.rc;
    const Res r2 = run("quietly bootstrap, rep(20): regress y x", s);
    const std::string v2 = r2.rc == "0" ? checkV(s, {"x", "_cons"},
                                                 {"+1.ad8490babf286X-00d", "-1.72307adae8d79X-00a",
                                                  "+1.90ca4f3f6aa9cX-007"})
                                        : "rc " + r2.rc;
    const double u = s.seed.stream().runiform();
    w2check(v1.empty() && v2.empty() && u == hx("+1.7ca78814a5da4X-003"),
            "lane BS T9: `set seed 123` + bootstrap equals seed(123); a second bootstrap and the "
            "next runiform() continue Stata's stream (o1.log T9, T9b, T9|U). " + v1 + " " + v2 +
                " u " + g17(u));
  }

  // E1 (o4.log): an eclass program that posts an equation-striped e(b) with no
  // esample() -- leebounds' own posting shape (`treat:lower treat:upper`) --
  // keeps the stripe, resamples the whole data and takes e(cmd) from the
  // program. Same draws as T2, so the same e(V).
  {
    try {
      (void)piet::doexec::executeDoSource(
          "capture program drop bseqest\n"
          "program define bseqest, eclass\n"
          "  syntax varlist\n"
          "  quietly regress `varlist'\n"
          "  tempname b V\n"
          "  matrix `b' = e(b)\n"
          "  matrix `V' = e(V)\n"
          "  matrix colnames `b' = treat:lower treat:upper\n"
          "  matrix colnames `V' = treat:lower treat:upper\n"
          "  matrix rownames `V' = treat:lower treat:upper\n"
          "  ereturn post `b' `V', depname(y)\n"
          "  ereturn local cmd \"eqest\"\n"
          "end\n",
          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    const Res r = run("quietly bootstrap, rep(50) seed(123) cluster(c): bseqest y x", s);
    const std::string v = r.rc == "0" ? checkV(s, {"treat:lower", "treat:upper"},
                                               {"+1.0032fe856f75aX-00c", "-1.842afa85ec020X-00a",
                                                "+1.7dd9cae010a72X-007"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eStr(s, "cmd") == "eqest" && eStr(s, "exp1") == "[treat]_b[lower]" &&
                eScalar(s, "N") == 60 && eScalar(s, "N_clust") == 15 &&
                std::isnan(eScalar(s, "chi2")) && eStr(s, "title") == "Bootstrap results",
            "lane BS E1: `bootstrap, cluster(c): <eclass program posting treat:lower "
            "treat:upper>` keeps the equation stripe, e(cmd) the program's, e(exp1) "
            "[treat]_b[lower], e(N) 60, e(N_clust) 15, no model test (o4.log E1). " + v +
                " cmd `" + eStr(s, "cmd") + "` exp1 `" + eStr(s, "exp1") + "`");
  }

  // U1 (o5.log): an exp_list element `_b[treat:lower]` -- the colon inside the
  // brackets is not the prefix colon -- and a parenthesized one, both unnamed:
  // _bs_1 _bs_2, e(exp2) without the parentheses.
  {
    const Res r = run(
        "quietly bootstrap _b[treat:lower] (_b[treat:upper]*2), rep(30) seed(5) cluster(c): "
        "bseqest y x",
        s);
    const std::string v = r.rc == "0" ? checkV(s, {"_bs_1", "_bs_2"},
                                               {"+1.3df82434c8ca7X-00d", "-1.579d3f0d6ce90X-00a",
                                                "+1.4e810386e2900X-006"})
                                      : "rc " + r.rc + " " + r.msg;
    w2check(v.empty() && eStr(s, "exp1") == "_b[treat:lower]" &&
                eStr(s, "exp2") == "_b[treat:upper]*2" && eStr(s, "cmd") == "bootstrap",
            "lane BS U1: `bootstrap _b[treat:lower] (_b[treat:upper]*2), ...: cmd` splits at the "
            "prefix colon, not the one in _b[treat:lower]; e(b) _bs_1 _bs_2, e(V) to 1e-12 "
            "(o5.log U1). " + v);
  }

  // I1 (o6.log): an `in` range restricts the base through e(sample) and is NOT
  // re-applied to the resampled rows (each replicate runs e(command)).
  {
    const Res r = run("quietly bootstrap, rep(20) seed(3) cluster(c): regress y x in 11/60", s);
    const auto V = s.eval.resolveEMatrix("V");
    const bool ok = r.rc == "0" && V.has_value() && V->data.size() == 2 &&
                    close12(V->data[0][0], hx("+1.0484b0a738ac7X-00c")) &&
                    close12(V->data[1][1], hx("+1.0716bd859a563X-007")) &&
                    eStr(s, "command") == "regress y x" && eScalar(s, "N") == 50 &&
                    eScalar(s, "N_reps") == 20;
    w2check(ok, "lane BS I1: `bootstrap, cluster(c): regress y x in 11/60` resamples the 50 "
                "e(sample) rows and runs `regress y x` on each replicate: e(V) to 1e-12, "
                "e(command) `regress y x`, e(N) 50 (o6.log I1). Got rc " + r.rc + " `" + r.msg +
                    "` command `" + eStr(s, "command") + "`");
  }
  // I2 (o6.log): a spaced named expression must be parenthesized.
  {
    const Res r = run("bootstrap d = _b[x] - (_b[_cons]), rep(20) seed(3): regress y x", s);
    w2check(r.rc == "198" && r.msg == "expressions must be bound in parentheses",
            "lane BS I2: `bootstrap d = _b[x] - (_b[_cons]), ...` is r(198) \"expressions must be "
            "bound in parentheses\" (o6.log I2; rc o7.log I2|RC|198). Got rc " + r.rc + " `" + r.msg + "`");
  }

  // A command whose e(sample) this build does not record (sureg here) is a
  // NAMED refusal, never a resample of rows Stata would not draw.
  {
    const Res r = run("bootstrap, rep(20) seed(4): sureg (y x) (c x)", s);
    w2check(r.rc != "0" && r.msg.find("estimation sample") != std::string::npos,
            "lane BS: bootstrap of a command whose e(sample) is not recorded (sureg here) "
            "refuses by name. Got rc " + r.rc + " `" + r.msg + "`");
  }
}
