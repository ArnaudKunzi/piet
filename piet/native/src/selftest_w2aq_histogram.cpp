// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `w2aq_histogram` (goal gfx-histogram-kdensity-family, 2026-10-21) --
// histogram/hist, twoway__histogram_gen and kdensity as StataBE 19.5 runs them:
// twoway__histogram_gen's bins, r() and gen() variables (one binning rule), the
// histogram note, discrete's float gap, by() panels, fweights, the refusals with
// Stata's rc, and kdensity's default grid, kernels, macro-text bandwidth, at(),
// iweights, generate() column order and r().
// Oracle: the w2aq_histogram_family capture (internal capture
// w2aq_histogram_family.log, log sha256 ef09ce85...; to land
// as tests/eval/golden/aer_w2/w2aq_histogram_family.log); each check cites its log
// line. Every check fails on main 6192e751, where twoway__histogram_gen is refused
// as a Stata internal (r(199)), histogram prints no note and refuses discrete,
// by(), start(), w() and weights, its bins follow a different rule, and kdensity
// posts no r(), draws 512 points, refuses nograph, at() and six kernels.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

// w2aq_histogram_family.do's closed-form fixture (60 rows; the variables these
// checks read).
const char* kFixture =
    "clear\n"
    "quietly set obs 60\n"
    "generate byte g = mod(_n - 1, 3) + 1\n"
    "generate byte np = mod(_n, 2)\n"
    "generate str2 s = cond(mod(_n, 4) == 0, \"c\", cond(mod(_n, 4) == 1, \"ab\", cond(mod(_n, 4) == 2, \"d\", \"bc\")))\n"
    "generate double x = mod(_n*7, 23)/2 - 3\n"
    "generate double y = mod(_n*11, 17) + mod(_n, 5)/8\n"
    "label variable y \"Outcome y\"\n"
    "format y %9.2f\n"
    "generate byte w = 1 + mod(_n, 3)\n"
    "generate byte k = mod(_n*3, 7)\n"
    "generate double xn = k + mod(_n, 2)/3\n"
    "generate double ag = cond(_n <= 9, (_n - 1)*1.5 - 1, .)\n";

struct Run {
  std::vector<std::string> displays;
  std::vector<std::string> errors;  // the message of every error block
  std::vector<nlohmann::ordered_json> graphs;
  bool arrayBlock = false;
};

Run run(piet::SessionState& session, const std::string& src) {
  Run r;
  try {
    auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object()) {
        r.arrayBlock = true;  // defect class 1: never an array pushed as a block
        continue;
      }
      const std::string t = b.value("type", std::string());
      if (t == "display") r.displays.push_back(b.value("text", std::string()));
      if (t == "error") r.errors.push_back(b.value("message", std::string()));
      if (t == "graph") r.graphs.push_back(b);
    }
  } catch (const std::exception& e) {
    r.displays.push_back(std::string("EXC:") + e.what());
  }
  return r;
}

std::string lineStarting(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines)
    if (l.compare(0, prefix.size(), prefix) == 0) return l;
  return "<no " + prefix + " line>";
}

bool hasLine(const std::vector<std::string>& lines, const std::string& text) {
  for (const auto& l : lines)
    if (l == text) return true;
  return false;
}

// Stata's %21x text -> double (missing -> NaN).
double fromStataHex(const std::string& s) {
  const std::size_t xpos = s.find('X');
  if (s.size() < 5 || xpos == std::string::npos) return std::nan("");
  const long e = std::strtol(s.substr(xpos + 1).c_str(), nullptr, 16);
  if (e == 0x3ff) return std::nan("");
  const std::string mant = s.substr(1, xpos - 1);
  const double m = std::strtod(("0x" + mant + "p0").c_str(), nullptr);
  if (m == 0.0) return 0.0;
  return (s[0] == '-' ? -1.0 : 1.0) * std::ldexp(m, static_cast<int>(e));
}

bool near(double got, double want) {
  if (std::isnan(want) || std::isnan(got)) return std::isnan(want) && std::isnan(got);
  return std::fabs(got - want) <= 1e-9 * std::fabs(want);
}

