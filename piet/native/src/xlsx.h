// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Minimal .xlsx (SpreadsheetML) reader/writer — P1 wave `import excel` /
// `export excel`.
//
// Scope, stated plainly: this is a DATA interchange layer, not an Excel
// implementation. It reads the cell values of the first worksheet (or a named
// one) and writes a single worksheet of values. No styles, formulas, dates as
// formatted values, merged cells, or multiple sheets on write.
//
// Two facts drove the shape:
//   * a .xlsx is a ZIP of XML parts, and every real producer (Excel, Stata)
//     DEFLATEs those parts — so the reader must inflate. RFC 1951 is
//     implemented here from the specification (fixed + dynamic Huffman and
//     stored blocks); there is no third-party dependency in this repo to lean
//     on and adding one for four call sites is not worth the supply chain.
//   * the writer does NOT need to compress: STORE (method 0) is a fully legal
//     ZIP method that Excel, Numbers, LibreOffice and Stata all open. Writing
//     stored entries keeps the writer to a CRC-32 and a header layout.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace piet::xlsx {

// One cell as read: either a number or a string. `isNumber == false` with an
// empty `text` means the cell was blank (or absent) — the caller decides
// whether that is a missing value.
struct Cell {
  bool isNumber = false;
  double number = 0.0;
  std::string text;
  bool blank = true;
  // The cell's number format, from its style (xl/styles.xml cellXfs): the
  // numFmtId and, for a custom one, its format code. `isDate` is true for a
  // date format (the built-in ids 14-22, 27-36, 45-47, 50-58, or a custom
  // code with day/month/year letters outside quotes and brackets). Stata's
  // import excel reads a date-formatted column as %td (w2ar log:1714).
  int numFmtId = 0;
  std::string numFmtCode;
  bool isDate = false;
};

struct Sheet {
  std::vector<std::string> header;        // row 1 when the caller asked for firstrow
  std::vector<std::vector<Cell>> rows;    // data rows, rectangular (padded with blanks)
  // 1-based SPREADSHEET column that `rows[*][0]` came from. Always 1 without
  // `cellrange`; with `cellrange(C3:D9)` it is 3, and it is the reason
  // `import excel`'s default variable names stay Stata's (`C`, `D`) instead of
  // renaming the window's first column to `A`.
  int firstCol = 1;
};

// AER scope-2 W3a: `import excel ..., sheet("NoSuchSheet")` is r(601) in Stata,
// with the message `worksheet NoSuchSheet not found` (captured, aer_b4
// pins_oracle_g.log:136-139). A distinct exception type so the Stata
// surface answers that code from a TYPE rather than by matching on the text of
// a generic runtime_error.
struct SheetNotFound : std::runtime_error {
  explicit SheetNotFound(const std::string& name)
      : std::runtime_error("worksheet " + name + " not found"), sheetName(name) {}
  std::string sheetName;
};

// AER scope-2 B4 P2: a `cellrange()` whose lower-right corner is outside the
// worksheet's data area. W3a CLAMPED, and said so in its own comment — "Named
// limitation, not a measured Stata match — the aer_b4 capture has no cellrange
// record to settle it against". oracle_q.log now settles it: Stata REFUSES,
// r(198), with TWO distinct messages, one for the column and one for the row
// (oracle_q.log:74-79 and :84-89), and leaves the data in memory untouched
// (c(k)==0, _N==0 on both, :81-82 and :91-92).
//
// The message is carried verbatim, two lines, because the second line is where
// Stata says WHICH corner is wrong — the whole difference between the two.
// A distinct type for the same reason SheetNotFound is one: the Stata surface
// answers the code from a TYPE, not by matching the text of a generic error.
struct CellRangeOutsideData : std::runtime_error {
  explicit CellRangeOutsideData(bool columnAxis)
      : std::runtime_error(
            columnAxis
                ? "invalid column range in option cellrange();\n"
                  "    The lower right column is right of the data area in the worksheet."
                : "invalid row range in option cellrange();\n"
                  "    The lower right row is after the data area in the worksheet."),
        isColumn(columnAxis) {}
  bool isColumn;
};

// One worksheet as the WORKBOOK declares it — not as the ZIP happens to be laid
// out. `import excel ..., describe` reports these, and `sheet("name")` selects
// from them.
struct SheetInfo {
  std::string name;   // xl/workbook.xml's <sheet name="...">
  std::string part;   // the resolved ZIP part, e.g. "xl/worksheets/sheet2.xml";
                      // empty when the relationship does not resolve to a part
  std::string range;  // used cell range, "A1:C75" form; empty for an empty sheet
};

// The workbook's sheets, IN WORKBOOK ORDER, each resolved name -> r:id ->
// relationship Target -> part path. Throws like read() when the file is not a
// readable .xlsx. `range` costs a full parse of each worksheet part, which is
// what `describe` is for; nothing on the load path calls this.
std::vector<SheetInfo> sheets(const std::string& path);

