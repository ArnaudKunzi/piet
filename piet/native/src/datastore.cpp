// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/datastore.h"

#include <filesystem>
#include <system_error>

#include "piet/counters.h"
#include "io/win_compat.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace piet {

nlohmann::ordered_json VarInfo::toJson() const {
  nlohmann::ordered_json j;
  j["name"] = name;
  j["dtype"] = dtype;
  j["format"] = format;
  j["label"] = label;
  if (nLevels.has_value()) {
    j["n_levels"] = *nLevels;
    // F5 (B.3.0 "bounded transcripts", MAXPERF Stage 0 Ship 2): the factor's
    // level STRINGS are no longer inlined into the varlist card. Every level
    // was duplicated into every varlist_after on every mutation — on the
    // incident run that was obs_id's 108,384 strings ≈ 2.3 MB PER mutation ×
    // 2,742 mutations. n_levels (the count) stays; the frontend fetches the
    // actual levels lazily via the EXISTING GET /api/var_detail route (2.4.0).
    // Only THIS JSON emission thins — the C++ `levels` vector is untouched and
    // still read directly by var_detail and the value-label rendering paths
    // (SessionState::varlistWithMeta's label-def remap, Datastore::browse).
  }
  j["n_missing"] = nMissing;
  return j;
}

nlohmann::json BrowseCell::toJson() const {
  switch (kind) {
    case Kind::Null:
      return nullptr;
    case Kind::Number:
      return number;
    case Kind::String:
      return text;
  }
  return nullptr;
}

Datastore::Datastore(const std::string& path) : path_(path), ds_(cecon::Dataset::open(path)) {
  finishConstruct();
}

// ALLOC-C3A-ARC1-CARRY: same Datastore, opened by carrying the previous frame's
// level dictionaries instead of rebuilding them. Identical afterwards — the
// carry either produced the same columns a full parse would have, or fell back
// to a full parse and produced them literally.
// ALLOC-C3A-ARC2A-PREVMAP added `src`: a pass-through of the previous frame's
// live mapping, which this Datastore never retains (the returned Dataset points
// only into its OWN mapping — see CarrySource's lifetime note).
Datastore::Datastore(const std::string& path, std::vector<cecon::Column>&& carried,
                     const cecon::CarryDelta& delta, cecon::CarryOutcome* why,
                     const cecon::CarrySource& src)
    : path_(path), ds_(cecon::Dataset::open_carried(path, std::move(carried), delta, why, src)) {
  finishConstruct();
}

void Datastore::finishConstruct() {
  ++constructCount_;  // selftest instrumentation: see constructCount() in datastore.h.
  // FLOOR ① site class COLUMNAR (counters.h). The `use` anchor: one open of an
  // N-byte frame is exactly the semantic work, so a single load reads amp ≡ 1
  // and a RE-open of the same frame reads amp = 2 — which is the defect ① is
  // pointed at (per-depvar re-opens, the S2 shape). Billed at the file's size,
  // the same quantity semantic_use() uses as its denominator (A1 §1.2); the
  // provenance re-hash is engine-side and deliberately NOT billed here (a
  // metric that reds on a ratified-unfixable cost teaches waiving — A1 §1.3).
  {
    std::error_code ec;
    auto sz = std::filesystem::file_size(cecon::to_native_path(path_), ec);
    if (!ec) counters::addColumnarBytes(static_cast<int64_t>(sz));
  }
  names_.reserve(ds_.columns().size());
  for (const auto& c : ds_.columns()) {
    if (!isHiddenColumn(c.name)) names_.push_back(c.name);  // e(sample) T2 (datastore.h)
  }
}


std::string Datastore::dtype(const std::string& name) const {
  return ds_.col(name).dtype == cecon::DType::Factor ? "factor" : "f64";
}

