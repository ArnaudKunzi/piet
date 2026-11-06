// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// twoway_opts.h — Stata option -> style-dict translation for twoway layers.
// C++ port of piet/backend/piet/twoway_opts.py. Pure functions, no I/O, no
// cobra dependency. Mirrors the SERIES_COLORS palette in frontend/js/graph.js
// so backend and frontend agree on default colors (kept in sync manually,
// same as the Python oracle's own docstring note).
#ifndef PIET_NATIVE_TWOWAY_OPTS_H
#define PIET_NATIVE_TWOWAY_OPTS_H

#include <array>
#include <string>

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::twoway {

// Mirror of graph.js SERIES_COLORS (same order, same hexes).
inline constexpr std::array<const char*, 8> kSeriesColors = {
    "#1850a8",  // p1 cobalt blue
    "#d42a1c",  // p2 mondrian red
    "#2e7d32",  // p3 forest green
    "#e65100",  // p4 orange
    "#6a1b9a",  // p5 purple
    "#00838f",  // p6 teal
    "#795548",  // p7 brown
    "#546e7a",  // p8 blue-grey
};

// Builds the `style` sub-dict for one layer from its parsed opts (mirrors
// twoway_opts.py's layer_style()). `layerIdx` picks a default palette color
// when no color option is set; `plottype` is the lower-cased layer keyword.
nlohmann::ordered_json layerStyle(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   int layerIdx, const std::string& plottype);

}  // namespace piet::twoway

#endif  // PIET_NATIVE_TWOWAY_OPTS_H
