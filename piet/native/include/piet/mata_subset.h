// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M0 -- the native Mata statement subset (ml-getmata T4).
//
// PIET has no Mata runtime. What the AER corpus does in Mata before `getmata`
// (papers 030 and 087) is matrix bookkeeping: build, join, filter, two
// elementwise operations and column extraction. M0 is exactly that set,
// evaluated natively over EvalMatrix, and nothing else:
//
//   stmt    := NAME '=' expr
//   expr    := rowjoin                        (joins bind loosest; `,` tighter than `\`)
//   rowjoin := coljoin ('\' coljoin)*
//   coljoin := addop (',' addop)*
//   addop   := mulop ((':+' | ':-') mulop)*   elementwise, c-conformable broadcast
//   mulop   := unary ((':*' | ':/') unary)*
//   unary   := '-' unary | postfix
//   postfix := primary ['[' sel ',' sel ']']  sel := '.' | positive integer
//   primary := NUMBER | '.' | NAME | '(' expr ')'
//            | J(r, c, v) | st_matrix("NAME") | exp(x) | rows(x) | cols(x) | invsym(x)
//            | select(A, rowmissing(A):==0)   the one idiom, same NAME twice
//
// Every other construct -- another function (rowsum, luinv, ...),
// another operator, a subscripted assignment, a loop, a bare expression -- is
// REFUSED BY NAME (r(199), refusedCommand("mata", ...)). A `mata:` ... `end`
// block is parsed in full before anything runs, so one statement outside M0
// refuses the WHOLE block and assigns nothing. Runtime failures keep Stata's
// Mata codes (3200 conformability, 3301 subscript, 3499 not found).
//
// Namespace (SPEC D3): bare names read and write session.eval.mataMatrices;
// st_matrix("X") reads session.eval.matrices (the Stata store) and nothing else.
#ifndef PIET_NATIVE_MATA_SUBSET_H
#define PIET_NATIVE_MATA_SUBSET_H

#include <cstddef>
#include <string>
#include <vector>

#include "piet/eval/session.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
}

namespace piet::mata {

// The byte that joins a folded block's statements into one line. 0x1D (GS)
// does not occur in Stata source, and it is not the 0x1F `input` uses, so the
// two folds can never be mistaken for each other.
constexpr char kMataStmtSep = '\x1D';
// Appended when the file ended before the block's `end`.
constexpr char kMataUnterminated = '\x1E';

// Fold every `mata:` / `mata` block opener (optionally behind capture/quietly/
// noisily prefixes) and its body up to the matching `end` into ONE line:
// "<prefixes>mata:" + (0x1D + statement)*. Runs in doexec BEFORE
// program::captureProgams, which drops a bare `end`; `program ... end` and
// `input ... end` regions are passed through untouched so the folds cannot eat
// each other's terminator.
std::vector<std::string> captureMataBlocks(const std::vector<std::string>& lines);

// True when a (prefix-stripped) dispatcher line is a Mata line: `mata: stmt`,
// a folded block, or the colon-less `mata` forms (which are refused).
bool isMataLine(const std::string& raw);

// Run one Mata line. Returns ONE error block, or an EMPTY array on success
// (Mata assignments print nothing) -- the caller filters the empty array and
// never pushes it as a block (defect class 1, dispatcher.cpp's mkmat note).
nlohmann::ordered_json runMataLine(const std::string& raw, SessionState& session);

// Run one M0 statement (`NAME = expr`) against the session. Same return shape.
nlohmann::ordered_json runMataStatement(const std::string& stmt, SessionState& session);

// The echo text of a folded block, rendered the way Stata's log shows it
// (": stmt" per line, then ": end"). A line without a fold comes back as is.
std::string renderFoldedMataForEcho(const std::string& raw);

// Column count of a Mata matrix, including a void 0 x c one.
size_t colsOf(const eval::EvalMatrix& m);

// The Mata store's lookup (nullptr when absent) -- bare Mata names only.
const eval::EvalMatrix* findMataMatrix(const eval::EvalSession& ev, const std::string& name);

}  // namespace piet::mata

#endif  // PIET_NATIVE_MATA_SUBSET_H
