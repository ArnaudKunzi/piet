// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// gettoken — C++ port of piet/backend/piet/router/gettoken.py.
//
// `gettoken TOK REST : SRC [, bind]` peels the first quote-aware token off
// the string value of local macro SRC, storing the token in local TOK and
// the remainder in local REST. Uses the shared scanOneToken() helper so the
// same quote-awareness rules apply here and in program arg-splitting (M2+).
#ifndef PIET_NATIVE_EVAL_GETTOKEN_H
#define PIET_NATIVE_EVAL_GETTOKEN_H

#include <functional>
#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// Minimal locals-store abstraction so gettoken can be unit-tested/diffed
// without the full M2 SessionState. get() mirrors Python's
// `session._locals.get(name, "")`; set() mirrors `session._locals[name] = v`.
struct LocalsStore {
  std::function<std::string(const std::string&)> get;
  std::function<void(const std::string&, const std::string&)> set;
  // gettoken's `(global) name` ([P] gettoken); empty where the caller keeps no
  // globals, and a (global) name then refuses.
  std::function<std::string(const std::string&)> getGlobal;
  std::function<void(const std::string&, const std::string&)> setGlobal;
};

// Executes `gettoken TOK REST : SRC [, bind]`.
//
// Returns an empty vector on success (Stata is silent; bindings land in
// `locals`), or a one-element vector containing the CommandErrorResult block
// on a syntax error. Matches run_gettoken()'s `list[dict]` return shape.
std::vector<nlohmann::json> runGettoken(const std::string& rawLine, LocalsStore& locals);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_GETTOKEN_H
