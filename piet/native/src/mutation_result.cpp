// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared `mutationResult` builder — see piet/mutation_result.h for the
// three-parameter contract and why this was consolidated from four per-TU
// copies (handlers.cpp, handlers_m3.cpp, handlers_data_verbs.cpp,
// egen.cpp; rule-of-three, COMMS §4 item 9).
#include "piet/mutation_result.h"

#include <set>

#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

ojson mutationResult(SessionState& session, const std::string& action,
                     const std::vector<std::string>& newVars,
                     const std::vector<std::string>& droppedVars,
                     const std::vector<std::string>& rewrittenVars) {
  // BUG-291: a column that just left the frame must not leave its metadata
  // behind for the next column of the same name to inherit. Declared drops are
  // erased on BOTH paths (the resident fast path returns without ever building
  // a card we could reconcile against), so drop/keep/rename behave identically
  // whichever plane served the mutation.
  for (const auto& name : droppedVars) session.forgetVarPlanes(name);

  // Stage 1b: resident-frame fast path — fold the just-completed mutation's
  // frame delta into the shadow varlist card without re-mmapping the
  // (virtual) head. Returns nullopt (falls through to the real rebuild) for
  // a structural change, a §25-only engine, or any non-dispatch call path.
  if (auto resident = session.tryResidentMutationCard(action, newVars, droppedVars, rewrittenVars))
    return *resident;

  // MUTRES-WIRE (2026-08-22). The fold declined, and the ONLY reason the code
  // below opens a Datastore is to read metadata — the column list, its types,
  // formats, labels, missing counts. For a VIRTUAL head that open is not a read:
  // it writes the entire frame to disk first. Measured on the acceptance corpus,
  // this single site forced 640 head materializations totalling 82.28 GB, 128.6 MB
  // each — 71% of every byte the run materialized, and the mechanism by which
  // joinby's earlier fix relocated its bytes instead of removing them.
  //
  // The engine has that metadata in RAM and now offers it two ways (see
  // cecon_cli.cpp): pushed on the run response when it knows the card cannot
  // survive (card_stable false), and pullable via the `vars` op for the refusals
  // it does not know about — chiefly `plan-declined`, where the engine thought
  // the card was fine and PIET's own fold plan is what failed. Push first (free),
  // then pull (one ~40 KB round trip), then the original path unchanged.
  if (const CobraFrameMeta* wire = session.pendingWireDelta() ? session.pendingWireDelta()
                                                             : session.fetchWireVarsFromWorker()) {
    // COMPLETENESS GUARD, load-bearing. The BUG-291 reconcile below deletes the
    // varname-keyed planes of every column NOT in this list, so a short list
    // silently strips metadata from columns that still exist. The engine sends a
    // full varlist by contract; this asserts it rather than trusting it, and any
    // disagreement falls back to the datastore rebuild — the same "never ship a
    // partial card" rule the fold sites already follow.
    const bool complete = wire->valid && static_cast<int64_t>(wire->vars.size()) == wire->cols;
    if (complete) {
      // seedShadowFromFrameVars is the EXISTING wire→card seeder (`use` and the
      // resync paths already use it). It carries format/label forward from the
      // previous card and takes the engine's only when non-empty — absent is not
      // a value (BUG-365) — leaves `levels` empty, which is its normal steady
      // state, and pairs residentRows_ with the card.
      session.seedShadowFromFrameVarsPublic(*wire);
      const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent();
      if (card != nullptr) {
        ojson out;
        out["type"] = "data_mutation";
        out["action"] = action;
        ojson varlistAfter = ojson::array();
        auto vlist = session.varlistWithMetaFrom(*card);
        for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
        session.updateNMissCache(vlist);
        // BUG-291 second half, identical to the datastore path below: verbs like
        // collapse/reshape/contract drop columns without declaring them, so the
        // authoritative post-mutation set reconciles the planes. Safe here only
        // because of the completeness guard above.
        {
          std::set<std::string> live;
          for (const auto& v : vlist) live.insert(v.name);
          std::vector<std::string> gone;
          for (const auto& [name, meta] : session.varMeta) {
            (void)meta;
            if (live.count(name) == 0) gone.push_back(name);
          }
          for (const auto& name : gone) session.forgetVarPlanes(name);
        }
        out["varlist_after"] = varlistAfter;
        out["n_rows_after"] = wire->rows;
        if (!newVars.empty()) out["new_vars"] = newVars;
        if (!droppedVars.empty()) out["dropped_vars"] = droppedVars;
        // seedNMissForPureAdd is deliberately NOT called: its every branch only
        // pushes PIET's nMissCache_ INTO a Datastore via seedNMissing(), and
        // there is no Datastore on this path. The counts themselves come from
        // the engine on the wire, and updateNMissCache above refreshes the cache
        // from them, so nothing is lost by skipping it.
        session.foldTracePublic("wire-rebuild-ok", static_cast<long long>(vlist.size()),
                                static_cast<long long>(wire->rows));
        return out;
      }
    }
    // Two DIFFERENT refusals used to share one label, and the census caught the
    // lie: every "wire-shape-mismatch" row it recorded read "12 vars vs 12 cols"
    // — the shape MATCHED. The real cause was the second condition, a card that
    // seeded fine but does not describe the user's current dataset (the worker
    // can be resynced onto a post-estimation frame while currentHbin_ still names
    // the working dataset — see seedShadowFromFrameVars' own note). Declining
    // there is correct; calling it a shape mismatch is not, and a diagnostic that
    // names the wrong cause is the BUG-362 class one layer down. Split.
    if (wire->valid && static_cast<int64_t>(wire->vars.size()) == wire->cols) {
      session.foldTracePublic("wire-card-not-current",
                              static_cast<long long>(wire->vars.size()),
                              static_cast<long long>(wire->rows));
    } else {
      session.foldTracePublic("wire-shape-mismatch",
                              static_cast<long long>(wire->vars.size()),
                              static_cast<long long>(wire->cols));
    }
  }

  session.invalidateDatastore();
  Datastore* ds = session.datastore();
  // Ordering fix: seedNMissForPureAdd MUST run BEFORE reseedShadowFrom.
  // reseedShadowFrom calls ds->varlist(), which memoizes Datastore::
  // varinfoCache_ ONCE per fresh ds and never rebuilds it; building that
  // cache routes every column through nMissing()'s nMissSeed_ lookup, so an
  // unseeded ds pays a full O(rows) scan per column that then freezes into
  // the cache forever — this is why the L4 sort/order carry-forward
  // (session_state.cpp seedNMissForPureAdd, branch 5) never actually
  // skipped the scan. seedNMissForPureAdd itself only reads SessionState::
  // nMissCache_ (set by the PRIOR mutation's updateNMissCache) and calls
  // the plain ds->seedNMissing() setter — no dependency on varlist()/the
  // shadow card, so it's safe to run first.
  session.seedNMissForPureAdd(ds, action, newVars, droppedVars, rewrittenVars);
  session.reseedShadowFrom(ds);  // keep the shadow coherent with this real rebuild.
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = action;
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  // BUG-291, second half: collapse / reshape / contract / duplicates drop
  // columns WITHOUT declaring them in droppedVars, so the loop above cannot see
  // them. On this (non-resident) path the freshly rebuilt card is authoritative,
  // so reconcile the varname-keyed planes against it. Guarded on `ds` because a
  // null datastore yields an empty vlist that would otherwise wipe every plane.
  if (ds != nullptr) {
    std::set<std::string> live;
    for (const auto& v : vlist) live.insert(v.name);
    std::vector<std::string> gone;
    for (const auto& [name, meta] : session.varMeta) {
      (void)meta;
      if (live.count(name) == 0) gone.push_back(name);
    }
    for (const auto& name : gone) session.forgetVarPlanes(name);
  }
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  if (!newVars.empty()) out["new_vars"] = newVars;
  if (!droppedVars.empty()) out["dropped_vars"] = droppedVars;
  return out;
}

}  // namespace piet::handlers
