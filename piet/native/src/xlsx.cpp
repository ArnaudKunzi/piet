// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "xlsx.h"
#include "zip_io.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <utility>

#include "piet/eval/pyvalue.h"  // pythonFloatRepr — shortest round-trip decimal
#include "io/win_compat.h"      // cecon::to_native_path

namespace piet::xlsx {
namespace {

// ---------------------------------------------------------------------------
// CRC-32 (ZIP's, the reflected IEEE polynomial 0xEDB88320).

uint32_t crc32_of(const std::string& data) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// RFC 1951 INFLATE. Implemented from the specification: a bit reader (LSB
// first), canonical Huffman decoding built from code lengths, and the three
// block types (stored / fixed / dynamic).

struct BitReader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;   // byte position
  uint32_t bitbuf = 0;
  int bitcnt = 0;

  BitReader(const uint8_t* d, size_t len) : p(d), n(len) {}

  int bit() {
    if (bitcnt == 0) {
      if (pos >= n) throw std::runtime_error("xlsx: truncated compressed stream");
      bitbuf = p[pos++];
      bitcnt = 8;
    }
    int b = bitbuf & 1;
    bitbuf >>= 1;
    --bitcnt;
    return b;
  }
  uint32_t bits(int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; ++i) v |= static_cast<uint32_t>(bit()) << i;
    return v;
  }
  void alignByte() { bitcnt = 0; }
};

// Canonical Huffman decoder: counts per length + symbols in code order.
struct Huffman {
  std::vector<int> counts;   // counts[len]
  std::vector<int> symbols;  // symbols ordered by (len, symbol)

  void build(const std::vector<int>& lengths, int maxLen) {
    counts.assign(static_cast<size_t>(maxLen) + 1, 0);
    for (int l : lengths)
      if (l > 0) ++counts[static_cast<size_t>(l)];
    std::vector<int> offs(static_cast<size_t>(maxLen) + 2, 0);
    for (int l = 1; l <= maxLen; ++l) offs[static_cast<size_t>(l) + 1] =
        offs[static_cast<size_t>(l)] + counts[static_cast<size_t>(l)];
    symbols.assign(static_cast<size_t>(offs[static_cast<size_t>(maxLen) + 1]), 0);
    for (size_t s = 0; s < lengths.size(); ++s) {
      if (lengths[s] > 0) symbols[static_cast<size_t>(offs[static_cast<size_t>(lengths[s])]++)] =
          static_cast<int>(s);
    }
  }

  int decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (size_t len = 1; len < counts.size(); ++len) {
      code |= br.bit();
      int count = counts[len];
      if (code - first < count) return symbols[static_cast<size_t>(index + (code - first))];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw std::runtime_error("xlsx: bad Huffman code in compressed stream");
  }
};

const int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                           33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                           1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                            6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

