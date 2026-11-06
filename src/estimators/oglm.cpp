// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "oglm.h"

// The ordered logit/probit core is header-only (like estimators/tobit.h): every
// entry point is an inline template-free function in oglm.h, so callers that
// include the header get the whole estimator with no link edge. This translation
// unit stays in cecon_core's source list — named there by the P7 scaffold commit —
// as the header's compile check: a change that breaks oglm.h fails the library
// build even if no command currently includes it.
namespace cecon {
namespace {
// ODR-safe no-op that forces the header's declarations to be instantiated here.
[[maybe_unused]] const auto oglm_header_compile_check = &oglm_fit;
}  // namespace
}  // namespace cecon
