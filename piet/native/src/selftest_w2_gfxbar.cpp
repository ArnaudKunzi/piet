// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph-family AI-1/AI-4 + coefplot-family AN-1 (2026-10-21): graph bar's
// serset, graph describe's record, and coefplot's generate()/r(), checked
// against the StataBE 19.5 oracle records they reproduce.
// Oracles: tests/eval/golden/aer_w2/w2ai_graph_family.log (gf_bar_mean, log:213-236)
// and tests/eval/golden/aer_w2/w2an_coefplot_family.log (cf_cmd_one, log:354-372).
// Every check fails on main ebcbd8c1, where `graph bar` is r(199), `graph
// describe` is a scatter error and coefplot refuses generate() with r(198).
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

std::vector<std::string> gbDisplays(piet::SessionState& session, const std::string& src) {
  std::vector<std::string> out;
  try {
    auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "display") out.push_back(b.value("text", std::string()));
      if (t == "error") out.push_back("ERR:" + b.value("message", std::string()));
    }
  } catch (const std::exception& e) {
    out.push_back(std::string("EXC:") + e.what());
  }
  return out;
}

std::string line(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines)
    if (l.compare(0, prefix.size(), prefix) == 0) return l;
  return "<no " + prefix + " line>";
}

}  // namespace

void testW2GfxBar() {
  std::printf("[selftest] --- W2 gfxbar: graph bar serset, graph describe, coefplot generate()/r() ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2gfxbar").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto s = std::make_unique<piet::SessionState>(workdir);
  s->cwd = workdir;
  const std::string fixture =
      "clear\n"
      "set obs 64\n"
      "generate byte g = mod(_n-1, 4) + 1\n"
      "generate double y = mod(_n*5, 13)/4 + g\n";
  const auto D = gbDisplays(
      *s, fixture +
              "graph bar (mean) y, over(g) nodraw name(gb1, replace)\n"
              "graph describe gb1\n"
              "display \"D|`r(family)'|`r(scheme)'|`r(ysize)'|`r(xsize)'|`r(command)'|\"\n"
              "preserve\n"
              "serset use, clear\n"
              "display \"S|\" _N \"|`r(varnames)'|\" %21x _values[1] \"|\" %21x g[4] \"|\"\n"
              "restore\n");
  w2check(line(D, "D|") == "D|bar|stcolor|4.5|7.5|bar (mean) y, over(g) nodraw name(gb1, replace)|",
          "W2 gfxbar (1) gf_bar_mean_d/_dc (log:235-236): graph describe's family, scheme, size and "
          "command -- got `" + line(D, "D|") + "`");
  w2check(line(D, "S|") == "S|4|_values _variables g|+1.4a00000000000X+001|+1.0000000000000X+002|",
          "W2 gfxbar (2) gf_bar_mean_ss (log:219-223): 4 rows `_values _variables g`, the g=1 mean "
          "and g coded 1..4 -- got `" + line(D, "S|") + "`");

  const auto C = gbDisplays(
      *s, "clear\n"
          "set obs 40\n"
          "generate x1 = mod(_n*7, 11)\n"
          "generate x2 = mod(_n*3, 5) + 0.5*mod(_n, 2)\n"
          "generate y = 1.5*x1 - 0.8*x2 + mod(_n*5, 7)/3\n"
          "quietly regress y x1 x2\n"
          "estimates store m1\n"
          "coefplot m1, generate(c_) replace nodraw\n"
          "display \"R|\" r(n_plots) \"|\" r(n_ci) \"|\" r(n_subgr) \"|\"\n"
          "local t : type c_at\n"
          "local u : type c_b\n"
          "display \"G|`t'|`u'|\" c_at[3] \"|\" c_df[1] \"|\"\n"
          "capture noisily coefplot m1, generate(c_) nodraw\n"
          "display \"E|\" _rc \"|\"\n");
  w2check(line(C, "R|") == "R|1|1|1|",
          "W2 gfxbar (3) cf_cmd_one (log:354): r(n_plots) 1, r(n_ci) 1, r(n_subgr) 1 -- got `" + line(C, "R|") + "`");
  w2check(line(C, "G|") == "G|byte|float|3|37|",
          "W2 gfxbar (4) cf_cmd_one_gt/_g3 (log:363-371): at compressed to byte, b float, _cons at 3, "
          "df 37 -- got `" + line(C, "G|") + "`");
  w2check(line(C, "E|") == "E|110|",
          "W2 gfxbar (5) cf_rf_genexist (log:2674): generate() onto existing variables without replace "
          "is r(110) -- got `" + line(C, "E|") + "`");
  fs::remove_all(workdir, ec);
}
