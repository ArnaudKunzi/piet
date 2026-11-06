// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The one delimited-text reader behind `import delimited` and `insheet`
// (internal note design-step3.md section
// 2). Three steps, each a rule read off the StataBE 19.5 oracle
// (tests/eval/golden/aer_w2/w2ar_data_io_family.log, "log:N" below):
//
//   parse()      bytes -> records of fields: delimiter set (one or several
//                characters, or one multi-character string), collapsed runs,
//                quote binding (loose: a quote binds to the end of its line;
//                strict: a quoted field may span up to maxquotedrows lines;
//                nobind: quotes are data) and quote stripping (default: the
//                enclosing quotes off and a doubled quote undoubled; yes: every
//                quote off; no: the field as written). log:3181-3286.
//   legalNames() header cells -> Stata names, plus the label to give a column
//                whose name differs from its cell (log:2570-2581).
//   typeColumn() the cells of one column -> its storage type (log:3331-3347).
//
// stageAndAdopt() then installs the typed table as the session's dataset and
// records every column's storage type and label, so `: type`, describe and
// the writers see the declared types (FU-1253).
#ifndef PIET_NATIVE_DELIMITED_IO_H
#define PIET_NATIVE_DELIMITED_IO_H

#include <optional>
#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
}

namespace piet::delimited {

struct ReadSpec {
  // Delimiter characters (any one ends a field). When `asString`, the whole
  // string is ONE delimiter.
  std::string delims = ",";
  bool asString = false;
  bool collapse = false;  // a run of delimiters counts as one
  enum class Bind { kLoose, kStrict, kNoBind };
  Bind bind = Bind::kLoose;
  // strict: how many lines a quoted field may span before the quote is
  // abandoned (Stata's default is 20, per help import delimited); 0 = no limit.
  long long maxQuotedRows = 20;
  enum class Strip { kDefault, kYes, kNo };
  Strip strip = Strip::kDefault;
  bool skipEmptyLines = true;
};

// One record per line (or per quoted multi-line record under strict). An
// empty line is skipped when spec.skipEmptyLines.
std::vector<std::vector<std::string>> parse(const std::string& text, const ReadSpec& spec);

// insheet's own reader (w2ar log:3526-3690): a field is trimmed of blanks, but
// a string column's width is its widest field BEFORE trimming (`  pad  ` makes
// name str7, log:3529); a quoted field is the text between its quotes, and a
// quoted field followed by anything but the delimiter reads as "" (`"say
// ""hi"""` -> [], log:3530), and a quote left open at the end of a line takes
// in the next line, joined by a blank. `delim` 0 = the line is one field (a
// first line with neither tab nor comma, dx_ins_028_names log:3630). Empty
// lines skip.
struct InsheetRows {
  std::vector<std::vector<std::string>> fields;
  std::vector<std::vector<size_t>> widths;  // parallel: each field's untrimmed width
};
InsheetRows parseInsheet(const std::string& text, char delim);

// The delimiter `import delimited` uses when none is named: the most frequent
// of comma, tab, semicolon and pipe on the first line, outside quotes (comma
// when none occurs). w2ar dx_imd_varlist reads a `;` file unprompted
// (log:3508).
char detectDelimiter(const std::string& text);

// A field Stata's reader takes as a number: optional blanks, sign, digits with
// at most one point, optional exponent. `*out` gets the value.
bool parseNumber(const std::string& field, double* out);
// "", "." and ".a".."z": a numeric missing.
bool isMissingToken(const std::string& field);

enum class Case { kPreserve, kLower, kUpper };

struct NameResult {
  std::vector<std::string> names;
  std::vector<std::string> labels;  // "" = no label
  bool clash = false;  // a v<column> was already taken (Fallback::kRefuse)
};

// What a column gets when its v<column> fallback is itself taken by an earlier
// column (StataBE 19.5, header `v3,b` over a row `3,4,5`; `v2,v2`; `a,v4` over
// four fields): insheet takes the lowest v# still free (v3 b v1, v2 v1, a v4 v3
// v1); import delimited refuses the file, "Error creating variables" r(198),
// which the caller says when `clash` comes back set.
enum class Fallback { kRefuse, kLowestFree };

// Header cells -> legal, unique Stata names (StataBE 19.5, probes
// scratch-r150/uni-v/p1.do, p2.do): ASCII letters/digits/_ and every Unicode
// letter or decimal digit are kept, every other character is removed, leading
// digits (any script) dropped, at most 32 CHARACTERS, then case-folded with the
// full Unicode mappings (sharp s -> SS, final sigma); an empty result, a result
// past 32 characters after the fold, a reserved word or a name an earlier column
// already took becomes v<column>. A column whose name differs from its cell gets
// the cell as its label (log:2573-2579: `Value X` -> valuex [Value X],
// `2nd` -> nd [2nd], `value x` -> v5). Columns past the header are empty cells.
NameResult legalNames(const std::vector<std::string>& cells, Case c,
                      Fallback f = Fallback::kRefuse);

// Full Unicode case mapping of a UTF-8 string (Case::kPreserve returns it as is).
std::string caseMapUtf8(const std::string& s, Case c);

// Storage type of one column of cells. `forceString` / `forceNumeric` are
// stringcols()/numericcols(); `asDouble` makes a non-integer column double.
//   any non-number (not a missing token) -> str# (# = longest byte length,
//     at least 1; strL past 2045)
//   all integers -> byte [-127,100], int [-32767,32740],
//     long [-2147483647,2147483620], else double
//   otherwise float (double with asDouble, or when a magnitude passes 2^24);
//   all missing -> byte
// A numericcols() column holding text is float, the text read as missing
// (log:3061 dx_imd_024_numericcols code float).
struct ColumnType {
  bool isString = false;
  std::string stataType;  // byte|int|long|float|double|str#|strL
};
// `precisionToDouble` (import delimited) lets the magnitude/precision rule
// above pick double; insheet stores float whatever the digits (w2k_binscatter
// BS_077_data_o1, w2k_binscatter.log:1477), so it passes false.
ColumnType typeColumn(const std::vector<const std::string*>& cells, bool forceString,
                      bool forceNumeric, bool asDouble, size_t widthFloor = 0,
                      bool precisionToDouble = true);

// A typed table ready to install.
struct Table {
  std::vector<std::string> names;
  std::vector<std::string> labels;     // parallel; "" = none
  std::vector<ColumnType> types;       // parallel
  std::vector<std::vector<std::string>> rows;  // rows[r][c], the field text as read
};

// Stage `t` (strings quoted so blanks survive; numbers at full precision
// after rounding to their storage type) through `cobra convert`, adopt it as
// the session's dataset with `sourceResolved` as c(filename), and record each
// column's storage type and label. nullopt = success; else the error block.
std::optional<nlohmann::ordered_json> stageAndAdopt(SessionState& session, const Table& t,
                                                    const std::string& verb,
                                                    const std::string& sourceResolved);

}  // namespace piet::delimited

#endif  // PIET_NATIVE_DELIMITED_IO_H