// `expected` is the entry's declared uncompressed size, and the output may not
// grow past it: a stream that would is corrupt or a decompression bomb, and is
// refused before it can exhaust memory (AER tier-1 review of unzipfile).
std::string inflate(const uint8_t* data, size_t len, size_t expected) {
  BitReader br(data, len);
  std::string out;
  if (expected) out.reserve(expected);
  auto overrun = [&](size_t add) {
    if (out.size() + add > expected)
      throw std::runtime_error("compressed entry inflates past its declared size");
  };

  Huffman fixedLit, fixedDist;
  {
    std::vector<int> l(288);
    for (int i = 0; i < 144; ++i) l[static_cast<size_t>(i)] = 8;
    for (int i = 144; i < 256; ++i) l[static_cast<size_t>(i)] = 9;
    for (int i = 256; i < 280; ++i) l[static_cast<size_t>(i)] = 7;
    for (int i = 280; i < 288; ++i) l[static_cast<size_t>(i)] = 8;
    fixedLit.build(l, 9);
    std::vector<int> d(30, 5);
    fixedDist.build(d, 5);
  }

  for (;;) {
    int last = br.bit();
    int type = static_cast<int>(br.bits(2));
    if (type == 0) {  // stored
      br.alignByte();
      if (br.pos + 4 > br.n) throw std::runtime_error("xlsx: truncated stored block");
      uint16_t blockLen = static_cast<uint16_t>(br.p[br.pos] | (br.p[br.pos + 1] << 8));
      br.pos += 4;  // LEN + NLEN
      if (br.pos + blockLen > br.n) throw std::runtime_error("xlsx: truncated stored block");
      overrun(blockLen);
      out.append(reinterpret_cast<const char*>(br.p + br.pos), blockLen);
      br.pos += blockLen;
    } else if (type == 1 || type == 2) {
      const Huffman* lit = &fixedLit;
      const Huffman* dist = &fixedDist;
      Huffman dynLit, dynDist;
      if (type == 2) {
        int hlit = static_cast<int>(br.bits(5)) + 257;
        int hdist = static_cast<int>(br.bits(5)) + 1;
        int hclen = static_cast<int>(br.bits(4)) + 4;
        static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
        std::vector<int> clen(19, 0);
        for (int i = 0; i < hclen; ++i) clen[static_cast<size_t>(kOrder[i])] =
            static_cast<int>(br.bits(3));
        Huffman clHuff;
        clHuff.build(clen, 7);
        std::vector<int> lengths;
        lengths.reserve(static_cast<size_t>(hlit + hdist));
        while (static_cast<int>(lengths.size()) < hlit + hdist) {
          int sym = clHuff.decode(br);
          if (sym < 16) {
            lengths.push_back(sym);
          } else if (sym == 16) {
            if (lengths.empty()) throw std::runtime_error("xlsx: bad code-length repeat");
            int prev = lengths.back();
            int rep = 3 + static_cast<int>(br.bits(2));
            for (int i = 0; i < rep; ++i) lengths.push_back(prev);
          } else if (sym == 17) {
            int rep = 3 + static_cast<int>(br.bits(3));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
          } else {
            int rep = 11 + static_cast<int>(br.bits(7));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
          }
        }
        std::vector<int> litLens(lengths.begin(), lengths.begin() + hlit);
        std::vector<int> distLens(lengths.begin() + hlit, lengths.begin() + hlit + hdist);
        dynLit.build(litLens, 15);
        dynDist.build(distLens, 15);
        lit = &dynLit;
        dist = &dynDist;
      }
      for (;;) {
        int sym = lit->decode(br);
        if (sym < 256) {
          overrun(1);
          out.push_back(static_cast<char>(sym));
        } else if (sym == 256) {
          break;
        } else {
          sym -= 257;
          if (sym >= 29) throw std::runtime_error("xlsx: bad length symbol");
          int length = kLenBase[sym] + static_cast<int>(br.bits(kLenExtra[sym]));
          int dsym = dist->decode(br);
          if (dsym >= 30) throw std::runtime_error("xlsx: bad distance symbol");
          size_t distance = static_cast<size_t>(kDistBase[dsym]) +
                            static_cast<size_t>(br.bits(kDistExtra[dsym]));
          if (distance > out.size()) throw std::runtime_error("xlsx: distance past start");
          size_t start = out.size() - distance;
          overrun(static_cast<size_t>(length));
          for (int i = 0; i < length; ++i) out.push_back(out[start + static_cast<size_t>(i)]);
        }
      }
    } else {
      throw std::runtime_error("xlsx: reserved deflate block type");
    }
    if (last) break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// ZIP reading: walk the central directory, then decompress the named entries.

uint16_t rd16(const std::string& b, size_t o) {
  return static_cast<uint16_t>(static_cast<unsigned char>(b[o]) |
                               (static_cast<unsigned char>(b[o + 1]) << 8));
}
uint32_t rd32(const std::string& b, size_t o) {
  return static_cast<uint32_t>(static_cast<unsigned char>(b[o])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[o + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[o + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[o + 3])) << 24);
}

// Every entry in central-directory order. `kind` only names the file in the
// two not-a-ZIP messages (".xlsx" for the workbook reader, ".zip" for
// piet::zipio), so the workbook reader's messages are the ones it always had.
std::vector<std::pair<std::string, std::string>> readZipOrdered(const std::string& path,
                                                                const char* kind) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (buf.size() < 22)
    throw std::runtime_error(path + " is not a " + kind + " file (too small)");

  // End of central directory: scan back for the signature.
  size_t eocd = std::string::npos;
  size_t scanStart = buf.size() >= 66000 ? buf.size() - 66000 : 0;
  for (size_t i = buf.size() - 22 + 1; i-- > scanStart;) {
    if (rd32(buf, i) == 0x06054b50u) { eocd = i; break; }
  }
  if (eocd == std::string::npos)
    throw std::runtime_error(path + " is not a " + kind + " file (no ZIP directory found)");

  uint16_t nEntries = rd16(buf, eocd + 10);
  uint32_t cdOff = rd32(buf, eocd + 16);
  // AER tier-1 review: a ZIP64 archive saturates these fields (65,535 entries,
  // 0xFFFFFFFF offsets) and keeps the real values in records this reader does
  // not walk; reading the saturated ones silently returned a partial archive.
  if (nEntries == 0xFFFFu || cdOff == 0xFFFFFFFFu)
    throw std::runtime_error(path + " is a ZIP64 archive — not supported");

  std::vector<std::pair<std::string, std::string>> parts;
  size_t p = cdOff;
  for (uint16_t e = 0; e < nEntries; ++e) {
    // A damaged directory or local header used to `break`/`continue` and the
    // archive read as a smaller one at rc 0 (StataBE: r(603) "invalid LOC
    // header"). Every one of these now throws.
    if (p + 46 > buf.size() || rd32(buf, p) != 0x02014b50u)
      throw std::runtime_error(path + ": damaged ZIP central directory");
    uint16_t method = rd16(buf, p + 10);
    uint32_t compSize = rd32(buf, p + 20);
    uint32_t rawSize = rd32(buf, p + 24);
    uint16_t nameLen = rd16(buf, p + 28);
    uint16_t extraLen = rd16(buf, p + 30);
    uint16_t commentLen = rd16(buf, p + 32);
    uint16_t flags = rd16(buf, p + 8);
    uint32_t localOff = rd32(buf, p + 42);
    std::string name = buf.substr(p + 46, nameLen);
    p += 46u + nameLen + extraLen + commentLen;

    if (flags & 0x1) throw std::runtime_error(path + " is encrypted — not supported");
    if (compSize == 0xFFFFFFFFu || rawSize == 0xFFFFFFFFu || localOff == 0xFFFFFFFFu)
      throw std::runtime_error(path + " is a ZIP64 archive — not supported");
    // size_t arithmetic: in uint32 a local offset near 4 GiB wrapped past the
    // bounds check and the read below left the buffer (a crash, AER tier-1 review).
    if (static_cast<size_t>(localOff) + 30 > buf.size() || rd32(buf, localOff) != 0x04034b50u)
      throw std::runtime_error(path + ": invalid ZIP local header for " + name);
    uint16_t lNameLen = rd16(buf, localOff + 26);
    uint16_t lExtraLen = rd16(buf, localOff + 28);
    size_t dataOff = static_cast<size_t>(localOff) + 30u + lNameLen + lExtraLen;
    if (dataOff + compSize > buf.size())
      throw std::runtime_error(path + ": truncated ZIP entry " + name);

    if (method == 0) {
      parts.emplace_back(name, buf.substr(dataOff, compSize));
    } else if (method == 8) {
      parts.emplace_back(name, inflate(reinterpret_cast<const uint8_t*>(buf.data()) + dataOff,
                                       compSize, rawSize));
    } else {
      throw std::runtime_error(path + ": ZIP compression method " + std::to_string(method) +
                               " is not supported (this build reads stored and deflated entries)");
    }
  }
  return parts;
}

// The workbook reader's view: by name, a later duplicate entry winning, exactly
// as when this function filled the map itself.
std::map<std::string, std::string> readZip(const std::string& path) {
  std::map<std::string, std::string> parts;
  for (auto& [name, data] : readZipOrdered(path, ".xlsx")) parts[name] = std::move(data);
  return parts;
}

// ---------------------------------------------------------------------------
// Tiny XML scanning. Deliberately NOT a regex: sheet XML is machine-generated
// and can be large, and we only need attribute lookup + element text.

std::string attr(const std::string& tag, const std::string& name) {
  std::string key = name + "=\"";
  size_t at = tag.find(key);
  if (at == std::string::npos) return std::string();
  at += key.size();
  size_t end = tag.find('"', at);
  if (end == std::string::npos) return std::string();
  return tag.substr(at, end - at);
}

std::string unescapeXml(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') { out.push_back(s[i]); continue; }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) { out.push_back(s[i]); continue; }
    std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") out.push_back('&');
    else if (ent == "lt") out.push_back('<');
    else if (ent == "gt") out.push_back('>');
    else if (ent == "quot") out.push_back('"');
    else if (ent == "apos") out.push_back('\'');
    else if (!ent.empty() && ent[0] == '#') {
      int code = 0;
      try {
        code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                   ? std::stoi(ent.substr(2), nullptr, 16)
                   : std::stoi(ent.substr(1));
      } catch (...) { code = 0; }
      if (code > 0 && code < 0x80) out.push_back(static_cast<char>(code));
      else if (code >= 0x80) {  // minimal UTF-8 encode
        if (code < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (code >> 6)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | (code >> 12)));
          out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
      }
    } else {
      out.push_back('&');
      out += ent;
      out.push_back(';');
    }
    i = semi;
  }
  return out;
}

// Concatenate every <t>…</t> inside `xml` starting at `from` up to `to`.
std::string collectText(const std::string& xml, size_t from, size_t to) {
  std::string out;
  size_t p = from;
  while (p < to) {
    size_t open = xml.find("<t", p);
    if (open == std::string::npos || open >= to) break;
    size_t gt = xml.find('>', open);
    if (gt == std::string::npos || gt >= to) break;
    if (xml[gt - 1] == '/') { p = gt + 1; continue; }  // <t/> — empty
    size_t close = xml.find("</t>", gt);
    if (close == std::string::npos || close > to) break;
    out += unescapeXml(xml.substr(gt + 1, close - gt - 1));
    p = close + 4;
  }
  return out;
}

std::vector<std::string> parseSharedStrings(const std::string& xml) {
  std::vector<std::string> out;
  size_t p = 0;
  while (true) {
    size_t si = xml.find("<si", p);
    if (si == std::string::npos) break;
    size_t end = xml.find("</si>", si);
    if (end == std::string::npos) break;
    out.push_back(collectText(xml, si, end));
    p = end + 5;
  }
  return out;
}

