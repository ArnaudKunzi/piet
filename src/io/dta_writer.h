// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — native writer for Stata format-118 .dta files. The inverse of
// io/dta_reader.{h,cpp}: for every byte that reader parses, this writer emits the
// byte real Stata (and therefore our own reader) will read back unchanged.
//
// P1-A3-WRITER-NUMERIC (internal note P1-A3-WRITER-IMPL-brief.md · internal note DESIGN-dta-writer.md
// §3) laid out the container and wrote the five NUMERIC types.
// P1-A3-WRITER-STRINGS-IMPL (ARC 3 step 4) FILLS the four fields step 3 deliberately
// emitted empty — it did not re-lay out the file: `str#` columns
// (<variable_types> + the data matrix), <variable_labels>, <value_label_names> +
// <value_labels>, <characteristics>, and strL text/binary GSOs.
//
// BYTE LAYOUTS ARE MEASURED, NOT DOC-DERIVED (step 4's one real trap). Two of the
// three new sections have a length prefix whose span the Stata manual states only in
// prose, and the prose reading was WRONG:
//   · <lbl> `slength` = **8 + 8n + txtlen** — it spans n/txtlen/off[]/val[]/txt and
//     EXCLUDES labname(129)+padding(3). Measured on Stata's own
//     /Applications/StataNow/auto.dta (slength=41, n=2, txtlen=17) and
//     ado/base/a/auto2.dta (41 and 81); pandas 3.0.3's independent StataWriter agrees
//     (46 = 8+8·3+14). The doc-prose reading "length of the value_label_table"
//     (= 140+8n+txtlen, counting from labname) is what a re-derivation lands on and
//     is not what either writer emits. OUR OWN READER CANNOT ADJUDICATE THIS: it reads
//     slength into a `(void)`-discarded local (dta_reader.cpp:415-418), so a wrong
//     value round-trips GREEN through gate ① and only real Stata would reject the file.
//   · <ch> block = `<ch>` + u4 len + varname(129) + charname(129) + contents +
//     NUL + `</ch>`, where **len = 258 + contents.size() + 1** (the NUL counts).
//     Measured on tests/eval/data/ts_diag_input.dta's 4 real `_dta[…]` tsset chars
//     (len=260 for a 1-byte value, 280 for a 21-byte one).
// If either is ever re-derived from prose again, re-measure those two files first.
//
// THE INVARIANT (non-negotiable, and it is what makes the type-narrowing policy safe):
// the writer never changes a stored double's value. A narrowing to `float` is
// admissible iff `(double)(float)v == v` for every finite value in the column (spec
// §3.3's guard). That guard is not merely prudent — it is exactly the
// signature-preservation condition, because `data_signature` hashes the raw double
// bits (src/data/datasignature.cpp:61-70). So:
//   · narrowing that PASSES the guard → identical doubles on read-back → identical
//     signature → gate ① green;
//   · narrowing that FAILS it → different doubles → different signature → gate ① red.
// The policy is therefore already gated; there is no separate assertion to trust.
//
// GATE ① (the every-commit net, brief §2.3 row 1):
//     datasignature(import-dta(export-dta(X))) == datasignature(X)
// GATE ②/③ (release leg, needs live StataBE) are the reverse-oracle harness already
// on main from P1-A3-REVORACLE-SCAFFOLD (tests/eval/properties.py
// p1a3_transport_leg{2,3}_*) — "Stata as TRANSPORT, not SIGNER" (brief §2.2).
//
// Q-RULINGS this file implements (do not re-open them here):
//   Q1 type-narrowing   — auto-narrow under the guard above, with an optional declared
//                         type from `--in-meta` honoured whenever it too is lossless.
//                         Ruled an ENGINEERING decision, not Art-13 (brief §4.1): the
//                         engine has never been able to write .dta, so narrowing
//                         cannot regress a promise, and values are unchanged by
//                         invariant. The stop-line that WOULD be Art-13 and is NOT
//                         crossed here: needing a format hint or a container-version
//                         bump to make narrowing work.
//   Q4 strL / str cap    — extended by the interactive persistence continuation:
//                         117/118 write real strL references and GSO records for
//                         long/binary strings and declared strL (see [P] dta §5.11).
//                         Format 115 still refuses unrepresentable strings.
//   Q5 extended missing  — CLOSED 2026-08-05 (P6 rider). All 27 missing values round-trip:
//                         emit_cell writes base+kind per storage type, the exact mirror of
//                         the reader's missing_code_raw(). Before this, a `.a` read in
//                         collapsed to `.` on the way out — a silent value change, and the
//                         asymmetry appeared the moment the READ side learned kinds
//                         (@96caa73). Writing plain `.` for every kind is what this note
//                         used to describe; it is no longer true.
//   Q6 date rescale-back — MOOT for %td/%tw/%tm/%tq/%th (the reader leaves all five as
//                         the raw Stata index, dta_reader.cpp:620-668, so the inverse
//                         is the identity). ONLY %tc/%tC carry a scale, ×86 400 000
//                         (brief §3). Its exactness is a measured property, see §%tc
//                         in the .cpp.
//   Q7 118 only         — no 115/117 write target.
//   Q8 header determinism — <timestamp> empty by default; <sortlist> zeros UNLESS the
//                         Frame carries a sort claim (BUG-272 closed BUG-200: the
//                         claim comes from the Frame's sort-state plane only, never a
//                         sidecar, and is deterministic — a function of data
//                         provenance, not the wall clock, so golden gates hold).
//   Q10 verb name       — `export-dta`, not `save`: cobra's batch CLI is
//                         file-in/file-out with no resident dataset to save in place.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "data/frame.h"