// Every %21x field of `line` (TAG|a|b|...) within 1e-9 of the oracle's.
bool hexFieldsNear(const std::string& line, const std::vector<const char*>& oracle) {
  std::vector<std::string> f;
  std::stringstream ss(line);
  for (std::string p; std::getline(ss, p, '|');) f.push_back(p);
  if (f.size() < oracle.size() + 1) return false;
  for (std::size_t i = 0; i < oracle.size(); ++i) {
    if (!near(fromStataHex(f[i + 1]), fromStataHex(oracle[i]))) return false;
  }
  return true;
}

std::vector<double> column(piet::SessionState& session, const std::string& v) {
  std::vector<double> out;
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(v)) return out;
  const auto& c = ds->col(v);
  const double* d = c.f64();
  for (int64_t i = 0; i < ds->nRows(); ++i) out.push_back(d[i]);
  return out;
}

int position(piet::SessionState& session, const std::string& v) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr) return -1;
  const auto card = ds->formatCard(std::nullopt);
  for (std::size_t i = 0; i < card.size(); ++i)
    if (card[i].name == v) return static_cast<int>(i) + 1;
  return -1;
}

}  // namespace

void testW2aqHistogram() {
  std::printf(
      "[selftest] --- W2 w2aq_histogram: histogram/kdensity/twoway__histogram_gen "
      "(w2aq_histogram_family.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2aq").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const char* kRtw =
      "display \"TW|\" %21x r(bin) \"|\" %21x r(start) \"|\" %21x r(width) \"|\" %21x r(min) \"|\" "
      "%21x r(max) \"|\" %21x r(N) \"|\" %21x r(area) \"|\" \"`r(type)'\" \"|\"\n";

  // (1) twoway__histogram_gen ..., return: r() (log:2857 hk_thg_ret_r).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) + "twoway__histogram_gen x, bin(5) return\n" + kRtw);
    const std::string tw = lineStarting(r.displays, "TW|");
    w2check(hexFieldsNear(tw, {"+1.4000000000000X+002", "-1.8000000000000X+001", "+1.199999999999aX+001",
                               "-1.8000000000000X+001", "+1.0000000000000X+003", "+1.e000000000000X+005",
                               "+1.0000000000000X+000"}) &&
                tw.find("|density|") != std::string::npos,
            "W2AQ (1) twoway__histogram_gen x, bin(5) return posts r(bin start width min max N area "
            "type) as log:2857 hk_thg_ret_r (main: r(199) refused as a Stata internal) -- got " + tw);
  }

  // (2) gen(h x): the occupied bins, the closing (start, .) row, the centre
  // before the height, float, labels and r(n_x) (log:2669-2678 hk_thg_start).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "twoway__histogram_gen x, start(-4) width(2.5) frequency gen(hk_h hk_x)\n"
                             "display \"NX|\" %21x r(n_x) \"|\"\n");
    const std::vector<double> h = column(session, "hk_h"), xc = column(session, "hk_x");
    const std::vector<const char*> wantH = {"+1.c000000000000X+002", "+1.c000000000000X+003",
                                            "+1.a000000000000X+003", "+1.a000000000000X+003",
                                            "+1.a000000000000X+003", "+1.0000000000000X+3ff"};
    const std::vector<const char*> wantX = {"-1.6000000000000X+001", "-1.0000000000000X-002",
                                            "+1.2000000000000X+001", "+1.3000000000000X+002",
                                            "+1.d000000000000X+002", "-1.0000000000000X+002"};
    bool ok = h.size() == 60 && xc.size() == 60;
    for (std::size_t i = 0; ok && i < wantH.size(); ++i)
      ok = near(h[i], fromStataHex(wantH[i])) && near(xc[i], fromStataHex(wantX[i]));
    for (std::size_t i = wantH.size(); ok && i < 60; ++i) ok = std::isnan(h[i]) && std::isnan(xc[i]);
    w2check(ok && position(session, "hk_x") < position(session, "hk_h") &&
                session.varMeta["hk_h"].stataType.value_or("") == "float" &&
                session.varMeta["hk_h"].label.value_or("") == "Frequency" &&
                session.varMeta["hk_x"].label.value_or("") == "x" &&
                hexFieldsNear(lineStarting(r.displays, "NX|"), {"+1.8000000000000X+002"}),
            "W2AQ (2) twoway__histogram_gen ..., start(-4) width(2.5) frequency gen(h x): five "
            "occupied bins, the closing (x = start, h = .) row, x created before h, float, "
            "labels Frequency/x, r(n_x) 6 (log:2669-2678; main: r(199))");
  }

  // (3) discrete: the smallest gap is a FLOAT gap (log:1731 hk_h_discnonint_tw
  // width .33333325 = float(7/3) - 2).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) + "twoway__histogram_gen xn, discrete display\n" + kRtw);
    const std::string tw = lineStarting(r.displays, "TW|");
    w2check(hexFieldsNear(tw, {"+1.4000000000000X+004", "+0.0000000000000X-3ff", "+1.5555500000000X-002",
                               "-1.5555500000000X-003", "+1.9ffff98000000X+002"}) &&
                hasLine(r.displays, "(start=0, width=.33333325)"),
            "W2AQ (3) discrete on non-integers: width is the float gap .33333325 and the note "
            "prints (start=0, width=.33333325) (log:1699, :1731) -- got " + tw);
  }

  // (4) the empty sample: r() and gen() (log:2774-2777 hk_thg_empty).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) + "twoway__histogram_gen x if x > 100, gen(hk_h hk_x)\n" + kRtw);
    const std::string tw = lineStarting(r.displays, "TW|");
    w2check(hexFieldsNear(tw, {"+1.0000000000000X+000", "+0.0000000000000X-3ff", "+0.0000000000000X-3ff",
                               "+0.0000000000000X-3ff", "+0.0000000000000X-3ff", "+1.0000000000000X+000",
                               "+0.0000000000000X-3ff"}) &&
                position(session, "hk_h") < position(session, "hk_x") &&
                session.varMeta["hk_h"].label.value_or("") == "Density" &&
                session.varMeta["hk_x"].label.value_or("?") == "",
            "W2AQ (4) empty sample: r() 1/0/0/0/0/1/0, h created before x, x unlabelled "
            "(log:2774-2777) -- got " + tw);
  }

  // (5) syntax only: the rclass program empties r() (log:2894).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "quietly summarize x\n"
                             "twoway__histogram_gen x, bin(5)\n"
                             "display \"SO|\" %21x r(N) \"|\" %21x r(mean) \"|\"\n");
    w2check(lineStarting(r.displays, "SO|") == "SO|+1.0000000000000X+3ff|+1.0000000000000X+3ff|",
            "W2AQ (5) twoway__histogram_gen without gen()/return/display leaves r() empty "
            "(log:2894 hk_thg_syntaxonly_r) -- got " + lineStarting(r.displays, "SO|"));
  }

  // (6) histogram prints twoway__histogram_gen's note (log:334) and its bars
  // are the occupied bins at the twin's start and width.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) + "histogram x, nodraw\n");
    const auto bars = r.graphs.empty() ? nlohmann::ordered_json::array()
                                       : r.graphs[0].value("bins", nlohmann::ordered_json::array());
    w2check(hasLine(r.displays, "(bin=7, start=-3, width=1.5714286)") && bars.size() == 7 &&
                r.graphs[0].value("bin", 0) == 7,
            "W2AQ (6) `histogram x` prints (bin=7, start=-3, width=1.5714286) (log:334) and draws "
            "the 7 occupied bins (main: no note, llround bin count)");
  }

  // (7) by() with fweights: one panel per group at the common start and width,
  // heights within the panel (log:1986-1994 hk_h_byfw serset 1).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) + "histogram x [fw=w], by(g) freq width(2) nodraw\n");
    bool ok = r.errors.empty() && !r.graphs.empty();
    nlohmann::ordered_json panels = ok ? r.graphs[0].value("panels", nlohmann::ordered_json::array())
                                       : nlohmann::ordered_json::array();
    ok = ok && panels.size() == 3;
    const std::vector<const char*> wantY = {"+1.0000000000000X+003", "+1.0000000000000X+003",
                                            "+1.0000000000000X+002", "+1.8000000000000X+002",
                                            "+1.0000000000000X+003", "+1.8000000000000X+002"};
    const std::vector<const char*> wantX = {"-1.0000000000000X+001", "+0.0000000000000X-3ff",
                                            "+1.0000000000000X+001", "+1.0000000000000X+002",
                                            "+1.8000000000000X+002", "+1.0000000000000X+003"};
    if (ok) {
      const auto bars = panels[0].value("bins", nlohmann::ordered_json::array());
      ok = bars.size() == wantY.size();
      for (std::size_t i = 0; ok && i < wantY.size(); ++i)
        ok = near(bars[i].value("y", 0.0), fromStataHex(wantY[i])) &&
             near(bars[i].value("x", 0.0), fromStataHex(wantX[i]));
    }
    w2check(ok, "W2AQ (7) `histogram x [fw=w], by(g) freq width(2)`: three panels, panel 1's "
                "weighted heights and centres as the oracle's serset (log:1986-1994; main: r(198))");
  }

  // (8) the refusals, each with Stata's rc (log: the hk_r_h_* and hk_r_thg_* arms).
  {
    struct R {
      const char* cmd;
      const char* rc;
    };
    const std::vector<R> kRef = {
        {"histogram s, nodraw", "109"},          {"histogram x y, nodraw", "103"},
        {"histogram x, bin(5) width(1) nodraw", "198"}, {"histogram x, freq percent nodraw", "198"},
        {"histogram x if x > 100, nodraw", "2000"}, {"histogram x [pw=w], nodraw", "101"},
        {"histogram x, bin(0) nodraw", "125"},     {"histogram x, by(, title(\"a\")) nodraw", "100"},
        {"histogram x, by(g) freq kdensity nodraw", "191"},
        {"twoway__histogram_gen x, gen(hk_only)", "198"}, {"twoway__histogram_gen x, gen(y x)", "110"},
        {"twoway__histogram_gen x, start(0) gen(hk_h hk_x)", "198"},
        {"kdensity x [pw=w], nograph generate(hk_a1 hk_a2)", "101"},
        {"kdensity x, nograph generate(hk_a1 hk_a2) kernel(foo)", "198"},
        {"kdensity x, nograph generate(x y)", "110"},
        {"kdensity x, nograph generate(hk_a1 hk_a2) title(\"a\")", "198"},
        {"kdensity s, nograph generate(hk_a1 hk_a2)", "109"},
    };
    std::string bad;
    for (const auto& t : kRef) {
      piet::SessionState session(workdir);
      session.eval.cwd = workdir;
      Run r = run(session, std::string(kFixture) + "capture noisily " + t.cmd + "\ndisplay \"RC|\" _rc\n");
      if (lineStarting(r.displays, "RC|") != std::string("RC|") + t.rc || r.arrayBlock)
        bad += std::string(" [") + t.cmd + " -> " + lineStarting(r.displays, "RC|") + "]";
    }
    w2check(bad.empty(), "W2AQ (8) refusals carry Stata's rc: 109 string, 103 two variables, 198 "
                         "bins+width / two types / start / gen(1 name) / bad kernel / nograph+title, "
                         "2000 empty, 101 pweight, 125 bin(0), 100 by() without a varlist, 191 "
                         "freq+kdensity+by, 110 existing gen() names (hk_r_h_*, hk_r_thg_*, "
                         "hk_r_kd_*; main answers 0, 1, 198 or 199 for most)" + bad);
  }

  // (9) kdensity: the n(50) default grid, generate(x d) with d the earlier
  // column, labels, and r() (log:5238-5242 hk_kd_default).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "kdensity x, nograph generate(hk_kx hk_kd)\n"
                             "display \"KD|\" %21x r(bwidth) \"|\" %21x r(n) \"|\" %21x r(scale) \"|\" "
                             "\"`r(kernel)'\" \"|\"\n");
    const std::vector<double> kx = column(session, "hk_kx"), kd = column(session, "hk_kd");
    const std::string kdl = lineStarting(r.displays, "KD|");
    w2check(r.graphs.empty() &&
                hexFieldsNear(kdl, {"+1.5294cb197af58X+000", "+1.9000000000000X+005", "+1.2394288ee8c60X-002"}) &&
                kdl.find("|epanechnikov|") != std::string::npos && kx.size() == 60 &&
                near(kx[0], fromStataHex("-1.14a532c65ebd6X+002")) &&
                near(kd[0], fromStataHex("+1.373b4fff14fa2X-006")) && std::isnan(kx[50]) &&
                position(session, "hk_kd") < position(session, "hk_kx") &&
                session.varMeta["hk_kd"].label.value_or("") == "density: x",
            "W2AQ (9) `kdensity x, nograph generate(kx kd)`: n(50) grid, r(bwidth n scale kernel), "
            "kd created before kx, label `density: x` (log:5238-5242; main: r(198) nograph) -- got " +
                kdl);
  }

  // (10) the rectangle kernel: the bandwidth is kept as macro text, which
  // decides which grid points cross |z| = 1 (log:4993 hk_kd_k_rectangle_r).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "kdensity x, nograph generate(hk_kx hk_kd) n(9) kernel(rectangle)\n"
                             "display \"KR|\" %21x r(bwidth) \"|\" %21x r(n) \"|\" %21x r(scale) \"|\"\n");
    const std::string kr = lineStarting(r.displays, "KR|");
    w2check(kr.compare(0, 25, "KR|+1.5294cb197af58X+000|") == 0 &&
                hexFieldsNear(kr, {"+1.5294cb197af58X+000", "+1.2000000000000X+003", "+1.be7b11732ecd7X+000"}),
            "W2AQ (10) kernel(rectangle): r(bwidth) bit for bit and r(scale) as the oracle (log:4993; "
            "a plain double bandwidth is an ulp off, moves grid points across |z| = 1 and r(scale) "
            "to +1.a739...) -- got " + kr);
  }

  // (11) n() > _N is capped with a note (log hk_kd_026: `(n() set to 60)`, r(n) 60).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "kdensity y, generate(hk_dx hk_dy) n(1000) nodraw\n"
                             "display \"KN|\" %21x r(bwidth) \"|\" %21x r(n) \"|\" %21x r(scale) \"|\"\n");
    w2check(hasLine(r.displays, "(n() set to 60)") &&
                hexFieldsNear(lineStarting(r.displays, "KN|"),
                              {"+1.f44bc962fd839X+000", "+1.e000000000000X+005", "+1.676700ecbb197X-002"}),
            "W2AQ (11) kdensity n(1000) on 60 rows prints (n() set to 60) and posts r(n) 60 "
            "(log:3439; main: the full 1000-point grid cut to 60 rows) -- got " +
                lineStarting(r.displays, "KN|"));
  }

  // (12) at() with a gaussian kernel and bw(5) (log:4323-4335 hk_kd_107at).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "kdensity y, kernel(gaussian) gen(hk_val hk_den) at(ag) nograph bw(5)\n"
                             "display \"KA|\" %21x r(bwidth) \"|\" %21x r(n) \"|\" %21x r(scale) \"|\"\n");
    const std::vector<double> den = column(session, "hk_den"), val = column(session, "hk_val");
    w2check(hexFieldsNear(lineStarting(r.displays, "KA|"),
                          {"+1.4000000000000X+002", "+1.2000000000000X+003", "+1.4149508fd884dX+001"}) &&
                den.size() == 60 && near(den[0], fromStataHex("+1.942746294f2caX-006")) &&
                near(den[8], fromStataHex("+1.a56cd4a101136X-005")) && std::isnan(den[9]) &&
                near(val[8], fromStataHex("+1.6000000000000X+003")),
            "W2AQ (12) kdensity at(ag): density at the 9 at() rows, r(n) 9 (log:4323-4335; main: "
            "r(198) at() refused) -- got " + lineStarting(r.displays, "KA|"));
  }

  // (13) iweights divide by h only (log:5460, :5466 hk_kd_iw).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, std::string(kFixture) +
                             "kdensity x [iw=w], nograph generate(hk_kx hk_kd) n(9)\n"
                             "display \"KI|\" %21x r(scale) \"|\" %21x hk_kd[3] \"|\"\n");
    w2check(hexFieldsNear(lineStarting(r.displays, "KI|"), {"+1.d2609acbd7d38X-007", "+1.5b84d61f006a5X+003"}),
            "W2AQ (13) kdensity [iw=w]: density = sum w K / h, r(scale) as the oracle (log:5460, "
            ":5466; main: r(198) weights refused) -- got " + lineStarting(r.displays, "KI|"));
  }

  // (14) no handler pushes an array as one block, and saving() writes its file.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string target = (fs::path(workdir) / "w2aq_hs2.eps").u8string();
    fs::remove(target, ec);
    Run r = run(session, std::string(kFixture) +
                             "histogram k, discrete percent saving(\"" + target + "\") nodraw\n"
                             "twoway__histogram_gen x, width(1.5) display gen(hk_h hk_x)\n");
    w2check(!r.arrayBlock && r.errors.empty() && fs::exists(target),
            "W2AQ (14) histogram saving(\"file.eps\") writes that file (log hk_h_031disc_f rc 0) and "
            "every block is an object");
  }

  // BUG-1006/1007: the readers of the oracle's hk_gv helper (every hk_*_g record),
  // each in its own run so one gap cannot mask the next. Record
  // log:2501 `PREC|hk_thg_024_g|0|60|16|hk_h2 hk_x2|16 15|...|8 8|8|1|`: gen(h x)
  // appends x then h after the fixture's variables, so h is c(k) and x c(k)-1.
  // The oracle's fixture has 14 variables (16 15); this one has 9 (11 10).
  const std::string kThg024 = std::string(kFixture) +
                              "twoway__histogram_gen y if g == 2, frac width(2) gen(hk_h2 hk_x2)\n";

  // (15) `: list posof` (BUG-1006; main: `list not allowed`, r(101)).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, kThg024 +
                             "unab all : _all\n"
                             "local p : list posof \"hk_h2\" in all\n"
                             "local q : list posof `\"hk_x2\"' in all\n"
                             "local z : list posof \"zz\" in all\n"
                             "display \"PO|`p'|`q'|`z'|\"\n");
    w2check(hasLine(r.displays, "PO|11|10|0|"),
            "W2AQ (15) `: list posof \"v\" in all` is the 1-based word position, 0 when absent "
            "(log:2501 hk_thg_024_g positions 16 15 on 14 fixture variables, here 11 10 on 9; main: "
                "r(101) list not allowed) -- got " +
                lineStarting(r.displays, "PO|"));
  }

  // (16) `confirm variable v, exact` (BUG-1006; main: r(111), the option list
  // read as names). Any other option is refused by name, r(198).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, kThg024 +
                             "capture confirm variable hk_h2, exact\n"
                             "local a = _rc\n"
                             "capture confirm variable hk_h, exact\n"
                             "local b = _rc\n"
                             "capture confirm variable hk_h2, nosuch\n"
                             "display \"CF|`a'|`b'|\" _rc \"|\"\n");
    w2check(hasLine(r.displays, "CF|0|111|198|"),
            "W2AQ (16) confirm variable v, exact: rc 0 for v, 111 for a prefix of it, 198 for an "
            "unknown option (log:2501 hk_thg_024_g is rc 0; main: 111 for v) -- got " +
                lineStarting(r.displays, "CF|"));
  }

  // (17) `` `c(name)' `` (BUG-1006; main: unsupported-macro refusal). A name outside
  // resolveCValue's spine that Stata has keeps that refusal; BUG-1420: one that is no
  // c() key at all expands to "" at rc 0, as in StataBE 19.5 (lane expr-sem x1b/a14.do
  // Q9). The arm held the refusal for `c(nosuchkey)' too until the key list existed.
  // A key `creturn list` does not print is still a key: StataBE 19.5 expands
  // `c(matsize)' to 400 and `c(framesmax)' to 100, so they are never "": c(matsize)
  // is served (400, BUG-1330), c(framesmax) refused by name.
  // BUG-1330: c(linesize) is a real name this build does not serve (refused by
  // name), and PIET's own c(sysdir_sample) keeps answering (the sysdir_* arm had
  // made it unreachable).
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, kThg024 + "display \"CK|`c(k)'|`c(N)'|\"\n");
    Run u = run(session, "display \"CU|`c(nosuchkey)'|\"\n");
    Run v = run(session, "display \"CV|`c(docx_paramode)'|\"\n");
    Run w = run(session, "display \"CW|`c(matsize)'|\"\n");
    Run f = run(session, "display \"CF|`c(framesmax)'|\"\n");
    Run g = run(session, "display \"CG|`c(linesize)'|\"\n");
    Run sd = run(session, "display \"CS|`c(sysdir_sample)'|\" c(sysdir_sample)\n");
    const std::string cs = lineStarting(sd.displays, "CS|");
    const size_t bar = cs.find('|', 3);  // CS|<delimited>|<bare>: the same directory twice
    const bool sampleDir = cs.rfind("CS|", 0) == 0 && bar != std::string::npos && bar > 9 &&
                           cs.compare(bar - 6, 6, "sample") == 0 &&
                           cs.substr(bar + 1) == cs.substr(3, bar - 3);
    w2check(hasLine(r.displays, "CK|11|60|") && r.errors.empty() && hasLine(u.displays, "CU||") &&
                u.errors.empty() && v.displays.empty() && v.errors.size() == 1 &&
                v.errors[0].find("c(docx_paramode)") != std::string::npos &&
                hasLine(w.displays, "CW|400|") && w.errors.empty() &&
                f.displays.empty() && f.errors.size() == 1 &&
                f.errors[0].find("c(framesmax)") != std::string::npos && g.displays.empty() &&
                g.errors.size() == 1 && g.errors[0].find("c(linesize)") != std::string::npos &&
                sd.errors.empty() && sampleDir,
            "W2AQ (17) `c(k)' and `c(N)' expand to 11 and 60 (log:2501 hk_thg_024_g c(k) 16 on 14 "
            "fixture variables, here 9; _N 60; main: unsupported macro), a name that is no c() "
            "key expands to \"\", a Stata key PIET does not serve (c(docx_paramode), "
            "c(linesize), the hidden c(framesmax)) is refused by name, the hidden c(matsize) "
            "is Stata's 400 (BUG-1330 serves it), and "
            "c(sysdir_sample) is the sample directory -- got " +
                lineStarting(r.displays, "CK|") + " " + lineStarting(u.displays, "CU|") + " " +
                lineStarting(w.displays, "CW|") + " " + lineStarting(f.displays, "CF|") + " " + cs);
  }

  // (18) numeric `r(name)' / `e(name)' are macro text by Stata's %18.0g rule
  // (BUG-1007; main: Python repr, `8.0`). count's r(N): log:2501 hk_thg_024_g
  // nonmissing `8 8`. The program's r(): w2af_macro_text_file.log:448 `.25`,
  // :484 `.3333333333333333`, :496 `1.00000000000e+20`, :502 `.00001`.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    Run r = run(session, kThg024 +
                             "quietly count if !missing(hk_h2)\n"
                             "display \"RN|`r(N)'|\"\n"
                             "capture program drop w2aq_rnum\n"
                             "program define w2aq_rnum, rclass\n"
                             "    return scalar q = .25\n"
                             "    return scalar third = 1/3\n"
                             "    return scalar e20 = 1e20\n"
                             "    return scalar tiny = 1e-5\n"
                             "end\n"
                             "w2aq_rnum\n"
                             "display \"RQ|`r(q)'|`r(third)'|`r(e20)'|`r(tiny)'|\"\n"
                             "quietly regress y x\n"
                             "display \"EN|`e(N)'|`e(df_m)'|\"\n");
    w2check(hasLine(r.displays, "RN|8|") &&
                hasLine(r.displays, "RQ|.25|.3333333333333333|1.00000000000e+20|.00001|") &&
                hasLine(r.displays, "EN|60|1|"),
            "W2AQ (18) `r(N)' after count is 8 (log:2501), `r(q)' `r(third)' `r(e20)' `r(tiny)' "
            "follow %18.0g (w2af log:448-502), `e(N)' is 60 (main: 8.0, 0.25, 60.0) -- got " +
                lineStarting(r.displays, "RN|") + " " + lineStarting(r.displays, "RQ|") + " " +
                lineStarting(r.displays, "EN|"));
  }

  fs::remove_all(workdir, ec);
}