// "BC12" -> 0-based column 54. Ignores the row part.
int colIndexFromRef(const std::string& ref) {
  int col = 0;
  for (char c : ref) {
    if (c >= 'A' && c <= 'Z') col = col * 26 + (c - 'A' + 1);
    else if (c >= 'a' && c <= 'z') col = col * 26 + (c - 'a' + 1);
    else break;
  }
  return col > 0 ? col - 1 : 0;
}

std::string colRef(int idx0) {  // 0 -> "A"
  std::string s;
  int n = idx0 + 1;
  while (n > 0) {
    int rem = (n - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return s;
}

std::string escapeXml(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// ZIP writing (STORE only — legal and universally readable).

void appendLE16(std::string& b, uint16_t v) {
  b.push_back(static_cast<char>(v & 0xFF));
  b.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void appendLE32(std::string& b, uint32_t v) {
  b.push_back(static_cast<char>(v & 0xFF));
  b.push_back(static_cast<char>((v >> 8) & 0xFF));
  b.push_back(static_cast<char>((v >> 16) & 0xFF));
  b.push_back(static_cast<char>((v >> 24) & 0xFF));
}

struct ZipEntry {
  std::string name;
  std::string data;
};

std::string buildZip(const std::vector<ZipEntry>& entries) {
  std::string out;
  struct Rec { uint32_t crc; uint32_t size; uint32_t off; };
  std::vector<Rec> recs;
  recs.reserve(entries.size());

  for (const auto& e : entries) {
    Rec r;
    r.off = static_cast<uint32_t>(out.size());
    r.crc = crc32_of(e.data);
    r.size = static_cast<uint32_t>(e.data.size());
    appendLE32(out, 0x04034b50u);
    appendLE16(out, 20);        // version needed
    appendLE16(out, 0);         // flags
    appendLE16(out, 0);         // method: stored
    appendLE16(out, 0);         // mod time (fixed — output is byte-deterministic)
    appendLE16(out, 0x21);      // mod date: 1980-01-01
    appendLE32(out, r.crc);
    appendLE32(out, r.size);
    appendLE32(out, r.size);
    appendLE16(out, static_cast<uint16_t>(e.name.size()));
    appendLE16(out, 0);         // extra len
    out += e.name;
    out += e.data;
    recs.push_back(r);
  }

  uint32_t cdOff = static_cast<uint32_t>(out.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    appendLE32(out, 0x02014b50u);
    appendLE16(out, 20);        // version made by
    appendLE16(out, 20);        // version needed
    appendLE16(out, 0);
    appendLE16(out, 0);         // stored
    appendLE16(out, 0);
    appendLE16(out, 0x21);
    appendLE32(out, recs[i].crc);
    appendLE32(out, recs[i].size);
    appendLE32(out, recs[i].size);
    appendLE16(out, static_cast<uint16_t>(entries[i].name.size()));
    appendLE16(out, 0);         // extra
    appendLE16(out, 0);         // comment
    appendLE16(out, 0);         // disk
    appendLE16(out, 0);         // internal attrs
    appendLE32(out, 0);         // external attrs
    appendLE32(out, recs[i].off);
    out += entries[i].name;
  }
  uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOff;

  appendLE32(out, 0x06054b50u);
  appendLE16(out, 0);
  appendLE16(out, 0);
  appendLE16(out, static_cast<uint16_t>(entries.size()));
  appendLE16(out, static_cast<uint16_t>(entries.size()));
  appendLE32(out, cdSize);
  appendLE32(out, cdOff);
  appendLE16(out, 0);
  return out;
}

// ---------------------------------------------------------------------------
// AER scope-2 W3a: THE WORKBOOK, not the ZIP directory.
//
// Until now this reader located "the worksheet" by trying
// `xl/worksheets/sheet1.xml` and then any `xl/worksheets/*.xml`, and never
// opened `xl/workbook.xml` at all. Two facts make that wrong rather than
// merely approximate:
//
//   * the sheet's NAME exists only in workbook.xml, so `sheet("Alpha")` was
//     unanswerable, and
//   * the part FILENAME carries no order and no identity. workbook.xml lists
//     sheets in tab order and points at each through an r:id which
//     xl/_rels/workbook.xml.rels maps to a Target; a producer is free to point
//     the first sheet at `sheet2.xml` (or at `/xl/worksheets/sheet2.xml`, the
//     package-absolute form). Guessing by filename then returns a DIFFERENT
//     SHEET at rc 0, with plausible numbers in it — the failure mode a reader
//     must never have.
//
// So the chain is walked: name -> r:id -> Target -> part. The old filename
// guess survives only as the last fallback, for a workbook whose workbook.xml
// is absent or unparsable.

// A relationship Target as a part path inside the package. Targets in
// workbook.xml.rels are relative to the part's own directory (`xl/`), except
// for the package-absolute form which starts at the root.
std::string relTargetToPart(const std::string& target) {
  if (target.empty()) return std::string();
  if (target[0] == '/') return target.substr(1);
  std::string t = target;
  while (t.rfind("./", 0) == 0) t = t.substr(2);
  return "xl/" + t;
}

std::vector<SheetInfo> workbookSheets(const std::map<std::string, std::string>& parts) {
  std::vector<SheetInfo> out;
  auto wb = parts.find("xl/workbook.xml");
  if (wb == parts.end()) return out;

  std::map<std::string, std::string> rels;
  {
    auto rl = parts.find("xl/_rels/workbook.xml.rels");
    if (rl != parts.end()) {
      const std::string& x = rl->second;
      size_t p = 0;
      while ((p = x.find("<Relationship", p)) != std::string::npos) {
        size_t gt = x.find('>', p);
        if (gt == std::string::npos) break;
        const std::string tag = x.substr(p, gt - p + 1);
        const std::string id = attr(tag, "Id");
        const std::string target = attr(tag, "Target");
        if (!id.empty() && !target.empty()) rels[id] = unescapeXml(target);
        p = gt + 1;
      }
    }
  }

  const std::string& x = wb->second;
  size_t p = 0;
  // "<sheet " with the trailing space: it must not match <sheets> or <sheetPr>.
  while ((p = x.find("<sheet ", p)) != std::string::npos) {
    size_t gt = x.find('>', p);
    if (gt == std::string::npos) break;
    const std::string tag = x.substr(p, gt - p + 1);
    SheetInfo si;
    si.name = unescapeXml(attr(tag, "name"));
    std::string rid = attr(tag, "r:id");
    if (rid.empty()) rid = attr(tag, "relationshipId");
    auto it = rels.find(rid);
    if (it != rels.end()) si.part = relTargetToPart(it->second);
    // A Target that names no part in this package is not a part; saying so here
    // keeps every consumer from having to re-check.
    if (!si.part.empty() && parts.find(si.part) == parts.end()) si.part.clear();
    out.push_back(std::move(si));
    p = gt + 1;
  }
  return out;
}

// The row grid of one worksheet part, positioned by each cell's own r= ref.
// Extracted from read() so `sheets()` (which needs each sheet's used range) and
// the load path agree by construction rather than by a second parser.
// One cellXfs entry's number format: the numFmtId and, for a custom id, the
// format code from <numFmts>.
struct XfFormat {
  int numFmtId = 0;
  std::string code;
  bool isDate = false;
};

std::vector<std::vector<Cell>> parseSheetRows(const std::string& sheetXml,
                                              const std::vector<std::string>& shared,
                                              size_t* maxColsOut,
                                              const std::vector<XfFormat>* xfs = nullptr);

}  // namespace

// ---------------------------------------------------------------------------

size_t worksheetCount(const std::string& path) {
  std::map<std::string, std::string> parts;
  try {
    parts = readZip(path);
  } catch (const std::exception&) {
    return 0;
  }
  size_t n = 0;
  for (const auto& kv : parts) {
    const std::string& name = kv.first;
    if (name.rfind("xl/worksheets/", 0) != 0) continue;
    if (name.size() <= 14) continue;                      // the directory entry itself
    if (name.find('/', 14) != std::string::npos) continue;  // xl/worksheets/_rels/...
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0) ++n;
  }
  return n;
}

namespace {

std::vector<std::vector<Cell>> parseSheetRows(const std::string& sheetXml,
                                              const std::vector<std::string>& shared,
                                              size_t* maxColsOut,
                                              const std::vector<XfFormat>* xfs) {
  std::vector<std::vector<Cell>> rows;
  size_t maxCols = 0;

  size_t p = 0;
  while (true) {
    size_t rowOpen = sheetXml.find("<row", p);
    if (rowOpen == std::string::npos) break;
    size_t rowGt = sheetXml.find('>', rowOpen);
    if (rowGt == std::string::npos) break;
    bool selfClosing = sheetXml[rowGt - 1] == '/';
    size_t rowEnd = selfClosing ? rowGt : sheetXml.find("</row>", rowGt);
    if (rowEnd == std::string::npos) break;

    // BUG-413: honour the row's OWN `r=` index instead of appending in document
    // order. A worksheet is allowed to omit empty rows entirely -- putexcel
    // writes exactly such a sheet (`A1` then `A4`), and so does every real
    // spreadsheet with a blank line in it. Appending sequentially made rows 1,
    // 4, 5 read back as rows 1, 2, 3: `import excel` silently SHIFTED every row
    // after a gap upward, at rc 0, with the right values in the wrong
    // observations. Columns were already positioned by their cell ref; this is
    // the same rule applied to the other axis.
    long long rowRef = -1;
    {
      const std::string rowTag = sheetXml.substr(rowOpen, rowGt - rowOpen + 1);
      size_t rp = rowTag.find(" r=\"");
      if (rp != std::string::npos) {
        size_t rq = rowTag.find('"', rp + 4);
        if (rq != std::string::npos) {
          try {
            rowRef = std::stoll(rowTag.substr(rp + 4, rq - rp - 4));
          } catch (...) {
            rowRef = -1;
          }
        }
      }
    }
    // A sheet with no r= attributes at all keeps the old positional behaviour,
    // which is what every file this writer produced before now looks like.
    if (rowRef > 0 && static_cast<size_t>(rowRef - 1) > rows.size()) {
      rows.resize(static_cast<size_t>(rowRef - 1));
    }

    std::vector<Cell> row;
    if (!selfClosing) {
      size_t q = rowGt;
      while (q < rowEnd) {
        size_t cOpen = sheetXml.find("<c", q);
        if (cOpen == std::string::npos || cOpen >= rowEnd) break;
        // Guard against matching <col>/<cols> — require the next char to be
        // whitespace or '>' or '/'.
        char after = sheetXml[cOpen + 2];
        if (!(after == ' ' || after == '>' || after == '/' || after == '\t')) { q = cOpen + 2; continue; }
        size_t cGt = sheetXml.find('>', cOpen);
        if (cGt == std::string::npos || cGt > rowEnd) break;
        std::string cTag = sheetXml.substr(cOpen, cGt - cOpen + 1);
        bool cSelfClosing = sheetXml[cGt - 1] == '/';
        size_t cEnd = cSelfClosing ? cGt : sheetXml.find("</c>", cGt);
        if (cEnd == std::string::npos) break;

        int colIdx = colIndexFromRef(attr(cTag, "r"));
        std::string type = attr(cTag, "t");
        Cell cell;
        if (xfs != nullptr) {
          const std::string sAttr = attr(cTag, "s");
          if (!sAttr.empty() && sAttr.find_first_not_of("0123456789") == std::string::npos) {
            const size_t xi = static_cast<size_t>(std::stoul(sAttr));
            if (xi < xfs->size()) {
              cell.numFmtId = (*xfs)[xi].numFmtId;
              cell.numFmtCode = (*xfs)[xi].code;
              cell.isDate = (*xfs)[xi].isDate;
            }
          }
        }
        if (!cSelfClosing) {
          std::string body = sheetXml.substr(cGt + 1, cEnd - cGt - 1);
          if (type == "s") {  // shared string index
            size_t vOpen = body.find("<v>");
            size_t vClose = body.find("</v>");
            if (vOpen != std::string::npos && vClose != std::string::npos) {
              try {
                size_t idx = static_cast<size_t>(std::stoul(body.substr(vOpen + 3, vClose - vOpen - 3)));
                if (idx < shared.size()) { cell.text = shared[idx]; cell.blank = cell.text.empty(); }
              } catch (...) {
              }
            }
          } else if (type == "inlineStr") {
            cell.text = collectText(body, 0, body.size());
            cell.blank = cell.text.empty();
          } else if (type == "str") {  // formula result, cached as text
            size_t vOpen = body.find("<v>");
            size_t vClose = body.find("</v>");
            if (vOpen != std::string::npos && vClose != std::string::npos) {
              cell.text = unescapeXml(body.substr(vOpen + 3, vClose - vOpen - 3));
              cell.blank = cell.text.empty();
            }
          } else if (type == "e") {
            // An error cell (#N/A, #DIV/0!, #VALUE! ...) is MISSING to Stata, not
            // the text of the error: 007's baseline sheet has 33 `#N/A` ids that
            // `drop if new_id == ""` removes in StataBE, after which `destring
            // new_id` finds every value numeric (probe nid.do, scratch-r150/t7-007).
            // Left blank.
          } else {  // number (or boolean, read as its literal text)
            size_t vOpen = body.find("<v>");
            size_t vClose = body.find("</v>");
            if (vOpen != std::string::npos && vClose != std::string::npos) {
              std::string raw = body.substr(vOpen + 3, vClose - vOpen - 3);
              try {
                size_t used = 0;
                double d = std::stod(raw, &used);
                if (used == raw.size()) {
                  cell.isNumber = true;
                  cell.number = d;
                  cell.blank = false;
                } else {
                  cell.text = unescapeXml(raw);
                  cell.blank = cell.text.empty();
                }
              } catch (...) {
                cell.text = unescapeXml(raw);
                cell.blank = cell.text.empty();
              }
            }
          }
        }
        if (static_cast<size_t>(colIdx) >= row.size()) row.resize(static_cast<size_t>(colIdx) + 1);
        row[static_cast<size_t>(colIdx)] = cell;
        q = cSelfClosing ? cGt + 1 : cEnd + 4;
      }
    }
    maxCols = std::max(maxCols, row.size());
    rows.push_back(std::move(row));
    p = selfClosing ? rowGt + 1 : rowEnd + 6;
  }

  for (auto& r : rows) r.resize(maxCols);
  if (maxColsOut != nullptr) *maxColsOut = maxCols;
  return rows;
}

// True for a number format that renders a date: the built-in date ids, or a
// custom code with a day/month/year letter outside its quoted literals and
// [bracketed] sections.
bool isDateFormat(int id, const std::string& code) {
  if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) ||
      (id >= 50 && id <= 58))
    return true;
  if (code.empty()) return false;
  bool inQ = false, inB = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    if (c == '"') { inQ = !inQ; continue; }
    if (inQ) continue;
    if (c == '[') { inB = true; continue; }
    if (c == ']') { inB = false; continue; }
    if (inB) continue;
    if (c == '\\') { ++i; continue; }
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == 'd' || l == 'y' || l == 'm') return true;
  }
  return false;
}

