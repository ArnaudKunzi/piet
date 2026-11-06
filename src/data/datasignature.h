// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — `datasignature`: a reproducible fingerprint of a Frame's CONTENT.
//
// P1-A3-DATASIG-VERB (internal note P1-A345-RESCOPE-20260730.md §2.4, DESIGN-dta-writer.md
// §4.2 sequencing step 1). This is the head of P1 ARC 3 and the lynchpin of the
// .dta writer's reverse oracle: once a signature exists, "did the writer preserve
// the data?" becomes one string comparison instead of a bespoke diff.
//
// Q9 RULING — engine-deterministic, NOT Stata-exact (decided from live StataBE19.5
// observation by P1-A3-DATASIG-ORACLE, tests/eval/golden/datasig_oracle{,_kfirm}.log;
// full record in tests/eval/manifest.json's P1A3_datasignature_reproducibility
// _comment). Stata's own signature packs an undocumented parenthetical term whose
// behavior is not even self-consistent across fixtures (it moves under a 12-var and
// a 2-var reorder but not under a 3-var one), and there is no published algorithm
// for the two checksums, so there is nothing well-defined to hit bit-for-bit. What
// the oracle DID pin is the CONTRACT Stata's signature satisfies, and that is what
// this function must satisfy too:
//   · deterministic        — same content, same string, every time, in-process or not
//   · reorder-sensitive    — moving a variable changes the string
//   · value-sensitive      — editing one cell changes the string
//   · narrowing-sensitive  — double→float storage narrowing changes the string
//   · format-INsensitive   — a display-format-only edit (%tw→%td) must NOT change it
//                            (Stata's did not; and P1-A0's carried `format` is a
//                            display string that no data-layer decision may read)
//
// SHAPE (Stata-shaped so the object is familiar, never presented as comparable):
//     <rows>:<cols>(<meta>):<vals-a>:<vals-b>
// e.g. `74:12(2148294703):1189232093:3600492578`. `meta` fingerprints the column
// IDENTITY plane (names, order, kind, factor dictionaries); `vals-a`/`vals-b` are two
// differently-seeded fingerprints of the STORED VALUES, mirroring Stata's own pair so
// a single 32-bit collision cannot silently pass a corrupted round-trip.
//
// It is a FREE FUNCTION over `const Frame&`, in its own file, deliberately: a
// signature is a pure function ON a Frame, not Frame state, and frame.{h,cpp} is a
// shared hot file whose manifest surface should not be widened for a read-only
// derivation (P1-A3-DATASIG-VERB row, SPRINT).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "data/frame.h"

namespace cecon {

// Fingerprint `fr`'s content. Read-only: every column is read through the const
// CowVec proxies, so a BORROWED (mmap-backed) column is never materialized by
// signing it — computing a signature must not cost what loading a copy costs.
// BUG-1461: what PIET's user-facing `datasignature` adds to the fingerprint so
// that it answers the questions Stata's does. Default-constructed, both
// signatures are exactly the ones above and below (the writer's round-trip
// oracle keeps row order in the value plane).
struct SignatureOptions {
    // Each column's values sign as an order-free multiset: sorting the rows, or
    // swapping two values within a column, leaves the signature unchanged
    // (StataBE 19.5: `gsort -x` then `datasignature confirm` is "data unchanged").
    bool unordered = false;
    // Column name -> Stata storage type (`byte`, `double`, `str12`, ...), signed
    // ahead of that column's values; the engine keeps no storage types of its own,
    // and Stata's signature is a function of them ([D] datasignature).
    std::map<std::string, std::string> types;
};

std::string data_signature(const Frame& fr, const SignatureOptions& opts = {});

// `<rows>:<k>:<vals-a>:<vals-b>` over `vars` in order (repeats allowed) and all of
// `fr`'s rows: the nonames shape e(datasignature) takes after an estimation
// command (signestimationsample). Throws std::runtime_error for an unknown name.
std::string data_signature_vars(const Frame& fr, const std::vector<std::string>& vars,
                                const SignatureOptions& opts = {});

}  // namespace cecon
