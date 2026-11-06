// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 `suest` (handlers_e3_suest.cpp) over stored regress fits: the stacked
// mean/lnvar equations with base columns, e(cmd) suest, e(vce) robust, e(rank) =
// the models' ranks + one lnvar each, and Stata's refusal rcs (w2j_e3_builtin.log:503
// S_plain `240|12|.|.|suest|robust|Robust||A1 A2|mean lnvar|mean lnvar|||b V|14|`,
// :1739 S_r_robust 322, :1773 S_r_noname 111, :1781 S_r_vceboot 198). It used to
// refuse everything with e3_deferred, rc 199.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

void testW2E3Suest() {
  std::printf("[selftest] --- W2 E3 suest: stored regress fits, e() layout, Stata's rcs ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_suest").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto seeded = [&workdir]() {
    auto s = std::make_unique<piet::SessionState>(workdir);
    for (const char* c : {"clear", "set obs 40", "generate id = ceil(_n/4)",
                          "generate g = mod(_n,3)+1", "generate double x1 = mod(_n*7,11)/3",
                          "generate double x2 = mod(_n*13,17)/4",
                          "generate double y = 1 + x1 - x2 + g + mod(_n*5,7)/3",
                          "generate double y2 = 2 - x1 + mod(_n*11,13)/4",
                          "quietly regress y x1 x2 i.g", "estimates store A1",
                          "quietly regress y2 x1 x2", "estimates store A2",
                          "quietly regress y x1 x2, vce(robust)", "estimates store R1",
                          "generate double w1 = 1 + mod(_n,3)", "generate double w2 = 1 + mod(_n,4)",
                          "quietly regress y x1 x2 [aweight=w1]", "estimates store W1",
                          "quietly regress y2 x1 x2 [aweight=w2]", "estimates store W2",
                          "quietly regress y x1 x2 i.g, noconstant", "estimates store N1",
                          "generate double x1d = 2*x1", "quietly regress y x1 x1d x2",
                          "estimates store O1"})
      (void)piet::dispatch(c, *s);
    return s;
  };
  auto rcOf = [](const nlohmann::ordered_json& blocks) {
    std::string rc = "0";
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
    return rc;
  };
  {
    auto s = seeded();
    std::string rc;
    try {
      rc = rcOf(piet::dispatch("suest A1 A2", *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    const auto* b = s->eval.findClassMatrix('e', "b");
    const std::string* cmd = s->eval.eStrings.find("cmd");
    const double* rank = s->eval.eScalars.find("rank");
    // A1: x1 x2 1b.g 2.g 3.g _cons + lnvar (7); A2: x1 x2 _cons + lnvar (4)
    w2check(rc == "0" && cmd != nullptr && *cmd == "suest" && b != nullptr &&
                b->colnames.size() == 11 && b->colnames[2] == "A1_mean:1b.g" &&
                b->colnames[6] == "A1_lnvar:_cons" && rank != nullptr && *rank == 10,
            "E3 suest: `suest A1 A2` is rc 0, e(cmd) suest, 11 columns with the base level, "
            "e(rank) 10 (S_plain's layout). Got rc " + rc);
  }
  {
    // The columns are the stored e(b)'s: a noconstant fit has no _mean:_cons, a
    // collinear fit keeps its `o.` column, and one model takes bare equation names.
    auto s = seeded();
    (void)piet::dispatch("suest N1 A2", *s);
    const auto* b = s->eval.findClassMatrix('e', "b");
    const double* rank = s->eval.eScalars.find("rank");
    w2check(b != nullptr && b->colnames.size() == 10 && b->colnames[4] == "N1_mean:3.g" &&
                b->colnames[5] == "N1_lnvar:_cons" && rank != nullptr && *rank == 9,
            "E3 suest: a noconstant fit has no _mean:_cons column");
    (void)piet::dispatch("suest O1", *s);
    b = s->eval.findClassMatrix('e', "b");
    w2check(b != nullptr && b->colnames.size() == 5 && b->colnames[0] == "mean:o.x1" &&
                b->colnames[4] == "lnvar:_cons",
            "E3 suest: a collinear fit keeps the stored o. column under bare equation names");
  }
  const char* refusals[][2] = {
      {"suest A1 R1", "322"},
      {"suest A1 NOSUCH", "111"},
      {"suest A1 A2, vce(bootstrap)", "198"},
      {"suest W1 W2", "322"},                         // different aweight expressions
      {"suest A1 A2, cluster(id) vce(robust)", "198"},
      {"suest A1 A2, cluster(id) vce(cluster g)", "100"},
      {"suest A1 A2, level(200)", "198"},
      {"suest A1 A2, regressm", "198"},
      {"suest A1 A2, d", "0"},
      {"suest A1 A2, vce()", "0"},
  };
  for (const auto& r : refusals) {
    auto s = seeded();
    std::string rc;
    try {
      rc = rcOf(piet::dispatch(r[0], *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    w2check(rc == r[1], std::string("E3 suest: `") + r[0] + "` is r(" + r[1] + "). Got " + rc);
  }
}
