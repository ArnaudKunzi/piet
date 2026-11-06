// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// zip_io.h -- the ZIP container primitives xlsx.cpp has carried since the .xlsx
// reader/writer landed, exposed for the AER tier-1 zip verbs (unzipfile, zipfile,
// zipuse, zipsave). The bodies stay in xlsx.cpp: one INFLATE, one central-directory
// walk and one STORED writer serve both callers, so the two cannot drift.
//
// Limits, all inherited and all loud: a ZIP64 archive, an encrypted entry, or a
// compression method other than stored (0) / deflated (8) throws. The writer stores
// only (no DEFLATE), stamps every entry 1980-01-01 00:00 so its bytes are
// deterministic, and keeps 16-bit entry counts and 32-bit sizes -- a caller must
// refuse more than 65535 entries or an entry of 4 GiB or more before calling build().
#pragma once

#include <string>
#include <vector>

namespace piet::zipio {

struct Entry {
  std::string name;  // as stored in the archive; a directory entry ends in '/'
  std::string data;  // uncompressed bytes (empty for a directory entry)
};

// Every entry in central-directory order. Throws std::runtime_error when the file
// cannot be read or is not a ZIP, and for the unsupported cases above.
std::vector<Entry> readEntries(const std::string& path);

// The archive bytes for `entries`, stored, in the given order.
std::string build(const std::vector<Entry>& entries);

// Write `bytes` to `path` through a sibling temp file and rename(2); an existing
// file is never truncated by a failed write. Throws std::runtime_error.
void commit(const std::string& path, const std::string& bytes);

}  // namespace piet::zipio