// What `read()` is allowed to narrow to.
struct ReadOptions {
  bool firstRowIsHeader = false;
  // Empty selects the workbook's FIRST sheet (workbook order, not part-name
  // order). A name that is not in the workbook throws SheetNotFound.
  std::string sheetName;
  // `cellrange` window, 1-based and inclusive on both ends. 0 means "unbounded
  // on that side", so `cellrange(A8)` is firstRow=8 with everything else 0 and
  // `cellrange(:D70)` is lastRow=70,lastCol=4 with the firsts 0.
  int firstRow = 0, lastRow = 0, firstCol = 0, lastCol = 0;
};

// Read one worksheet of `path`. `firstRowIsHeader` moves the first row OF THE
// WINDOW into `header` instead of `rows` (Stata: "firstrow uses the first row
// of the cell range for variable names if cellrange() is specified", [D] import
// excel). Throws SheetNotFound for a missing named sheet, and
// std::runtime_error with a caller-ready message on anything else it cannot
// handle (not a ZIP, encrypted, no worksheet, an unsupported compression
// method).
Sheet read(const std::string& path, const ReadOptions& opts);

// The pre-W3a spelling, kept so no existing caller had to change: the first
// worksheet, whole, with no window.
Sheet read(const std::string& path, bool firstRowIsHeader);

// How many worksheets `path` contains. Added for AER-SCOPE1 bundle G, where
// `putexcel set ..., modify` must refuse a workbook this build cannot rewrite
// without discarding sheets.
//
// It exists because the alternative was a byte-scan of the file for
// "xl/worksheets/", and that heuristic was WRONG in the ordinary case: a STORED
// zip puts the next record's "PK" signature immediately after the filename, so
// the scan read `sheet1.xmlPK` as a second, distinct sheet and every workbook
// this writer produces looked like a multi-sheet one. This uses the same ZIP
// reader `read()` does. Returns 0 when the file cannot be opened or parsed --
// the caller decides whether that is a refusal or a fresh-file case.
size_t worksheetCount(const std::string& path);

// Column of values to write. Exactly one of `numbers`/`strings` is used per
// row: `isNumber[i]` selects. A row marked neither number nor string is
// written as a blank cell.
struct OutColumn {
  std::string name;
  std::vector<bool> isNumber;
  std::vector<bool> isBlank;
  std::vector<double> numbers;
  std::vector<std::string> strings;
  // The number format the column's numeric cells carry: 0 = General (no
  // style), 14 = the m/d/yyyy date format Stata writes for a %td variable,
  // 2 = the 0.00 format it writes for a %#.2f one (w2ar log:1089-1104). A
  // non-zero id makes the workbook carry a styles part.
  int numFmtId = 0;
};

// Write a single worksheet. `writeHeader` emits the column names as row 1.
// `sheetName` is what `export excel ..., sheet("Alpha")` puts in workbook.xml;
// empty keeps the historical "Sheet1", so a call without it is byte-identical
// to one made before W3a. Throws std::runtime_error on an I/O failure.
void write(const std::string& path, const std::vector<OutColumn>& cols, bool writeHeader,
           const std::string& sheetName = std::string());

// `export excel ..., sheet("S") sheetreplace` — replace (or insert) ONE
// worksheet of an EXISTING workbook, keeping every other part of it. Where
// write() authors a whole package, this one rewrites someone else's: the sheet
// part is replaced when the name is already in the workbook, and otherwise a
// new part is added and workbook.xml, xl/_rels/workbook.xml.rels and
// [Content_Types].xml are all patched to agree about it (a package where those
// four disagree is one Excel offers to "repair", in front of the user's own
// file). Every check runs BEFORE any part is replaced, and the result is
// committed through a sibling temp + rename(2), so a refusal leaves the
// workbook exactly as it was.
//
// Named limitations, not silent ones: the repackaged workbook is STORED rather
// than DEFLATEd (this file has no deflate), and no styles part is authored, so
// `keepcellfmt` is still unimplemented.
//
// Throws std::runtime_error when `path` is not a readable .xlsx, when the parts
// it must patch are missing or not the documents they claim to be, or on I/O.
void writeSheetInto(const std::string& path, const std::string& sheetName,
                    const std::vector<OutColumn>& cols, bool writeHeader);

// AER-SCOPE1 bundle G (IO-TEXT): the CELL-addressed writer `putexcel` needs.
//
// `write()` above is rectangular -- a list of equal-length columns -- which is
// the right shape for `export excel` and the wrong one for `putexcel`, whose
// whole grammar is "put this value at B7". Rather than bend a sparse cell map
// into fake columns (which would have to invent blanks for every gap and could
// not express a merge at all), this shares `write()`'s XML skeleton, column-ref
// helper, escaping and ZIP entry code, and differs only in how the rows and
// cells are emitted.
struct CellOut {
  int row = 1;   // 1-based, as the user writes it (A1 -> row 1, col 1)
  int col = 1;
  bool isNumber = false;
  double number = 0.0;
  std::string text;
};

// Write `cells` to `path` as a single worksheet named `sheetName`. `merges`
// holds ranges in "A1:B2" form; an empty vector emits no <mergeCells> element
// at all, so an unmerged workbook is byte-identical to one written before this
// entry point existed. Throws std::runtime_error on an I/O failure.
void writeCells(const std::string& path, const std::string& sheetName,
                const std::vector<CellOut>& cells, const std::vector<std::string>& merges);

}  // namespace piet::xlsx