// The workbook's cellXfs, in order (a cell's s="i" indexes it).
std::vector<XfFormat> cellXfsOf(const std::map<std::string, std::string>& parts) {
  std::vector<XfFormat> out;
  auto it = parts.find("xl/styles.xml");
  if (it == parts.end()) return out;
  const std::string& xml = it->second;
  std::map<int, std::string> custom;
  {
    const size_t a = xml.find("<numFmts");
    const size_t b = a == std::string::npos ? std::string::npos : xml.find("</numFmts>", a);
    size_t p = a;
    while (a != std::string::npos && b != std::string::npos) {
      p = xml.find("<numFmt ", p + 1);
      if (p == std::string::npos || p > b) break;
      const size_t gt = xml.find('>', p);
      if (gt == std::string::npos) break;
      const std::string tag = xml.substr(p, gt - p + 1);
      const std::string id = attr(tag, "numFmtId");
      try {
        custom[std::stoi(id)] = unescapeXml(attr(tag, "formatCode"));
      } catch (...) {
      }
    }
  }
  const size_t a = xml.find("<cellXfs");
  if (a == std::string::npos) return out;
  const size_t b = xml.find("</cellXfs>", a);
  if (b == std::string::npos) return out;
  size_t p = a;
  while (true) {
    p = xml.find("<xf", p + 1);
    if (p == std::string::npos || p > b) break;
    const char after = xml[p + 3];
    if (!(after == ' ' || after == '>' || after == '/')) continue;
    const size_t gt = xml.find('>', p);
    if (gt == std::string::npos) break;
    const std::string tag = xml.substr(p, gt - p + 1);
    XfFormat f;
    try {
      const std::string id = attr(tag, "numFmtId");
      f.numFmtId = id.empty() ? 0 : std::stoi(id);
    } catch (...) {
      f.numFmtId = 0;
    }
    auto c = custom.find(f.numFmtId);
    if (c != custom.end()) f.code = c->second;
    f.isDate = isDateFormat(f.numFmtId, f.code);
    out.push_back(f);
  }
  return out;
}

