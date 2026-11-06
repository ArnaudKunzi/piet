// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — native reader for Stata .dta formats 114 / 115 / 117 / 118 (no Python/pandas
// dependency). Hand-rolled per the public Stata .dta technical spec; byte layout
// cross-checked against pandas.io.stata (read-only reference, NOT a runtime dep).
//
// Oracle parity target: PIET's Python backend loads .dta via
//   pandas.read_stata(path, convert_categoricals=False)
// (see piet/backend/piet/api/routes_data.py:_convert_to_hbin, and the identical
// inline copies in merge_op.py / append.py / joinby_op.py). That means:
//   - Any NUMERIC storage type (byte/int/long/float/double), value-labeled or
//     not, stays F64 with its RAW numeric code — value labels are NOT baked into
//     the column; they ride along as separate display-only metadata.
//   - STRING storage types (str1..str2045, strL) become a Frame factor column
//     (dictionary-encoded, first-seen order — same convention as Frame::from_csv
///    and pandas.factorize).
//   - %d/%td-family date formats are NOT auto-converted to datetime by
//     convert_categoricals=False (pandas only converts %tc/%td/etc. via its own
//     separate `convert_dates` machinery, which IS enabled by default in
//     pd.read_stata). Stata date/time storage is already a numeric (long/double)
//     column counting days/ms/etc. since 1960-01-01; PIET's oracle converts %td
///    (and the other date formats) to epoch-DAY f64 (see tools/to_hbin.py
//     STATA_EPOCH handling) — this reader reproduces that: any variable whose
//     Stata display format starts with "%t" (a date/time format) is rescaled
//     from its native Stata date unit to epoch-days f64, matching
///    tools/to_hbin.py's datetime64->epoch-days path bit-for-bit on round numbers.
//   - The display format itself is CARRIED, not just consumed: every variable's
//     `fmtlist` entry is attached to its Frame column (`FrameColumn::format`,
//     P1-A0-FORMAT-CARRIAGE 2026-07-30) as well as reported in `var_meta`, so it
//     survives a write into the .hbin header's optional per-column "format" key
//     and is readable again as `cecon::Column::format`. Before this the string was
//     read, used for the %t* rescale above, and then dropped — so a `%td` that came
//     from the file could never reach a display layer (P1-W1 re-review §1).
//     Carriage is metadata-only: no value changes, and the rescale above remains
//     the ONLY place the engine interprets a format.
//   - Values outside the type's VALID_RANGE (Stata's system-missing sentinel
//     region, "." and above) become NaN carrying the Stata missing KIND —
//     plain "." or one of the extended codes .a-.z (see data/missing.h for
//     the representation and data/frame.cpp for the comparison semantics the
//     kind drives: `y == .a` is true only for `.a` cells, `. < .a < ... < .z`,
//     and arithmetic collapses any of them back to "."). Codes are decoded
//     per storage type — consecutive integers above the valid maximum, and a
//     fixed bit stride above the float/double "." sentinel.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "data/frame.h"

namespace cecon {

// One Stata variable's descriptive metadata (parallel to PIET's
// session._var_meta[v] = {label, format, stata_type} + session._value_labels).
struct DtaVarMeta {
    std::string name;
    std::string label;        // variable label ("" if none)
    std::string format;       // Stata display format, e.g. "%9.0g", "%td"
    std::string stata_type;   // "byte"|"int"|"long"|"float"|"double"|"strNN"|"strL"
    std::string value_label_name;  // attached value-label set name ("" if none)
};

struct DtaValueLabel {
    std::string name;                  // label-set name
    std::map<int64_t, std::string> map;  // numeric code -> display text
};

// One <characteristics> entry (117/118 only; the field names mirror the writer's
// DtaWriteChar — dta_writer.h — so `import-dta --out-meta`'s JSON crosses back into
// `export-dta --in-meta` with no reshaping). `varname` is a Frame column name or the
// literal "_dta" for dataset-level chars (Stata's own tsset/notes convention).
struct DtaChar {
    std::string varname;
    std::string charname;
    std::string value;
};

struct DtaResult {
    Frame frame;
    int format_version = 0;            // 115 | 117 | 118
    std::string data_label;
    std::string timestamp;             // <timestamp> ("" = none, or old-format not carried — see below)
    std::vector<DtaVarMeta> var_meta;  // parallel to frame.columns(), same order
    std::vector<DtaValueLabel> value_labels;  // all label sets defined in the file
    std::vector<DtaChar> characteristics;  // all <characteristics> entries (117/118 only)
};

// Read a Stata .dta file (format 114, 115, 117, or 118 — auto-detected from the
// header; 114 and 115 share one legacy binary container) into a Frame + metadata,
// matching pandas.read_stata(...,
// convert_categoricals=False) column-for-column (dtype + numeric value).
// Throws EngineError("data_io", ...) on any parse failure or unsupported format.
DtaResult read_dta(const std::string& path);

}  // namespace cecon