int64_t Datastore::nMissing(const std::string& name) const {
  // C2-F13a: an unchanged column's missing-count was carried from the prior
  // state (pure-add mutation) — return it and skip the full scan + its
  // whole-column mmap fault-in. Seeded only for cols cobra left byte-identical.
  if (auto it = nMissSeed_.find(name); it != nMissSeed_.end()) return it->second;
  const cecon::Column& c = ds_.col(name);
  // NMISS-CARRY (2026-08-20). The seed above only ever covers columns the engine
  // left byte-identical, so a JOIN — which produces no byte-identical column at
  // all — missed it entirely and paid a full scan per column, every time a card
  // was rebuilt from the file. After a stateless joinby/merge there is no delta
  // to fold, so that rebuild is unavoidable and the scan was its dominant cost:
  // 45.4 s, 24% of the acceptance corpus's wall (PIET_EMIT_TIMING=4, 2026-08-20).
  //
  // The header now states the count the engine already had when it wrote the
  // file. -1 means the key was ABSENT (every .hbin written before this key, and
  // every writer that does not state it, including patch_hbin's appended
  // columns) — and absent means count it yourself, exactly as before. A count of
  // zero is a real answer and is trusted; only -1 falls through.
  // PIET_NO_NMISS_CARRY: disable the carry and take the scan path. Two users.
  // (1) A/B measurement — the carry's whole claim is that it removes a scan, and
  // that is only checkable against an arm where the scan still happens.
  // (2) Gates whose DISCRIMINATOR is the scan count. The card-tax gate asserts
  // fullScanCount()==1 to prove seedNMissForPureAdd's branch 2 fired; with the
  // carry live that count is 0 whether or not the branch fires, so the gate
  // would still pass while blind. Rather than relax its assertion — which is how
  // a gate keeps its green and loses its teeth ([[gate-fix-can-trade-blindness]])
  // — the gate sets this and keeps measuring exactly what it always measured.
  // Read FRESH each call, not memoized into a static: the card-tax gate flips it
  // mid-process and a one-shot read would freeze whichever value the first
  // nMissing() in the process happened to see. Same convention, for the same
  // reason, as scratchBudgetBytes() in session_state.cpp. A getenv costs
  // nanoseconds against the O(rows) scan this branch exists to skip.
  const char* carryOffEnv = std::getenv("PIET_NO_NMISS_CARRY");
  const bool carryOff = carryOffEnv != nullptr && *carryOffEnv != '\0';
  if (!carryOff && c.n_missing >= 0) {
    // PIET_VERIFY_NMISS: the falsification arm. A carried number that is WRONG is
    // a silent wrong answer in the variable panel — the exact class this codebase
    // keeps paying for — and "the writer computes it correctly" is an assumption
    // until something checks it. Under this env var the reader does BOTH: takes
    // the carried value and scans anyway, and aborts on disagreement, naming the
    // column and both numbers. Off by default (the scan is the cost the carry
    // exists to remove); on, it turns any corpus run into a per-column audit of
    // the writer. Run it over the corpus before trusting the field.
    if (const char* v = std::getenv("PIET_VERIFY_NMISS"); v && *v) {
      const int64_t scanned = scanMissing(name);
      if (scanned != c.n_missing) {
        std::fprintf(stderr,
                     "[piet_native] FATAL NMISS-CARRY MISMATCH: column '%s' header says %lld, "
                     "scan says %lld — the header is lying and every card built from this file "
                     "carries a wrong missing count.\n",
                     name.c_str(), static_cast<long long>(c.n_missing),
                     static_cast<long long>(scanned));
        std::abort();
      }
    }
    return c.n_missing;
  }
  ++fullScanCount_;  // selftest instrumentation: this is the expensive real-scan path.
  return scanMissing(name);
}

// The counting loop itself, factored out so the carry's verification arm and the
// fallback share ONE implementation — a verifier carrying its own copy of the
// arithmetic can agree with itself while both disagree with the engine.
int64_t Datastore::scanMissing(const std::string& name) const {
  const cecon::Column& c = ds_.col(name);
  int64_t n = 0;
  if (c.dtype == cecon::DType::F64) {
    const double* d = c.f64();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
    for (int64_t i = 0; i < count; ++i) {
      if (std::isnan(d[i])) ++n;
    }
  } else {
    const int32_t* d = c.codes();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(int32_t));
    for (int64_t i = 0; i < count; ++i) {
      if (d[i] < 0) ++n;
    }
  }
  return n;
}

