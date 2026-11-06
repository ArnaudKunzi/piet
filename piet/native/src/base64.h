// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Minimal base64 encoder — used only to build a `data:text/html;base64,...`
// URI for loading the demo page without a percent-encoding pass (see
// main.cpp). Not a vendored dependency: ~20 lines, kept in-tree rather than
// pulling in a library for one function.
#ifndef PIET_NATIVE_BASE64_H
#define PIET_NATIVE_BASE64_H

#include <string>

namespace piet {

inline std::string base64Encode(const std::string& in) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= in.size()) {
    unsigned int n = (static_cast<unsigned char>(in[i]) << 16) |
                      (static_cast<unsigned char>(in[i + 1]) << 8) |
                      static_cast<unsigned char>(in[i + 2]);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back(kTable[n & 0x3F]);
    i += 3;
  }

  size_t remaining = in.size() - i;
  if (remaining == 1) {
    unsigned int n = static_cast<unsigned char>(in[i]) << 16;
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    unsigned int n = (static_cast<unsigned char>(in[i]) << 16) |
                      (static_cast<unsigned char>(in[i + 1]) << 8);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back('=');
  }

  return out;
}

// N1 (P8.1-W2): decode standard base64 (with optional `=` padding; ASCII
// whitespace/newlines a data-URI or line-wrapped payload may carry are
// skipped). Returns false — leaving `out` in an unspecified partial state —
// on an invalid character or data appearing after padding, so a corrupt
// payload becomes an HONEST write error at the caller (file.save) rather than
// silently-wrong bytes on disk. Binary-safe (embedded NULs and high bytes
// round-trip). Counterpart to base64Encode above; used by POST /api/file/save
// (content_b64 branch) to write PNG/SVG exports the frontend hands over
// base64-encoded.
inline bool base64Decode(const std::string& in, std::string& out) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out.clear();
  out.reserve(in.size() / 4 * 3);
  int buf = 0;
  int bits = 0;
  bool sawPad = false;
  for (unsigned char c : in) {
    if (c == '=') {
      sawPad = true;
      continue;
    }
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    if (sawPad) return false;  // real data after padding — malformed
    int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
  }
  return true;
}

}  // namespace piet

#endif  // PIET_NATIVE_BASE64_H
