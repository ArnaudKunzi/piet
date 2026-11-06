// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `lpoly` (goal gfx-lpoly-lowess, 2026-10-15) -- lpoly and lowess as
// StataBE 19.5 answers them in the corpus forms: at() evaluates the smooth at
// the at variable's rows (BUG-936), if/in restrict the sample and leave the
// generated rows outside it missing (BUG-937), se() creates its variable
// (BUG-938), the rule-of-thumb kernel constants are Stata's (BUG-939), [aw=],
// the refusals' rcs, the generated storage type and labels, r() as Stata
// leaves it, and the option net that runs only when the graph is built.
// Oracle: internal capture
// w2ae_lpoly_lowess.log (the case aer_w2:w2ae_lpoly_lowess); every check names
// the PREC arm it rests on and copies its %21x value. Each arm below fails on
// main a609612e (at(): grid written, r(ngrid) 50; if/in: ignored or r(198);
// [aw=]/[pw=]: r(198); se(): no variable; refusals: rc 0 or 1; type float).
// JSON access goes through is_object()/value() inside a try, so a wrong shape
// is a FAIL, never an abort.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gLpolyChecks = 0;
void lpCheck(bool cond, const std::string& label) {
  ++gLpolyChecks;
  w2check(cond, label);
}

struct Run {
  std::string rc = "0";
  bool allObjects = true;  // defect class 1: no block may be an array
  std::vector<nlohmann::ordered_json> blocks;
};

Run run(piet::SessionState& session, const std::string& cmd) {
  Run r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) {
        r.allObjects = false;
        continue;
      }
      r.blocks.push_back(b);
      if (b.value("type", std::string()) == "error") r.rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    r.rc = std::string("EXC:") + e.what();
  }
  return r;
}

std::string shown(piet::SessionState& session, const std::string& exp) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch("display " + exp, session))
      if (b.is_object() && b.value("type", std::string()) == "display") text += b.value("text", std::string());
  } catch (const std::exception&) {
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  while (!text.empty() && text.front() == ' ') text.erase(text.begin());
  return text;
}

// Stata's %21x text -> double (missing -> NaN). "+1.b3a0199bbe118X+002":
// hex mantissa, hex exponent; X+3ff is the missing value.
double fromStataHex(const std::string& s) {
  const std::size_t xpos = s.find('X');
  if (s.size() < 5 || xpos == std::string::npos) return std::nan("");
  const long e = std::strtol(s.substr(xpos + 1).c_str(), nullptr, 16);
  if (e == 0x3ff) return std::nan("");
  const std::string mant = s.substr(1, xpos - 1);  // "1.b3a..."
  const double m = std::strtod(("0x" + mant + "p0").c_str(), nullptr);
  if (m == 0.0) return 0.0;
  return (s[0] == '-' ? -1.0 : 1.0) * std::ldexp(m, static_cast<int>(e));
}

// `local m : type V` / `: variable label V`, then the macro's text.
std::string macroFn(piet::SessionState& session, const std::string& fn, const std::string& var) {
  try {
    (void)piet::dispatch("local __lpm : " + fn + " " + var, session);
  } catch (const std::exception&) {
    return "EXC";
  }
  return shown(session, "\"`__lpm'\"");
}

double hexOf(piet::SessionState& session, const std::string& exp) {
  return fromStataHex(shown(session, "%21x " + exp));
}

bool near(double got, const char* oracleHex) {
  const double want = fromStataHex(oracleHex);
  if (std::isnan(want)) return std::isnan(got);
  if (std::isnan(got)) return false;
  return std::fabs(got - want) <= 1e-9 * std::fabs(want) + (want == 0.0 ? 1e-12 : 0.0);
}