VarInfo Datastore::buildVarInfo(const std::string& name) const {
  const cecon::Column& c = ds_.col(name);
  VarInfo info;
  info.name = name;
  info.dtype = (c.dtype == cecon::DType::Factor) ? "factor" : "f64";
  info.nMissing = nMissing(name);
  // P1-A0B-FMT-PIET-CONSUME: the hbin header's per-column display format
  // (P1-A0-FORMAT-CARRIAGE) reaches the reader here. Empty c.format means the
  // key was absent (pre-A0 file or a format-less writer) — leave VarInfo's
  // own "%9.0g" member-initializer default in place rather than overwrite it
  // with "" (A0 law: "" = unknown, never a fabricated value).
  if (!c.format.empty()) {
    info.format = c.format;
    info.formatStated = true;
  }
  // BUG-198: the header's optional per-column `label` key reaches the card the
  // same way. VarInfo::label has NO default (unlike format's "%9.0g"), so an
  // absent key and an unstated label are the same empty string here — which is
  // exactly right: nothing downstream may invent a variable label.
  info.label = c.label;
  if (c.dtype == cecon::DType::Factor) {
    // A12/FLOOR: a non-owning VIEW into the column's own level dictionary, NOT
    // a deep copy. The card emits only n_levels (F5); the strings are read
    // lazily by var_detail / value-label rendering, all against this live
    // Datastore (see LevelsRef in datastore.h). Reintroducing `info.levels =
    // c.levels` here trips the levelStringCopies() selftest gate.
    info.levels = LevelsRef::view(&c.levels);
    info.nLevels = static_cast<int64_t>(c.levels.size());
  }
  return info;
}

const std::vector<VarInfo>& Datastore::varlist() {
  if (!varinfoCache_.has_value()) {
    std::vector<VarInfo> v;
    v.reserve(names_.size());
    for (const auto& n : names_) v.push_back(buildVarInfo(n));
    varinfoCache_ = std::move(v);
  }
  return *varinfoCache_;
}

VarInfo Datastore::varinfo(const std::string& name) {
  for (const auto& v : varlist()) {
    if (v.name == name) return v;
  }
  throw std::runtime_error("variable not found: " + name);
}

std::vector<VarInfo> Datastore::formatCard(
    const std::optional<std::vector<std::string>>& vars) const {
  std::vector<VarInfo> out;
  auto add = [&](const std::string& name) {
    if (!ds_.has(name)) return;
    const cecon::Column& c = ds_.col(name);
    VarInfo info;
    info.name = name;
    info.dtype = (c.dtype == cecon::DType::Factor) ? "factor" : "f64";
    if (!c.format.empty()) {  // buildVarInfo's rule: "" = the file stated nothing
      info.format = c.format;
      info.formatStated = true;
    }
    out.push_back(std::move(info));
  };
  if (vars.has_value()) {
    for (const auto& n : *vars) add(n);
  } else {
    for (const auto& n : names_) add(n);
  }
  return out;
}