// The shared-string table, or an empty one when the workbook has none.
std::vector<std::string> sharedStringsOf(const std::map<std::string, std::string>& parts) {
  auto it = parts.find("xl/sharedStrings.xml");
  if (it == parts.end()) return {};
  return parseSharedStrings(it->second);
}

// The one place the LAST-RESORT filename guess still lives: a workbook whose
// workbook.xml is missing or unparsable, which is not a shape any producer in
// this campaign emits but is what the pre-W3a reader always did.
const std::string* guessSheetPart(const std::map<std::string, std::string>& parts) {
  auto it = parts.find("xl/worksheets/sheet1.xml");
  if (it != parts.end()) return &it->second;
  for (const auto& kv : parts) {
    if (kv.first.rfind("xl/worksheets/", 0) == 0 && kv.first.size() > 4 &&
        kv.first.compare(kv.first.size() - 4, 4, ".xml") == 0) {
      return &kv.second;
    }
  }
  return nullptr;
}

// The rectangle a sheet actually occupies, in "A1:C75" form; empty when no cell
// in it holds anything. This is `describe`'s "available cell range".
std::string usedRange(const std::vector<std::vector<Cell>>& rows) {
  size_t r0 = 0, r1 = 0, c0 = 0, c1 = 0;
  bool any = false;
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t c = 0; c < rows[r].size(); ++c) {
      if (rows[r][c].blank) continue;
      if (!any) { r0 = r1 = r; c0 = c1 = c; any = true; continue; }
      r1 = r;                      // rows are visited in ascending order
      if (r < r0) r0 = r;
      if (c < c0) c0 = c;
      if (c > c1) c1 = c;
    }
  }
  if (!any) return std::string();
  return colRef(static_cast<int>(c0)) + std::to_string(r0 + 1) + ":" +
         colRef(static_cast<int>(c1)) + std::to_string(r1 + 1);
}

}  // namespace

std::vector<SheetInfo> sheets(const std::string& path) {
  std::map<std::string, std::string> parts = readZip(path);
  std::vector<SheetInfo> out = workbookSheets(parts);
  if (out.empty()) {
    // No workbook.xml to name them: report what is demonstrably there, under
    // the conventional names, rather than reporting nothing.
    for (const auto& kv : parts) {
      if (kv.first.rfind("xl/worksheets/", 0) != 0) continue;
      if (kv.first.find('/', 14) != std::string::npos) continue;
      if (kv.first.size() <= 4 || kv.first.compare(kv.first.size() - 4, 4, ".xml") != 0) continue;
      SheetInfo si;
      si.name = kv.first.substr(14, kv.first.size() - 18);
      si.part = kv.first;
      out.push_back(std::move(si));
    }
  }
  const std::vector<std::string> shared = sharedStringsOf(parts);
  for (SheetInfo& si : out) {
    if (si.part.empty()) continue;
    size_t maxCols = 0;
    si.range = usedRange(parseSheetRows(parts.at(si.part), shared, &maxCols));
  }
  return out;
}

