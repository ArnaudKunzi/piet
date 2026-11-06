// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Internal seam between the evalcase driver core (evalcase.cpp) and the
// module-specific case handlers that land in later M1 commits
// (exprfns/macros). evalcase_ext.cpp owns handleExtendedCase(); it returns a
// null json for kinds it does not know, letting the core report
// "unknown case kind".
#ifndef PIET_NATIVE_EVAL_EVALCASE_INTERNAL_H
#define PIET_NATIVE_EVAL_EVALCASE_INTERNAL_H

#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// Entry point for the --evalcase mode (called from main.cpp).
int runEvalcaseDriver();

// Handles exprfn/eval_arith/macro/... kinds. Returns null json when the kind
// is unknown to the extended handler set.
nlohmann::ordered_json handleExtendedCase(const std::string& kind, const nlohmann::ordered_json& c);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_EVALCASE_INTERNAL_H