nlohmann::ordered_json Datastore::browse(
    int64_t offset, int64_t limit, const std::optional<std::vector<std::string>>& vars,
    const std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>>&
        valueLabelMap,
    const std::vector<std::pair<std::string, std::string>>& formatMap) const {
  offset = std::max<int64_t>(0, offset);
  limit = std::max<int64_t>(0, limit);
  int64_t stop = offset + limit;

  // value_label_map lookup helper (varname -> vector<{code,text}>).
  auto lookupVL = [&](const std::string& name) -> const std::vector<std::pair<int, std::string>>* {
    for (const auto& [k, v] : valueLabelMap) {
      if (k == name) return &v;
    }
    return nullptr;
  };

  // format_map lookup helper (varname -> %fmt string): the caller's resolved
  // formats (SessionState::displayFormatMap); nullptr for a variable the map
  // does not name, which falls back to the file's own format below and never
  // to a fabricated default like VarInfo's own "%9.0g" member-initializer.
  auto lookupFmt = [&](const std::string& name) -> const std::string* {
    for (const auto& [k, v] : formatMap) {
      if (k == name) return &v;
    }
    return nullptr;
  };

  std::vector<std::pair<int, std::string>> selected;  // (idx, name)
  for (size_t idx = 0; idx < names_.size(); ++idx) {
    const std::string& name = names_[idx];
    if (!vars.has_value() ||
        std::find(vars->begin(), vars->end(), name) != vars->end()) {
      selected.emplace_back(static_cast<int>(idx), name);
    }
  }
  if (vars.has_value()) {
    std::map<std::string, size_t> order;
    for (size_t i = 0; i < vars->size(); ++i) order[(*vars)[i]] = i;
    std::stable_sort(selected.begin(), selected.end(), [&](const auto& a, const auto& b) {
      size_t oa = order.count(a.second) ? order[a.second] : vars->size();
      size_t ob = order.count(b.second) ? order[b.second] : vars->size();
      return oa < ob;
    });
  }

  nlohmann::ordered_json columnsMeta = nlohmann::ordered_json::array();
  int64_t nWindow = 0;

  struct ColData {
    const cecon::Column* col;
    int64_t winStart, winStop;
    const std::vector<std::pair<int, std::string>>* vl;
  };
  std::vector<ColData> colData;

  for (const auto& [idx, name] : selected) {
    const cecon::Column& c = ds_.col(name);
    nlohmann::ordered_json cm;
    cm["name"] = name;
    cm["dtype"] = (c.dtype == cecon::DType::Factor) ? "factor" : "f64";
    cm["index"] = idx;
    if (c.dtype == cecon::DType::Factor) cm["levels"] = c.levels;
    const auto* vl = lookupVL(name);
    if (vl && !vl->empty()) {
      nlohmann::ordered_json vlj = nlohmann::ordered_json::object();
      for (const auto& [code, text] : *vl) vlj[std::to_string(code)] = text;
      cm["value_labels"] = vlj;
    }
    // P1-A0B-FMT-PIET-CONSUME: fallback order session > file > none. A user's
    // just-issued `format VAR %fmt` (formatMap) must not lose to the file's
    // own value; c.format is the file-derived fallback when no session
    // override exists; "" (either side) still never writes the key.
    const std::string* fmt = lookupFmt(name);
    if (fmt && !fmt->empty()) {
      cm["format"] = *fmt;
    } else if (!c.format.empty()) {
      cm["format"] = c.format;
    }
    columnsMeta.push_back(cm);

    int64_t nRows = ds_.n_rows();
    int64_t winStart = std::min(offset, nRows);
    int64_t winStop = std::min(stop, nRows);
    colData.push_back(ColData{&c, winStart, winStop, (vl && !vl->empty()) ? vl : nullptr});
    nWindow = std::max<int64_t>(nWindow, winStop - winStart);
  }

  nlohmann::ordered_json rows = nlohmann::ordered_json::array();
  for (int64_t r = 0; r < nWindow; ++r) {
    nlohmann::ordered_json row = nlohmann::ordered_json::array();
    for (const auto& cd : colData) {
      int64_t i = cd.winStart + r;
      if (i >= cd.winStop) {
        row.push_back(nullptr);
        continue;
      }
      if (cd.col->dtype == cecon::DType::F64) {
        double v = cd.col->f64()[i];
        if (std::isnan(v)) {
          row.push_back(nullptr);
        } else if (cd.vl) {
          int code = static_cast<int>(v);
          const std::string* text = nullptr;
          for (const auto& [c2, t2] : *cd.vl) {
            if (c2 == code) {
              text = &t2;
              break;
            }
          }
          if (text) {
            row.push_back(*text);
          } else {
            row.push_back(v);
          }
        } else {
          row.push_back(v);
        }
      } else {
        int32_t code = cd.col->codes()[i];
        if (code >= 0 && static_cast<size_t>(code) < cd.col->levels.size()) {
          row.push_back(cd.col->levels[code]);
        } else {
          row.push_back(nullptr);
        }
      }
    }
    rows.push_back(row);
  }

  nlohmann::ordered_json out;
  out["type"] = "browse";
  out["offset"] = offset;
  out["limit"] = limit;
  out["n_rows_total"] = ds_.n_rows();
  out["columns"] = columnsMeta;
  out["rows"] = rows;
  return out;
}

}  // namespace piet