namespace cecon {

// One variable's declared write hints. Normally round-tripped from
// `import-dta --out-meta`'s JSON sidecar through `export-dta --in-meta`, which is the
// ONLY channel by which a source file's Stata storage type can reach a writer: the
// Frame carries f64/factor and nothing else, and the hbin header's key set
// (name/dtype/levels/format) has no `stata_type`, so the declared type dies at the
// hbin boundary one command earlier (brief §1). Absent a hint the writer decides by
// scanning, which is the spec §3.3 policy unchanged.
struct DtaWriteVarMeta {
    std::string name;        // matched against Frame column names; order-independent
    std::string format;      // Stata display format to emit ("" = derive from the type)
    std::string stata_type;  // "byte"|"int"|"long"|"float"|"double" ("" = decide by scan)
    std::string label;       // <variable_labels> entry ("" = none); ≤320 bytes
    // <value_label_names> entry ("" = none). A non-empty name MUST be defined in
    // DtaWriteOptions::value_labels — a name pointing at an absent table is the one
    // way this writer could emit a file Stata opens and then errors on, so it is a
    // reject, not a warning.
    std::string value_label_name;
};

// One value-label set, shaped to mirror the reader's DtaValueLabel (name + code→text)
// so `import-dta --out-meta`'s JSON crosses back with no reshaping. Display-only:
// the column data always carries the raw numeric code (spec §3.4).
struct DtaWriteValueLabels {
    std::string name;
    std::map<int64_t, std::string> map;  // Stata codes are int32; out-of-range = reject
};

// One <characteristics> entry. `varname` is a Frame column name or the literal
// "_dta" for dataset-level chars (Stata's own tsset/notes convention, e.g.
// _dta[_TSitrvl]); it is NOT validated against the column list, because Stata itself
// keeps chars for variables a later `drop` removed.
struct DtaWriteChar {
    std::string varname;
    std::string charname;
    std::string value;
};

struct DtaWriteOptions {
    std::string data_label;                  // <label>; "" = none
    // <timestamp>; "" = none (Q8 default, for byte-determinism when no source
    // timestamp is supplied). FIXED (BUG-199, 2026-08-07): dta_reader.cpp now
    // surfaces the parsed <timestamp> into DtaResult::timestamp (both the XML
    // 117/118 path and the old-format 115 fixed 18-byte field), and
    // apps/cmds/cmds_data.cpp round-trips it through the `import-dta --out-meta` /
    // `export-dta --in-meta` JSON sidecar's "timestamp" key, mirroring "data_label"
    // exactly. A caller that supplies no --in-meta (or an --in-meta predating this
    // fix) still gets "" here, which is the same byte-deterministic default as
    // before — this is purely additive. Measured live: StataBE's own `describe` on
    // /Applications/StataNow/auto.dta shows "13 Apr 2024 17:45"; the same describe
    // on cobra's import-dta -> export-dta round trip of that file (with --out-meta /
    // --in-meta threaded through) now shows the same string.
    std::string timestamp;
    std::vector<DtaWriteVarMeta> var_meta;   // may be empty, or cover only some columns
    std::vector<DtaWriteValueLabels> value_labels;  // emitted verbatim, in this order
    // Chars round-trip through the FULL product surface as of 2026-07-30
    // (FIX-DTA-READER-CHARS-DROP, @bf48e51): dta_reader.cpp captures
    // <characteristics>'s map offset and parses the section (read_characteristics(),
    // dta_reader.cpp ~:458-484) into DtaResult.characteristics, and
    // apps/cmds/cmds_data.cpp wires it both ways (`import-dta --out-meta`'s
    // "characteristics" JSON array <-> `export-dta --in-meta`'s same shape). This
    // comment used to say the reader SKIPS the section (reader §5.2 gap) — that was
    // true before the above commit and is not true now; do not re-derive the gap from
    // this prose without re-checking dta_reader.cpp first (AUDIT-DTA-WRITER-20260806
    // caught this exact staleness). Q3 scope = ALL chars verbatim, no Stata-reserved
    // allow-list.
    std::vector<DtaWriteChar> characteristics;
    // false (default) = write a missing string cell as `""`, which is what Stata
    // itself stores — there is no other spelling in the format. true = refuse to
    // write instead, for callers that would rather fail than let the Frame's
    // missing/`""` distinction collapse. See THE FACTOR ASYMMETRY below.
    bool strict_string_missing = false;
    // The .dta release written: 118 (default; Stata 14+), 117 (Stata 13: the same
    // XML container with 33-byte names, 49-byte formats, 81-byte variable labels,
    // a 4-byte N and a 1-byte data-label length) or 115 (Stata 12: the old binary
    // layout -- a 109-byte header, one-byte type codes 251..255 / str1..str244,
    // expansion fields for the characteristics). `saveold`'s version(): 11/12 ->
    // 115, 13 -> 117, 14 -> 118 (w2ar_data_io_family.log dx_svo_*).
    int release = 118;
};

// Write `fr` to `path` as a Stata format-118 .dta (LSF byte order).
// Throws EngineError("data_io", …) — never truncates, never silently narrows — on:
// a non-finite non-NaN value (Stata has no infinity, so storing one would read back as
// `.` = a silent value change), a value exceeding the strL GSO length limit, a MISSING cell
// in a factor column ONLY when strict_string_missing is set (see below), a value-label
// name with no table, a value-label code
// outside int32, a name/format/label too long for its fixed-width field, or >65535
// variables.
//
// THE FACTOR ASYMMETRY (BUG-024's write side — the one place the format is strictly
// weaker than the Frame). Stata has exactly one string value for "nothing here": `""`.
// The engine has two, and they are NOT the same thing:
//   · a factor LEVEL that is the empty string — a real observed value. Written as a
//     zero-filled str# cell, and read back as a level again (the reader passes
//     empty_is_missing=false, dta_reader.cpp:571-575), so it round-trips exactly. This
//     is the BUG-024 ruling honoured in the write direction.
//   · code < 0, i.e. a genuinely MISSING string cell (only CSV-shaped input produces
//     one; the .dta reader never does). There is no byte for it, because Stata has no
//     string missing to encode — in Stata a string variable's missing IS `""`, so
//     writing `""` is not a deviation from Stata, it is what Stata stores. We therefore
//     WRITE IT, and print one stderr line per affected column naming the column, the
//     count and the first row, so the collapse is reported rather than silent.
//     What is lost is internal to the Frame, not to Stata: `data_signature` does see
//     the difference (missing hashes as one token, `""` hashes as a string:
//     datasignature.cpp:118-127), so an export→import round trip of a CSV-shaped
//     missing does not reproduce its original signature. Set
//     DtaWriteOptions::strict_string_missing to refuse the write instead.
// KNOWN NON-ROUND-TRIP, stated because gate ① would otherwise look flaky: an UNUSED
// factor level (in the dictionary, no row referencing it) is not representable — only
// cells reach the file — so it vanishes on read-back and the signature's identity plane
// changes. `import-dta` never builds such a dictionary; a `keep if` upstream of an
// export can. This is a property of the format, not of the width policy (the width
// covers the whole dictionary either way).
void write_dta(const Frame& fr, const std::string& path,
               const DtaWriteOptions& opt = DtaWriteOptions{});

}  // namespace cecon
