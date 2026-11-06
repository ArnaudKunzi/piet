// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 `acreg` (handlers_e3_acreg.cpp): the heteroskedastic fit runs (rc 0) with
// e(cmd) acreg, no e(vce), 3 columns and no e(df_r) (w2j_e3_acreg.log:264
// S03_robust_noopt `0|acreg|||y|57|.|...|2|54|...|3|x1 x2 _cons|`), and the
// companion-option refusals carry the ado's rcs (log:3205 R01 hac without lag 498,
// :3228 R02 spatial without dist 498, :3339 R07 lag(1.5) 198, :3361 R08 latitude
// out of range 198). It used to refuse everything with e3_deferred, rc 199.
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

void testW2E3Acreg() {
  std::printf("[selftest] --- W2 E3 acreg: runs (HC0), e() layout, the ado's refusal rcs ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_acreg").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto seeded = [&workdir]() {
    auto s = std::make_unique<piet::SessionState>(workdir);
    for (const char* c : {"clear", "set obs 20", "generate double price = 3000 + 17*_n*_n",
                          "generate double mpg = 10 + mod(_n*7,13)", "generate double weight = 2000 + 31*_n",
                          "generate byte foreign = mod(_n,2)", "generate id = ceil(_n/4)",
                          "generate t = mod(_n-1,4)+1", "generate double lat = 40 + id/10",
                          "generate double lon = id/5"})
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
      rc = rcOf(piet::dispatch("acreg price mpg weight", *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    const auto* b = s->eval.findClassMatrix('e', "b");
    const std::string* cmd = s->eval.eStrings.find("cmd");
    w2check(rc == "0" && cmd != nullptr && *cmd == "acreg" && b != nullptr &&
                b->colnames.size() == 3 && b->colnames.back() == "_cons",
            "E3 acreg: `acreg price mpg weight` is rc 0, e(cmd) acreg, e(b) `mpg weight _cons` (S03). "
            "Got rc " + rc);
    w2check(s->eval.eStrings.find("vce") == nullptr && s->eval.eScalars.find("df_r") == nullptr,
            "E3 acreg: no e(vce) and no e(df_r) (IDENTITY.md 1.1; S03 `||` and df_r `.`)");
  }
  const char* refusals[][2] = {
      {"acreg price mpg, id(id) time(t) spatial latitude(lat) longitude(lon) dist(250) hac", "498"},
      {"acreg price mpg, id(id) time(t) spatial latitude(lat) longitude(lon)", "498"},
      {"acreg price mpg, id(id) time(t) spatial latitude(lat) longitude(lon) dist(250) lag(1.5)",
       "198"},
      {"acreg price mpg, latitude(weight)", "198"},
  };
  for (const auto& r : refusals) {
    auto s = seeded();
    std::string rc;
    try {
      rc = rcOf(piet::dispatch(r[0], *s));
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    w2check(rc == r[1], std::string("E3 acreg: `") + r[0] + "` is r(" + r[1] + "). Got " + rc);
  }
}