// The case's fixture (w2ae_lpoly_lowess.do), built through the dispatcher.
void mkfix(piet::SessionState& session) {
  for (const char* s : {"clear", "set obs 120", "generate byte grp = mod(_n, 2)",
                        "generate byte g = mod(_n-1, 4) + 1",
                        "generate double x = floor(mod(_n*37, 101)/4)/2 + grp",
                        "generate double z = mod(_n*13, 17)/3",
                        "generate double y = 1 + 0.5*x + 0.3*z + g/2 + mod(_n*7, 11)/5 - grp*x/4",
                        "generate byte w = 1 + mod(_n, 3)", "generate double xat = x in 1/5",
                        "generate double xat2 = .", "replace xat2 = 3 in 1", "replace xat2 = 7.5 in 2",
                        "replace xat2 = 12 in 4", "replace xat2 = 7.5 in 5", "replace xat2 = 100 in 6"})
    (void)piet::dispatch(s, session);
}

}  // namespace

void testW2Lpoly() {
  std::printf("[selftest] --- W2 lpoly: lpoly/lowess at(), if/in, [aw=], se() (w2ae_lpoly_lowess.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2lpoly").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);
  mkfix(session);

  try {
    // ---- lpl_ctl: the ROT bandwidth (BUG-939), r(pwidth) without ci, double + labels.
    Run r = run(session, "lpoly y x, nograph gen(lc_g lc_s)");
    lpCheck(r.rc == "0" && r.allObjects, "lpl_ctl: rc 0, every block an object");
    lpCheck(near(hexOf(session, "r(bwidth)"), "+1.db10f29beda63X-001"),
            "lpl_ctl: r(bwidth) is Stata's ROT (+1.db10f29beda63X-001; the 6-digit constants gave +1.db10f4d416dd7X-001)");
    lpCheck(std::isnan(hexOf(session, "r(pwidth)")), "lpl_ctl: r(pwidth) is missing without ci");
    lpCheck(near(hexOf(session, "lc_s[1]"), "+1.f83e4674c1998X+001"), "lpl_ctl_o1: smooth at the grid's first point");
    lpCheck(macroFn(session, "type", "lc_s") == "double", "lpl_ctl_lc_s: storage type double");
    lpCheck(macroFn(session, "variable label", "lc_g") == "lpoly smoothing grid",
            "lpl_ctl_lc_g_lab: label 'lpoly smoothing grid'");
    lpCheck(macroFn(session, "variable label", "lc_s") == "lpoly smooth: y", "lpl_ctl_lc_s_lab: label 'lpoly smooth: y'");

    // ---- lpl_at (BUG-936): at() evaluates at xat's rows; one generate() name.
    r = run(session, "lpoly y x, generate(la_s) at(xat) nograph");
    lpCheck(r.rc == "0", "lpl_at: rc 0");
    lpCheck(shown(session, "r(ngrid)") == "5", "lpl_at: r(ngrid) 5 (main: 50, the grid)");
    lpCheck(near(hexOf(session, "la_s[1]"), "+1.b3a0199bbe118X+002") &&
                near(hexOf(session, "la_s[3]"), "+1.1c1ac30f8387eX+002"),
            "lpl_at_o1/o3: the smooth at x=5.5 and x=4 (main wrote the grid 0, .265)");
    lpCheck(std::isnan(hexOf(session, "la_s[6]")), "lpl_at_o6: a row without an at value stays missing");
    // ---- lpl_at_miss: missing / repeated / out-of-data at points.
    r = run(session, "lpoly y x, nograph at(xat2) gen(lm_s)");
    lpCheck(shown(session, "r(ngrid)") == "4" && std::isnan(hexOf(session, "lm_s[6]")) &&
                near(hexOf(session, "lm_s[2]"), "+1.c9eadff951169X+002") &&
                near(hexOf(session, "lm_s[5]"), "+1.c9eadff951169X+002"),
            "lpl_at_miss: r(ngrid) 4; at=100 has no data and stays missing; 7.5 twice");
    // ---- lpl_at_self_if: the at points ignore if/in, the sample does not.
    r = run(session, "quietly lpoly y x if x > 2 & x <= 10, bwidth(1) nograph gen(las_s) at(x)");
    lpCheck(shown(session, "r(N)") == "75" && shown(session, "r(ngrid)") == "114",
            "lpl_at_self_if: r(N) 75, r(ngrid) 114");
    // ---- lpl_at_two: with at() the first of two names takes the at values.
    r = run(session, "lpoly y x, nograph at(xat) gen(lat_g lat_s)");
    lpCheck(near(hexOf(session, "lat_g[1]"), "+1.6000000000000X+002") && std::isnan(hexOf(session, "lat_g[6]")),
            "lpl_at_two_o1/o6: the first name holds the at values");

    // ---- refusals with the ado's rcs (defect class 2).
    lpCheck(run(session, "lpoly y x, nograph gen(lg1_s)").rc == "198", "lpl_gen1_noat: one name without at() is r(198)");
    lpCheck(run(session, "lpoly y x, nograph gen(lg3_a lg3_b lg3_c)").rc == "198", "lpl_gen3: three names r(198)");
    lpCheck(run(session, "lpoly y x, nograph gen(lgx_g lc_s)").rc == "110", "lpl_gen_exists: r(110), not rc 1");
    lpCheck(run(session, "lpoly y x, se(lse_se) nograph").rc == "198", "lpl_se_alone: r(198)");
    lpCheck(run(session, "lpoly y x [pw=w], nograph").rc == "101", "lpl_pw: pweights not allowed, r(101)");
    lpCheck(run(session, "lowess y x [aw=w], gen(wa_s) nograph").rc == "101", "lpl_lw_aw: weights not allowed, r(101)");
    lpCheck(run(session, "lowess y x, gen(lc_s) nograph").rc == "110", "lpl_lw_gen_exists: r(110), not rc 1");

    // ---- lpl_dkn: kernel abbreviation (_get_kernel_name) and the gaussian ROT.
    r = run(session, "lpoly y x, degree(1) kernel(gau) n(20) nograph gen(ldk_g ldk_s)");
    lpCheck(r.rc == "0" && shown(session, "r(kernel)") == "gaussian" && shown(session, "r(ngrid)") == "20" &&
                near(hexOf(session, "r(bwidth)"), "+1.dfc037ffb7003X-001"),
            "lpl_dkn: kernel(gau) is gaussian; ROT +1.dfc037ffb7003X-001");

    // ---- lpl_if / lpl_in (BUG-937).
    r = run(session, "lpoly y x if grp == 1, nograph gen(lf_g lf_s)");
    lpCheck(shown(session, "r(N)") == "60" && near(hexOf(session, "r(bwidth)"), "+1.57b19db8240d1X-001") &&
                near(hexOf(session, "lf_g[1]"), "+1.0000000000000X+000") &&
                near(hexOf(session, "lf_s[1]"), "+1.98c7d5c34fe7eX+001"),
            "lpl_if: r(N) 60, the grid starts at the sample's min 1, smooth of the sample");
    r = run(session, "lpoly y x in 1/60, nograph gen(ln_g ln_s)");
    lpCheck(r.rc == "0" && shown(session, "r(N)") == "60" && near(hexOf(session, "ln_s[1]"), "+1.0efde1c14ddc9X+002"),
            "lpl_in: `in` is served (main r(198)); r(N) 60");

    // ---- lpl_aw: [aw=] in the ROT and in the local fit.
    r = run(session, "lpoly y x [aw=w], nograph gen(lw_g lw_s)");
    lpCheck(r.rc == "0" && near(hexOf(session, "r(bwidth)"), "+1.dc86d96f02f92X-001") &&
                near(hexOf(session, "lw_s[1]"), "+1.f728dae9b0061X+001"),
            "lpl_aw: weighted ROT +1.dc86d96f02f92X-001 and smooth (main r(198))");

    // ---- lpl_ci_se / lpl_se_at (BUG-938): se() creates its variable.
    r = run(session, "lpoly y x, ci degree(2) gen(lcs_g lcs_s) se(lcs_se) nodraw");
    lpCheck(r.rc == "0" && near(hexOf(session, "lcs_se[1]"), "+1.0f3bf670f1df5X+000") &&
                near(hexOf(session, "r(pwidth)"), "+1.a831f7f007256X+000"),
            "lpl_ci_se: se() holds Stata's SEs; r(pwidth) under ci");
    lpCheck(macroFn(session, "variable label", "lcs_se") == "lpoly SEs", "lpl_ci_se_lcs_se_lab: 'lpoly SEs'");
    r = run(session, "lpoly y x, at(xat) gen(lsa_s) se(lsa_se) nograph");
    lpCheck(near(hexOf(session, "lsa_se[1]"), "+1.b9867e18e25bdX-003") && std::isnan(hexOf(session, "lsa_se[6]")),
            "lpl_se_at: SEs at the at points");

    // ---- the net runs only when the graph is built (defect class 3).
    lpCheck(run(session, "lpoly y x, nosuchopt(1) nodraw").rc == "198", "lpl_typo_draw: r(198) when drawn (main rc 0)");
    lpCheck(run(session, "lpoly y x, nosuchopt(1) nograph").rc == "0", "lpl_typo_nograph: rc 0 under nograph");
    lpCheck(run(session, "lowess y x, nosuchopt(1) nodraw").rc == "198", "lpl_lw_typo_draw: r(198) when drawn");
    lpCheck(run(session, "lowess y x, nosuchopt(1) nograph").rc == "0", "lpl_lw_typo_nograph: rc 0 under nograph");
    // ---- lpl_044: corpus twoway options accepted, carried or noted, never silent.
    r = run(session,
            "lpoly y x, ci graphregion(color(white)) ylabel(0(2)14, angle(h)) xtitle(\"Milking machines per farm 1959\") "
            "title(\" \") ytitle(\" \") legend(on rows(2) pos(1) ring(0) order(1 \"95pct confidence interval\" 2 "
            "\"Local polynomial smooth\")) noscatter nodraw");
    bool noted = false, graph = false;
    for (const auto& b : r.blocks) {
      if (b.value("type", std::string()) != "graph") continue;
      graph = true;
      const std::string note = b.value("note", std::string());
      noted = note.find("graphregion") != std::string::npos && note.find("legend") != std::string::npos &&
              note.find("ylabel") != std::string::npos && note.find("accepted and not applied") != std::string::npos;
      lpCheck(b.value("x_label", std::string()) == "Milking machines per farm 1959", "lpl_044: xtitle() carried");
    }
    lpCheck(r.rc == "0" && graph && noted && near(hexOf(session, "r(pwidth)"), "+1.644cb5f4f23caX+000"),
            "lpl_044: a graph block whose note names graphregion/legend/ylabel; r(pwidth) under ci");

    // ---- lowess: if/in by row (BUG-937), r(bwidth) not posted, double.
    r = run(session, "lowess y x , gen(wc_s) nograph");
    lpCheck(r.rc == "0" && shown(session, "r(N)") == "120" && std::isnan(hexOf(session, "r(bwidth)")) &&
                near(hexOf(session, "wc_s[1]"), "+1.9a28468c00d2eX+002") &&
                macroFn(session, "type", "wc_s") == "double",
            "lpl_lw_ctl: r(N) 120, r(bwidth) missing (main .8), double");
    r = run(session, "lowess y x if grp == 1, gen(wi_s) nograph");
    lpCheck(shown(session, "r(N)") == "60" && near(hexOf(session, "wi_s[1]"), "+1.685713ffe0f82X+002") &&
                std::isnan(hexOf(session, "wi_s[2]")),
            "lpl_lw_if: r(N) 60, smooth of the sample, rows outside missing (main: 120 rows)");
    r = run(session, "lowess y x in 1/40, gen(wn_s) nograph");
    lpCheck(r.rc == "0" && shown(session, "r(N)") == "40" && std::isnan(hexOf(session, "wn_s[41]")) &&
                near(hexOf(session, "wn_s[1]"), "+1.9dac1ba6019e7X+002"),
            "lpl_lw_in: `in` served by row (main r(198))");
  } catch (const std::exception& e) {
    lpCheck(false, std::string("W2 lpoly: exception ") + e.what());
  }
  std::printf("[selftest] W2 lpoly: %d checks\n", gLpolyChecks);
  fs::remove_all(workdir, ec);
}
