// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// See piet/column_fetch.h for why this pair is shared rather than replicated.
#include "piet/column_fetch.h"
#include "piet/translate.h"

#include <atomic>
#include <filesystem>
#include <regex>
#include <set>
#include <system_error>

#include "piet/cobra_session.h"
#include "piet/datastore.h"
#include "piet/eval/exprfns.h"  // eval::kRxName (the Unicode name rule, core/ident.h)
#include "piet/session_state.h"
#include "io/columnar.h"
#include "io/win_compat.h"

namespace piet {
namespace {

// Same scheme as strmutate's maskStringLiterals: blank the inside of quoted runs
// so an identifier-looking word inside a string literal is not read as a column.
std::string maskStringLiterals(const std::string& s) {
  std::string out = s;
  bool in = false;
  char q = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    if (!in && (out[i] == '"' || out[i] == '\'')) { in = true; q = out[i]; continue; }
    if (in && out[i] == q) { in = false; continue; }
    if (in) out[i] = ' ';
  }
  return out;
}

// Unique per call: a projection must never collide with a live one from the same
// statement (a `replace` whose expression is evaluated while another fetch's file
// is still mapped).
std::string nextFetchPath(const std::string& workdir) {
  static std::atomic<unsigned long long> seq{0};
  return workdir + "/getcols_" + std::to_string(seq++) + ".hbin";
}

}  // namespace

// See the header for the safety contract these two share.
//
// Reuses referencesFactor's scanner verbatim (mask string literals, take bare
// identifiers, skip anything followed by '(' so a function name is not mistaken
// for a column). Only names the session's shadow varlist confirms are columns are
// returned — an unknown identifier is a local, a scalar, or a typo, and either way
// asking the engine for it would fail the WHOLE getcols request (section 38 refuses
// partial projections on purpose).
//
// Returns nullopt when the answer cannot be trusted: no shadow card, or an
// identifier that might be a column but is not on the card. The caller then takes
// the ordinary materialize-the-whole-frame path. Guessing SMALL here is the
// dangerous direction — a missed reference would make evalRows fail on a column the
// mini frame does not carry — so the predicate is "prove every identifier", not
// "collect what looks right".
std::optional<std::vector<std::string>> referencedColumns(
    SessionState& session, const std::vector<const std::string*>& exprs,
    const std::string& alsoInclude) {
  const auto* card = session.shadowVarlistIfCurrent();
  if (card == nullptr) return std::nullopt;
  std::set<std::string> known;
  for (const auto& info : *card) known.insert(info.name);

  std::set<std::string> want;
  if (!alsoInclude.empty() && known.count(alsoInclude)) want.insert(alsoInclude);
  // A Unicode name is one name (core/ident.h): `strlen(ملاحظات) > x` must fetch
  // ملاحظات, not only x.
  static const std::regex reIdent(eval::kRxName);
  for (const std::string* e : exprs) {
    if (e == nullptr || e->empty()) continue;
    std::string masked = maskStringLiterals(*e);
    for (auto it = std::sregex_iterator(masked.begin(), masked.end(), reIdent),
              end = std::sregex_iterator();
         it != end; ++it) {
      const std::smatch& m = *it;
      size_t after = static_cast<size_t>(m.position() + m.length());
      if (after < masked.size() && masked[after] == '(') continue;  // function call
      const std::string id = m.str();
      if (known.count(id)) want.insert(id);
      // else: a local/scalar/keyword/typo. Not proof of a missing column — the
      // engine-side evaluator resolves those itself — so it does not disqualify
      // the fetch. What WOULD disqualify it is a column we failed to name, and
      // that cannot happen: every name on the card that appears here is taken.
    }
  }
  // BUG-369 follow-up: a TS operator reads columns the identifier scan cannot
  // see. `L.beertax` scans as `L` (not on the card, ignored) + `beertax` — but
  // EVALUATING it needs the tsset time column (and panel column under xtset),
  // which appears in no expression text at all. A projection built without them
  // fails "no column: t" the moment the ts context rides the call. Same
  // conservative rule as the rest of this function: if the session's tsset
  // column is not on the card, refuse to answer rather than guess small.
  {
    bool anyTs = false;
    for (const std::string* e : exprs)
      if (e != nullptr && translate::exprHasTsToken(*e)) { anyTs = true; break; }
    if (anyTs) {
      for (const auto& tsv : {session.timevar, session.panelvar}) {
        if (tsv.has_value() && !tsv->empty()) {
          if (!known.count(*tsv)) return std::nullopt;
          want.insert(*tsv);
        }
      }
    }
  }
  if (want.empty()) return std::nullopt;   // nothing to fetch; the old path is cheaper
  return std::vector<std::string>(want.begin(), want.end());
}

// The mini-frame fetch itself. Returns the opened Datastore on success, and leaves
// `session` untouched on every failure path — this is a pure optimization, so a
// miss must cost nothing but a fallback.
std::unique_ptr<Datastore> fetchColumns(SessionState& session,
                                        const std::vector<std::string>& cols,
                                        const std::string& workdir, std::string* pathOut) {
  piet::CobraSession* cs = session.cobraSessionIfLive();
  if (cs == nullptr || cs->framesState() != piet::CobraSession::Frames::Yes) return nullptr;
  if (!session.residentFrameLive() || !session.residentHeadDirty()) return nullptr;
  const std::string out = nextFetchPath(workdir);
  CobraRunResult r = cs->frameGetcols(cols, out);
  if (!r.ok) {
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(out), ec);  // never leave a torn file
    return nullptr;
  }
  try {
    auto ds = std::make_unique<Datastore>(out);
    *pathOut = out;
    // BUG-360 follow-up: the failure paths above remove `out`; the SUCCESS path
    // never did, so every projection this function ever handed back stayed on
    // disk until the process exited (692 files / 460 MB on a five-task
    // controls=yes slice). Hand it to the session, which drains the ledger at
    // the outermost dispatch boundary — a projection's life is one command.
    session.noteProjectionScratch(out);
    return ds;
  } catch (const std::exception&) {
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(out), ec);
    return nullptr;
  }
}

// `dataPath` is the .hbin `setcol --data` must patch. It is normally ds.path(), but
// when `ds` is a getcols PROJECTION (B2) the two differ: values and dtypes come from
// the projection, while the mutation still has to land on the session's real head.
}  // namespace piet
