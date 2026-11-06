// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 `swindex` (handlers_e3_swindex.cpp): the command runs (rc 0), creates the
// index as a new double variable, returns r(wt)/r(pw) k x 1 and leaves e() alone
// (w2j_e3_swindex.log:189 V01 `0|30|30|double|%10.0g||...`, :1828 X01
// `0|regress|30|4|1|4|1|...`), and its refusals carry Stata's rcs (:1918 R03 111,
// :1931 R04 110, :1943 R05 7). It used to refuse everything with e3_deferred, rc 199.
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

void testW2E3Swindex() {
  std::printf("[selftest] --- W2 E3 swindex: runs, r(wt)/r(pw), Stata's refusal rcs ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_swindex").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto seeded = [&workdir]() {
    auto s = std::make_unique<piet::SessionState>(workdir);
    for (const char* c : {"clear", "set obs 20", "generate double price = 3000 + 17*_n*_n",
                          "generate double mpg = 10 + mod(_n*7,13)", "generate double weight = 2000 + 31*_n",
                          "generate byte foreign = mod(_n,2)"})
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
      rc = rcOf(piet::dispatch("swindex mpg weight, generate(hidx)", *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    const auto* wt = s->eval.findClassMatrix('r', "wt");
    const auto* pw = s->eval.findClassMatrix('r', "pw");
    w2check(rc == "0" && s->datastore() != nullptr && s->datastore()->has("hidx"),
            "E3 swindex: `swindex mpg weight, generate(hidx)` is rc 0 and creates hidx (V01). Got rc " +
                rc);
    w2check(wt != nullptr && pw != nullptr && wt->data.size() == 2 && pw->data.size() == 2,
            "E3 swindex: r(wt) and r(pw) are 2 x 1 (X01)");
    w2check(!s->lastModel().has_value(), "E3 swindex: sets no model state (e() untouched, X01)");
  }
  const char* refusals[][2] = {
      {"swindex mpg weight, generate(hidx) normby(nosuchvar)", "111"},
      {"swindex mpg weight, generate(price)", "110"},
      {"swindex mpg weight", "7"},
      {"swindex mpg weight, generate(hidx) bogus", "198"},
      {"swindex mpg weight, generate(hidx) nor(foreign)", "198"},
  };
  for (const auto& r : refusals) {
    auto s = seeded();
    std::string rc;
    try {
      rc = rcOf(piet::dispatch(r[0], *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    w2check(rc == r[1], std::string("E3 swindex: `") + r[0] + "` is r(" + r[1] + "). Got " + rc);
  }
}
