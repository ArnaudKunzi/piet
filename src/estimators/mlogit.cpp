// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "mlogit.h"

// The multinomial-logit core is header-only (like estimators/tobit.h): every entry
// point is an inline template-free function in mlogit.h, so callers that include
// the header get the whole estimator with no link edge. This translation unit stays
// in cecon_core's source list — named there by the P7 scaffold commit — as the
// header's compile check: a change that breaks mlogit.h fails the library build
// even if no command currently includes it.
namespace cecon {
namespace {
// ODR-safe no-op that forces the header's declarations to be instantiated here.
[[maybe_unused]] const auto mlogit_header_compile_check = &mlogit_fit;
}  // namespace
}  // namespace cecon
