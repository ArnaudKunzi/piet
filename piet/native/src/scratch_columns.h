// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Short-lived helper columns for a handler that wants an `if`, an `in` or an
// expression evaluated exactly as the user's own `generate` would evaluate it.
// Every column added here is dropped again, on every way out of the handler, and
// c(changed) goes back to what it was unless the handler says it changed the data.
#ifndef PIET_NATIVE_SCRATCH_COLUMNS_H
#define PIET_NATIVE_SCRATCH_COLUMNS_H

#include <optional>
#include <string>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

class ScratchColumns {
 public:
  explicit ScratchColumns(SessionState& session)
      : session_(session), changedOnEntry_(session.datasetChanged) {}
  ScratchColumns(const ScratchColumns&) = delete;
  ScratchColumns& operator=(const ScratchColumns&) = delete;

  ~ScratchColumns() {
    try {
      dropAll();
    } catch (...) {  // a destructor runs during error returns; the refusal stands
    }
    session_.datasetChanged = dataChanged_ || changedOnEntry_;
  }

  // Adds `generate double <new> = <rhs><qualifier>` and names the new column in
  // *name. `qualifier` is "" or starts with a blank. The error block is generate's
  // own, so a bad name, a type mismatch or a bad range answer as generate does.
  std::optional<nlohmann::ordered_json> add(const std::string& rhs, const std::string& qualifier,
                                            std::string* name) {
    *name = freshName();
    const auto blocks =
        dispatch("quietly generate double " + *name + " = " + rhs + qualifier, session_);
    for (const auto& block : blocks)
      if (block.is_object() && block.value("type", std::string()) == "error") return block;
    held_.push_back(*name);
    return std::nullopt;
  }

  // The column as one double per observation (a missing value is a NaN).
  std::vector<double> values(const std::string& name) const {
    const Datastore* data = session_.datastore();
    const cecon::Column& column = data->col(name);
    const double* cells = column.f64();
    return std::vector<double>(cells, cells + data->nRows());
  }

  void dropAll() {
    if (held_.empty()) return;
    std::string names;
    for (const std::string& held : held_) names += " " + held;
    held_.clear();
    SessionState::TempDropScope quietDrop(session_);
    (void)dispatch("quietly drop" + names, session_);
  }

  void noteDataChanged() { dataChanged_ = true; }

 private:
  std::string freshName() {
    const Datastore* data = session_.datastore();
    std::string name;
    do {
      name = "__scratch" + std::to_string(++serial_);
    } while (data != nullptr && data->has(name));
    return name;
  }

  SessionState& session_;
  const bool changedOnEntry_;
  bool dataChanged_ = false;
  int serial_ = 0;
  std::vector<std::string> held_;
};

}  // namespace piet::handlers

#endif  // PIET_NATIVE_SCRATCH_COLUMNS_H