Sheet read(const std::string& path, const ReadOptions& opts) {
  std::map<std::string, std::string> parts = readZip(path);
  const std::vector<SheetInfo> wbSheets = workbookSheets(parts);

  const std::string* sheetXml = nullptr;
  if (!opts.sheetName.empty()) {
    bool named = false;
    for (const SheetInfo& si : wbSheets) {
      if (si.name != opts.sheetName) continue;
      named = true;
      if (!si.part.empty()) sheetXml = &parts.at(si.part);
      break;
    }
    // Named-but-absent and named-but-broken are different facts: the first is
    // Stata's r(601), the second is a defective package.
    if (!named) throw SheetNotFound(opts.sheetName);
    if (sheetXml == nullptr) {
      throw std::runtime_error("worksheet " + opts.sheetName + " in " + path +
                               " has no worksheet part");
    }
  } else {
    for (const SheetInfo& si : wbSheets) {
      if (si.part.empty()) continue;
      sheetXml = &parts.at(si.part);
      break;
    }
    if (sheetXml == nullptr) sheetXml = guessSheetPart(parts);
  }
  if (sheetXml == nullptr) throw std::runtime_error(path + " contains no worksheet");

  const std::vector<std::string> shared = sharedStringsOf(parts);
  const std::vector<XfFormat> xfs = cellXfsOf(parts);
  size_t maxCols = 0;
  std::vector<std::vector<Cell>> rows = parseSheetRows(*sheetXml, shared, &maxCols, &xfs);

  // The `cellrange` window, applied to the ABSOLUTE grid before anything else
  // consumes it: rows are already positioned by their own r= index and cells by
  // their own column ref, so a 1-based inclusive window is a slice here and
  // would be a guess anywhere later.
  // B4 P2: the lower-right corner must be INSIDE the data area. Measured on the
  // grid as parsed, BEFORE any windowing below narrows it — `rows.size()` is
  // the sheet's own last data row and `maxCols` its own last data column, which
  // is what Stata means by "the data area in the worksheet". Both checks are
  // one-sided upper bounds, so an open-ended cellrange (lastRow/lastCol == 0)
  // is untouched.
  //
  // Column first: oracle_q measured the two axes SEPARATELY (A1:Z75 exceeds only
  // the column bound, A1:D200 only the row bound), so a range that exceeds BOTH
  // is not settled by that capture and the order here is a choice, not a
  // measurement. It is named as such rather than presented as parity.
  if (opts.lastCol > 0 && static_cast<size_t>(opts.lastCol) > maxCols) {
    throw CellRangeOutsideData(/*columnAxis=*/true);
  }
  if (opts.lastRow > 0 && static_cast<size_t>(opts.lastRow) > rows.size()) {
    throw CellRangeOutsideData(/*columnAxis=*/false);
  }

  Sheet out;
  const size_t firstRow = opts.firstRow > 0 ? static_cast<size_t>(opts.firstRow) : 1;
  const size_t firstCol = opts.firstCol > 0 ? static_cast<size_t>(opts.firstCol) : 1;
  out.firstCol = static_cast<int>(firstCol);
  if (opts.lastRow > 0 && static_cast<size_t>(opts.lastRow) < rows.size()) {
    rows.resize(static_cast<size_t>(opts.lastRow));
  }
  if (firstRow > 1) {
    if (firstRow - 1 >= rows.size()) rows.clear();
    else rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(firstRow - 1));
  }
  if (firstCol > 1 || opts.lastCol > 0) {
    // The clamp below is now only ever a no-op on the upper bound: B4 P2's
    // check above has already REFUSED any lastCol past the data area, which is
    // what oracle_q.log:74-79 measured Stata doing. It stays as the guard for
    // the ragged case (a row shorter than the widest row), which is a different
    // thing from a cellrange past the sheet.
    size_t lo = firstCol - 1;
    size_t hi = opts.lastCol > 0 ? static_cast<size_t>(opts.lastCol) : maxCols;  // exclusive
    if (hi > maxCols) hi = maxCols;
    for (auto& r : rows) {
      if (lo >= r.size()) { r.clear(); continue; }
      const size_t end = std::min(hi, r.size());
      std::vector<Cell> slice(r.begin() + static_cast<std::ptrdiff_t>(lo),
                              r.begin() + static_cast<std::ptrdiff_t>(std::max(end, lo)));
      r = std::move(slice);
    }
  }

  if (opts.firstRowIsHeader && !rows.empty()) {
    for (const Cell& c : rows.front()) {
      if (c.blank) out.header.push_back(std::string());
      else if (c.isNumber) out.header.push_back(eval::pythonFloatRepr(c.number));
      else out.header.push_back(c.text);
    }
    rows.erase(rows.begin());
  }
  out.rows = std::move(rows);
  return out;
}

Sheet read(const std::string& path, bool firstRowIsHeader) {
  ReadOptions opts;
  opts.firstRowIsHeader = firstRowIsHeader;
  return read(path, opts);
}

namespace {

// ---------------------------------------------------------------------------
// THE SHARED SKELETON (AER-SCOPE1 bundle G).
//
// `write()` (rectangular, for `export excel`) and `writeCells()` (sparse and
// cell-addressed, for `putexcel`) differ ONLY in how the <row>/<c> elements
// between these bookends are produced. Everything else -- the worksheet
// preamble, the merge table, the five ZIP parts, the STORE-only container and
// the sibling-temp + rename(2) commit -- is emitted from here, once. A second
// copy would be a second thing to keep in step, and the round-trip test only
// ever exercises one of the two writers.

const char kSheetOpen[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<sheetData>";

// Close <sheetData>, append the merge table when there is one, close the
// worksheet. An EMPTY `merges` emits no <mergeCells> element at all, which is
// what keeps an unmerged workbook byte-identical to the document `write()`
// produced before this entry point existed.
std::string closeSheet(const std::vector<std::string>& merges) {
  std::string out = "</sheetData>";
  if (!merges.empty()) {
    out += "<mergeCells count=\"" + std::to_string(merges.size()) + "\">";
    for (const auto& m : merges) out += "<mergeCell ref=\"" + escapeXml(m) + "\"/>";
    out += "</mergeCells>";
  }
  out += "</worksheet>";
  return out;
}

// Sibling temp + rename(2): a failure never truncates an existing workbook, and
// writing over a file we just READ is safe (BUG-138's dest==source law — the
// sheetreplace path below reads `path` and then writes it).
void commitZip(const std::string& path, const std::string& zip) {
  std::string tmp = path + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    // The close flushes; a full disk or a file-size limit shows up only there,
    // and the rename below would then replace the user's file with a truncated
    // one at rc 0 (AER tier-1 review: `unzipfile ..., replace` under ulimit -f).
    f.close();
    if (f.fail()) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      throw std::runtime_error("failed writing " + path + " (disk full?)");
    }
  }
  std::error_code ec;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(path), ec);
  if (ec) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    throw std::runtime_error("failed writing " + path);
  }
}

// Package one complete worksheet part as a .xlsx at `path`, under the sheet
// name `sheetName`. Throws std::runtime_error on an I/O failure.
// The styles part this writer authors when a column carries a number format:
// cellXfs 0 = General, 1 = numFmtId 14 (m/d/yyyy), 2 = numFmtId 2 (0.00).
const char kStylesXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
    "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
    "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/>"
    "</cellStyleXfs>"
    "<cellXfs count=\"3\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
    "<xf numFmtId=\"14\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
    "applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"2\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
    "applyNumberFormat=\"1\"/></cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
    "</styleSheet>";

const char kStylesContentType[] =
    "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-"
    "officedocument.spreadsheetml.styles+xml\"/>";

bool needsStyles(const std::vector<OutColumn>& cols) {
  for (const auto& c : cols)
    if (c.numFmtId != 0) return true;
  return false;
}

// numFmtId -> cellXfs index in kStylesXml.
std::map<int, int> ownXfIndex() { return {{14, 1}, {2, 2}}; }

void packageWorkbook(const std::string& path, const std::string& sheetName,
                     const std::string& sheet, bool withStyles = false) {
  std::vector<ZipEntry> entries = {
      {"[Content_Types].xml",
       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
       "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package."
       "relationships+xml\"/>"
       "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
       "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-"
       "officedocument.spreadsheetml.sheet.main+xml\"/>"
       "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd."
       "openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
       "</Types>"},
      {"_rels/.rels",
       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
       "relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
       "</Relationships>"},
      {"xl/workbook.xml",
       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
       "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
       "<sheets><sheet name=\"" + escapeXml(sheetName) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets>"
       "</workbook>"},
      {"xl/_rels/workbook.xml.rels",
       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
       "relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
       "</Relationships>"},
      {"xl/worksheets/sheet1.xml", sheet},
  };
  if (withStyles) {
    // Three parts learn about the styles part: the content types, the
    // workbook's relationships, and the part itself.
    std::string& ct = entries[0].data;
    ct.insert(ct.rfind("</Types>"), kStylesContentType);
    std::string& rels = entries[3].data;
    rels.insert(rels.rfind("</Relationships>"),
                "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/"
                "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>");
    entries.push_back({"xl/styles.xml", kStylesXml});
  }

  commitZip(path, buildZip(entries));
}

// One cell's XML, at an already-computed A1 reference. The number branch is
// FULL precision (`pythonFloatRepr`, the shortest decimal that reads back
// bit-identical) -- never a rounded rendering, in either writer. A NaN has no
// numeric spelling a spreadsheet reads back as missing, so it is written as
// Stata's own missing token instead of the literal "nan" a bare repr would
// emit into a <v> element.
void appendCell(std::string& sheet, const std::string& ref, bool isNumber, double number,
                const std::string& text) {
  if (isNumber && !std::isnan(number)) {
    sheet += "<c r=\"" + ref + "\"><v>" + eval::pythonFloatRepr(number) + "</v></c>";
    return;
  }
  sheet += "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t>";
  sheet += escapeXml(isNumber ? std::string(".") : text);
  sheet += "</t></is></c>";
}

// The rectangle `export excel` writes, as one worksheet part. Extracted from
// write() so writeSheetInto() emits the SAME bytes rather than a second
// description of the same table.
std::string rectSheetXml(const std::vector<OutColumn>& cols, bool writeHeader,
                         const std::map<int, int>& xfOf = {}) {
  size_t nRows = 0;
  for (const auto& c : cols) nRows = std::max(nRows, c.isNumber.size());
  const size_t nCols = cols.size();

  std::string sheet;
  sheet.reserve(nRows * nCols * 24 + 512);
  sheet += kSheetOpen;

  size_t rowNo = 1;
  if (writeHeader) {
    sheet += "<row r=\"1\">";
    for (size_t c = 0; c < nCols; ++c) {
      sheet += "<c r=\"" + colRef(static_cast<int>(c)) + "1\" t=\"inlineStr\"><is><t>";
      sheet += escapeXml(cols[c].name);
      sheet += "</t></is></c>";
    }
    sheet += "</row>";
    ++rowNo;
  }

  for (size_t r = 0; r < nRows; ++r, ++rowNo) {
    sheet += "<row r=\"" + std::to_string(rowNo) + "\">";
    for (size_t c = 0; c < nCols; ++c) {
      const OutColumn& oc = cols[c];
      bool blank = r >= oc.isBlank.size() || oc.isBlank[r];
      if (blank) continue;  // an absent cell IS the blank cell
      std::string ref = colRef(static_cast<int>(c)) + std::to_string(rowNo);
      const bool isNum = r < oc.isNumber.size() && oc.isNumber[r];
      if (isNum && oc.numFmtId != 0 && !std::isnan(oc.numbers[r])) {
        auto xf = xfOf.find(oc.numFmtId);
        if (xf != xfOf.end()) {
          sheet += "<c r=\"" + ref + "\" s=\"" + std::to_string(xf->second) + "\"><v>" +
                   eval::pythonFloatRepr(oc.numbers[r]) + "</v></c>";
          continue;
        }
      }
      appendCell(sheet, ref, isNum, isNum ? oc.numbers[r] : 0.0,
                 r < oc.strings.size() ? oc.strings[r] : std::string());
    }
    sheet += "</row>";
  }
  // No merges: `export excel` writes a plain rectangle, so this emits exactly
  // the bytes it always did.
  sheet += closeSheet({});
  return sheet;
}

// Insert `text` immediately before the LAST occurrence of `closeTag`. Returns
// false when the tag is not there, which is the caller's signal that the part
// is not the document it expected -- never a silent no-op.
bool insertBefore(std::string* xml, const std::string& closeTag, const std::string& text) {
  const size_t at = xml->rfind(closeTag);
  if (at == std::string::npos) return false;
  xml->insert(at, text);
  return true;
}

}  // namespace

void write(const std::string& path, const std::vector<OutColumn>& cols, bool writeHeader,
           const std::string& sheetName) {
  const bool styled = needsStyles(cols);
  packageWorkbook(path, sheetName.empty() ? std::string("Sheet1") : sheetName,
                  rectSheetXml(cols, writeHeader, styled ? ownXfIndex() : std::map<int, int>()),
                  styled);
}

// `export excel ..., sheet("S") sheetreplace` — AER scope-2 W3a.
//
// This is the expensive one, and the reason it is a separate entry point rather
// than a flag on write(): write() PACKAGES five parts it authored itself, while
// this has to keep every part of a workbook someone else authored and change
// exactly one of them. The corpus spells it as a top-level `sheetreplace`
// (papers 020, 038, 091, 108); Stata's syntax table spells the same request
// `sheet("S", replace)`.
//
// What it does NOT do, stated rather than pretended: the repackaged workbook is
// STORED, not DEFLATEd (this writer has no deflate), and no styles part is
// authored, so `keepcellfmt` remains unimplemented -- a sheet written here
// carries values only. Parts it does not understand (styles, theme, calcChain)
// pass through byte-for-byte.
void writeSheetInto(const std::string& path, const std::string& sheetName,
                    const std::vector<OutColumn>& cols, bool writeHeader) {
  if (sheetName.empty()) throw std::runtime_error("sheetreplace needs a sheet name");
  std::map<std::string, std::string> parts = readZip(path);
  if (parts.find("xl/workbook.xml") == parts.end()) {
    throw std::runtime_error(path + " has no xl/workbook.xml — refusing to rewrite it");
  }
  // A column with a number format needs a cellXfs entry for it: reuse the
  // workbook's own when it has one with that numFmtId, else append one (and
  // author the whole styles part when the workbook has none).
  std::map<int, int> xfOf;
  if (needsStyles(cols)) {
    auto st = parts.find("xl/styles.xml");
    if (st == parts.end()) {
      const std::string relsPath = "xl/_rels/workbook.xml.rels";
      auto rels = parts.find(relsPath);
      auto ct = parts.find("[Content_Types].xml");
      if (rels == parts.end() || ct == parts.end()) {
        throw std::runtime_error(path + " lacks the parts a styles part must be registered in");
      }
      size_t rid = 1;
      while (rels->second.find("\"rId" + std::to_string(rid) + "\"") != std::string::npos) ++rid;
      std::string relsXml = rels->second;
      std::string ctXml = ct->second;
      if (!insertBefore(&relsXml, "</Relationships>",
                        "<Relationship Id=\"rId" + std::to_string(rid) +
                            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                            "relationships/styles\" Target=\"styles.xml\"/>") ||
          !insertBefore(&ctXml, "</Types>", kStylesContentType)) {
        throw std::runtime_error(path + " has malformed package parts — refusing to rewrite it");
      }
      parts[relsPath] = std::move(relsXml);
      parts["[Content_Types].xml"] = std::move(ctXml);
      parts["xl/styles.xml"] = kStylesXml;
      xfOf = ownXfIndex();
    } else {
      std::string styles = st->second;
      const size_t a = styles.find("<cellXfs");
      const size_t b = a == std::string::npos ? std::string::npos : styles.find("</cellXfs>", a);
      if (a == std::string::npos || b == std::string::npos) {
        throw std::runtime_error("xl/styles.xml has no cellXfs — refusing to rewrite " + path);
      }
      std::vector<int> ids;
      for (size_t p = a; (p = styles.find("<xf", p + 1)) != std::string::npos && p < b;) {
        const char after = styles[p + 3];
        if (!(after == ' ' || after == '>' || after == '/')) continue;
        const size_t gt = styles.find('>', p);
        const std::string id = attr(styles.substr(p, gt - p + 1), "numFmtId");
        ids.push_back(id.empty() ? 0 : std::atoi(id.c_str()));
      }
      std::string added;
      int next = static_cast<int>(ids.size());
      for (const auto& c : cols) {
        if (c.numFmtId == 0 || xfOf.count(c.numFmtId)) continue;
        int found = -1;
        for (size_t i = 0; i < ids.size(); ++i)
          if (ids[i] == c.numFmtId) { found = static_cast<int>(i); break; }
        if (found < 0) {
          added += "<xf numFmtId=\"" + std::to_string(c.numFmtId) +
                   "\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>";
          found = next++;
        }
        xfOf[c.numFmtId] = found;
      }
      if (!added.empty()) {
        styles.insert(b, added);
        const size_t cnt = styles.find("count=\"", a);
        const size_t gt = styles.find('>', a);
        if (cnt != std::string::npos && cnt < gt) {
          const size_t q = styles.find('"', cnt + 7);
          styles.replace(cnt + 7, q - (cnt + 7), std::to_string(next));
        }
        parts["xl/styles.xml"] = std::move(styles);
      }
    }
  }
  const std::string sheetXml = rectSheetXml(cols, writeHeader, xfOf);

  const std::vector<SheetInfo> existing = workbookSheets(parts);
  std::string targetPart;
  for (const SheetInfo& si : existing) {
    if (si.name == sheetName) { targetPart = si.part; break; }
  }

  if (!targetPart.empty()) {
    // The sheet is there: replace its part and touch NOTHING else. workbook.xml,
    // the rels and [Content_Types].xml already describe it correctly.
    parts[targetPart] = sheetXml;
  } else {
    // A new sheet. Four documents have to agree about it, and a package where
    // they disagree is one Excel offers to "repair" -- which is a data-loss
    // prompt in front of the user's own file.
    size_t n = 1;
    while (parts.find("xl/worksheets/sheet" + std::to_string(n) + ".xml") != parts.end()) ++n;
    const std::string newPart = "xl/worksheets/sheet" + std::to_string(n) + ".xml";

    // An r:id nothing in workbook.xml.rels is using.
    std::string relsPath = "xl/_rels/workbook.xml.rels";
    if (parts.find(relsPath) == parts.end()) {
      throw std::runtime_error(path + " has no " + relsPath + " — refusing to rewrite it");
    }
    size_t rid = 1;
    while (parts[relsPath].find("\"rId" + std::to_string(rid) + "\"") != std::string::npos) ++rid;
    const std::string ridStr = "rId" + std::to_string(rid);

    // A sheetId nothing in workbook.xml is using.
    long long maxSheetId = 0;
    {
      const std::string& wb = parts["xl/workbook.xml"];
      size_t p = 0;
      while ((p = wb.find("sheetId=\"", p)) != std::string::npos) {
        p += 9;
        long long v = 0;
        while (p < wb.size() && wb[p] >= '0' && wb[p] <= '9') { v = v * 10 + (wb[p] - '0'); ++p; }
        maxSheetId = std::max(maxSheetId, v);
      }
    }

    std::string relsXml = parts[relsPath];
    if (!insertBefore(&relsXml,
                      "</Relationships>",
                      "<Relationship Id=\"" + ridStr +
                          "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                          "relationships/worksheet\" Target=\"worksheets/sheet" +
                          std::to_string(n) + ".xml\"/>")) {
      throw std::runtime_error(relsPath + " is not a relationships part — refusing to rewrite " +
                               path);
    }
    std::string wbXml = parts["xl/workbook.xml"];
    if (!insertBefore(&wbXml, "</sheets>",
                      "<sheet name=\"" + escapeXml(sheetName) + "\" sheetId=\"" +
                          std::to_string(maxSheetId + 1) + "\" r:id=\"" + ridStr + "\"/>")) {
      throw std::runtime_error("xl/workbook.xml has no <sheets> element — refusing to rewrite " +
                               path);
    }
    std::string ctPath = "[Content_Types].xml";
    if (parts.find(ctPath) == parts.end()) {
      throw std::runtime_error(path + " has no [Content_Types].xml — refusing to rewrite it");
    }
    std::string ctXml = parts[ctPath];
    if (!insertBefore(&ctXml, "</Types>",
                      "<Override PartName=\"/" + newPart +
                          "\" ContentType=\"application/vnd.openxmlformats-officedocument."
                          "spreadsheetml.worksheet+xml\"/>")) {
      throw std::runtime_error("[Content_Types].xml is not a content-types part — refusing to "
                               "rewrite " + path);
    }
    // Every check passed before ANY part was replaced: the map is mutated only
    // once the whole edit is known to be expressible.
    parts[relsPath] = std::move(relsXml);
    parts["xl/workbook.xml"] = std::move(wbXml);
    parts[ctPath] = std::move(ctXml);
    parts[newPart] = sheetXml;
  }

  // [Content_Types].xml first, as every producer writes it; the rest in the
  // map's own order, which is deterministic.
  std::vector<ZipEntry> entries;
  entries.reserve(parts.size());
  {
    auto ct = parts.find("[Content_Types].xml");
    if (ct != parts.end()) entries.push_back({ct->first, ct->second});
    for (const auto& kv : parts) {
      if (kv.first == "[Content_Types].xml") continue;
      entries.push_back({kv.first, kv.second});
    }
  }
  commitZip(path, buildZip(entries));
}

// AER-SCOPE1 bundle G (IO-TEXT). The sparse twin of write(): same skeleton,
// same escaping, same column-ref helper, same ZIP container -- only the row
// emission differs, because `putexcel`'s grammar addresses one cell at a time
// and leaves every unmentioned cell absent (which IS the blank cell in
// SpreadsheetML, exactly as write()'s `blank` branch already relies on).
void writeCells(const std::string& path, const std::string& sheetName,
                const std::vector<CellOut>& cells, const std::vector<std::string>& merges) {
  // Last write to an address wins, and rows/columns come out in ascending
  // order -- a <row r> out of order, or two <c> elements with the same r,
  // is a document Excel repairs rather than opens.
  std::map<int, std::map<int, const CellOut*>> grid;
  for (const CellOut& c : cells) {
    if (c.row < 1 || c.col < 1) continue;  // an unaddressable cell, silently absent
    grid[c.row][c.col] = &c;
  }

  std::string sheet;
  sheet.reserve(cells.size() * 32 + 512);
  sheet += kSheetOpen;
  for (const auto& [rowNo, row] : grid) {
    sheet += "<row r=\"" + std::to_string(rowNo) + "\">";
    for (const auto& [colNo, cell] : row) {
      const std::string ref = colRef(colNo - 1) + std::to_string(rowNo);
      appendCell(sheet, ref, cell->isNumber, cell->number, cell->text);
    }
    sheet += "</row>";
  }
  sheet += closeSheet(merges);

  packageWorkbook(path, sheetName.empty() ? std::string("Sheet1") : sheetName, sheet);
}

}  // namespace piet::xlsx

// AER tier-1: the zip verbs' window onto the primitives above (zip_io.h).
namespace piet::zipio {

std::vector<Entry> readEntries(const std::string& path) {
  std::vector<Entry> out;
  for (auto& [name, data] : xlsx::readZipOrdered(path, ".zip")) {
    out.push_back(Entry{name, std::move(data)});
  }
  return out;
}

std::string build(const std::vector<Entry>& entries) {
  std::vector<xlsx::ZipEntry> ze;
  ze.reserve(entries.size());
  for (const auto& e : entries) ze.push_back(xlsx::ZipEntry{e.name, e.data});
  return xlsx::buildZip(ze);
}

void commit(const std::string& path, const std::string& bytes) { xlsx::commitZip(path, bytes); }

}  // namespace piet::zipio
