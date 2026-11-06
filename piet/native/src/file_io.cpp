// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-343 — Stata's `file` command family; BUG-1526 added binary mode, `seek`
// and `set`.
//
// WHY IT EXISTS AT ALL. `file open|write|read|close` had no row in the 162-entry
// command registry: a do-file that logged its own progress, or built a report,
// stopped with an honest unsupported-command error. It was found the way gaps
// usually are — a probe fixture authored ahead of its feature assumed the
// surrounding language worked, and it did not.
//
// SCOPE, chosen and named rather than drifted into:
//   * `file open <h> using "<path>", {read|write|read write} [text|binary] [replace|append]`
//   * `file write <h> ("str"|`"…"'|(exp)|%fmt (exp)|_skip|_column|_newline|_char|_tab|_page|_dup) …`
//     (text) or `%{8|4}z (exp)` / `%{4|2|1}b[s|u] (exp)` / `%#s "str"|(exp)` (binary)
//   * `file read <h> <localname>` (text: one line, r(eof) on EOF) or
//     `file read <h> %fmt name …` (binary: a scalar, or a local for %#s)
//   * `file seek <h> query|tof|eof|#` → r(loc); `file set <h> byteorder hilo|lohi|1|2`
//   * `file close <h>` / `file close _all`
// `sersetfile` and `query` are REFUSED BY NAME. That is the honest half of the
// gate-① contract: what the engine cannot do, the product says plainly.
//
// Handle state lives on SessionState::fileHandles for the same reason postfile's
// does: Stata's file I/O is a named-handle protocol whose state outlives the
// statement that opened it.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

namespace {

using ojson = nlohmann::ordered_json;

ojson err(const std::string& msg, const std::string& code = "") {
  return code.empty() ? eval::engineError(msg) : eval::engineError(msg, code);
}

// EM2: an error carrying Stata's own return code (file.sthlp; w2af_macro_text_file.log
// :1650 r(601) read-open of a missing file, :1665 r(110) a handle already open,
// :2130 r(111) a handle that is not open, :1771 r(198) an unknown write directive).
ojson errRc(const std::string& msg, const std::string& rc, const std::string& code = "engine_error") {
  return eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt, "r(" + rc + ")");
}

std::string trimc(const std::string& s) { return strutil::trim(s); }

// Strip one layer of Stata quoting: "text" or compound `"text"'.
std::string dequote(const std::string& raw) {
  std::string s = trimc(raw);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

// Split a `file write` payload into its tokens, keeping quoted runs whole. Stata
// separates write items by whitespace but a quoted string may contain spaces, so
// a plain istringstream would shred `"a b"` into two items and emit them with a
// space that was never asked for.
// EM7: one item per string ("..." or a nesting `"..."'), parenthesized expression
// (balanced, quoted spans skipped -- `("ab" + "cd")` is one item), %fmt, or
// directive (`_name` with an optional `(#)`, so `_skip(3)` and `_col(10)` stay one
// item even when the argument is spaced).
std::vector<std::string> splitWriteItems(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  auto skipQuoted = [&](size_t k) -> size_t {  // one past a quoted span at k, or k
    if (eval::startsCompound(s, k)) {
      const size_t e = eval::compoundSpanEnd(s, k);
      return e == std::string::npos ? s.size() : e;
    }
    if (s[k] == '"') {
      const size_t e = s.find('"', k + 1);
      return e == std::string::npos ? s.size() : e + 1;
    }
    return k;
  };
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    const size_t start = i;
    const size_t q = skipQuoted(i);
    if (q != i) {
      i = q;
    } else if (s[i] == '(') {
      int depth = 0;
      while (i < s.size()) {
        const size_t qq = skipQuoted(i);
        if (qq != i) {
          i = qq;
          continue;
        }
        if (s[i] == '(') ++depth;
        if (s[i] == ')' && --depth == 0) {
          ++i;
          break;
        }
        ++i;
      }
    } else {
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '"' &&
             s[i] != '(' && !eval::startsCompound(s, i))
        ++i;
      // a directive's `(#)` belongs to it, spaces allowed inside
      if (s[start] == '_' && i < s.size() && s[i] == '(') {
        const size_t close = s.find(')', i);
        i = close == std::string::npos ? s.size() : close + 1;
      }
      if (i == start) ++i;  // a stray character is its own (invalid) item
    }
    out.push_back(s.substr(start, i - start));
  }
  return out;
}

// Stata's %fmt for one value written by `file write` (its validity is checked
// where the %fmt is read). A string format pads a string by its BYTES, as
// StataBE does (`%~9s ("é")` is three blanks, the two bytes, four blanks):
// right-justified, `-` left, `~` centred with the odd blank on the right
// (`%~9s ("ab")` is `   ab    `). A numeric format renders a number exactly as
// `display` does: `%08.2f (3.14159)` 00003.14, `%3.1f (2.25)` 2.2
// (w2af_macro_text_file.log:1837), `%12.0g (1/3)` ` .3333333333` (:1840). A
// format of the other kind is ignored and the value written as if it had none
// (`%9s (1/3)` .33333333, `%9.2f ("ab")` ab) -- StataBE 19.5, measured.
std::string formatWriteValue(const std::string& fmt, const std::string& text, bool isStr) {
  const bool strFmt = !fmt.empty() && piet::handlers::isStringDisplayFormat(fmt);
  if (isStr) {
    if (!strFmt) return text;
    const char align = fmt[1] == '-' || fmt[1] == '~' ? fmt[1] : '\0';
    const size_t w = std::stoul(fmt.substr(align ? 2 : 1, fmt.size() - (align ? 3 : 2)));
    if (text.size() >= w) return text;
    const size_t fill = w - text.size();
    if (align == '-') return text + std::string(fill, ' ');
    if (align == '~') return std::string(fill / 2, ' ') + text + std::string(fill - fill / 2, ' ');
    return std::string(fill, ' ') + text;
  }
  // a number with no format, or a string one: %10.0g, blanks removed
  if (fmt.empty() || strFmt) return strutil::trim(piet::formatNumberAsDisplayed("%10.0g", text));
  return piet::formatNumberAsDisplayed(fmt, text);
}


// ---- BUG-1526: binary mode ([P] file, "Writing and reading binary files") ---
// Measured in StataBE 19.5 (scratch-laneA2/agents/prs/x3.do): the elements are
// %{1|2|4}b[s|u], %{4|8}z and %#s; anything else is r(198) "<fmt>:  invalid
// binary format".
struct BinFmt {
  char kind = 0;  // 'b', 'z' or 's'
  size_t width = 0;
  char sign = 0;  // b: 0 = Stata's byte/int/long (with missing values), 's', 'u'
};

std::optional<BinFmt> parseBinFmt(const std::string& f) {
  static const std::regex re(R"(^%([0-9]{1,9})(bs|bu|b|z|s)$)");
  std::smatch m;
  if (!std::regex_match(f, m, re)) return std::nullopt;
  BinFmt b;
  b.width = static_cast<size_t>(std::stoul(m[1].str()));
  b.kind = m[2].str()[0];
  b.sign = m[2].length() > 1 ? m[2].str()[1] : 0;
  if (b.kind == 'b' && b.width != 1 && b.width != 2 && b.width != 4) return std::nullopt;
  if (b.kind == 'z' && b.width != 4 && b.width != 8) return std::nullopt;
  if (b.kind == 's' && b.width < 1) return std::nullopt;
  return b;
}

bool nativeHilo() {
  const uint16_t one = 1;
  unsigned char first = 0;
  std::memcpy(&first, &one, 1);
  return first == 0;
}

// The low `width` bytes of `bits` in the handle's byte order (hilo = most
// significant byte first).
std::string packBytes(uint64_t bits, size_t width, bool hilo) {
  std::string out(width, '\0');
  for (size_t k = 0; k < width; ++k) {
    const size_t shift = 8 * (hilo ? width - 1 - k : k);
    out[k] = static_cast<char>((bits >> shift) & 0xFFu);
  }
  return out;
}

uint64_t unpackBytes(const std::string& in, bool hilo) {
  uint64_t v = 0;
  const size_t width = in.size();
  for (size_t k = 0; k < width; ++k) {
    const size_t shift = 8 * (hilo ? width - 1 - k : k);
    v |= static_cast<uint64_t>(static_cast<unsigned char>(in[k])) << shift;
  }
  return v;
}

// Stata's largest nonmissing float and double (c(maxfloat), c(maxdouble)) and
// the bit patterns of their missing values: `.` and then one step per .a-.z.
constexpr double kMaxFloat = 1.70141173319264430e+38;
constexpr double kMaxDouble = 8.98846567431157854e+307;
constexpr uint32_t kFloatMissing = 0x7f000000u, kFloatMissingStep = 0x800u;
constexpr uint64_t kDoubleMissing = 0x7fe0000000000000ull,
                   kDoubleMissingStep = 0x0000010000000000ull;

// The largest value of a Stata byte/int/long; the 27 values above it are its
// missing values (StataBE 19.5: %1b 100 reads back 100, 101 `.`).
int64_t stataIntMax(size_t width) {
  return width == 1 ? 100 : width == 2 ? 32740 : 2147483620;
}

// The bytes a %b/%z element writes for `v` ([P] file Appendix A.2, measured in
// x3.do E9-E14): a noninteger is truncated; out of range, a Stata type writes
// `.`, a signed or unsigned one its minimum or maximum, and a missing value is
// "too large"; %1b/%2b/%4b keep .a-.z, %4z/%8z write the missing patterns.
std::string encodeBinNumber(const BinFmt& f, double v, std::optional<int> missingKind,
                            bool hilo) {
  const int kind = missingKind.value_or(0);
  if (f.kind == 'z') {
    if (f.width == 8) {
      uint64_t bits = kDoubleMissing + kDoubleMissingStep * static_cast<uint64_t>(kind);
      if (!missingKind && std::fabs(v) <= kMaxDouble) std::memcpy(&bits, &v, 8);
      return packBytes(bits, 8, hilo);
    }
    uint32_t bits = kFloatMissing + kFloatMissingStep * static_cast<uint32_t>(kind);
    if (!missingKind && std::fabs(v) <= kMaxFloat) {
      const float fv = static_cast<float>(v);
      std::memcpy(&bits, &fv, 4);
    }
    return packBytes(bits, 4, hilo);
  }
  const int bitsN = static_cast<int>(8 * f.width);
  int64_t lo = 0, hi = 0, x = 0;
  if (f.sign == 'u') {
    hi = static_cast<int64_t>((uint64_t{1} << bitsN) - 1);
  } else {
    hi = static_cast<int64_t>((uint64_t{1} << (bitsN - 1)) - 1);
    lo = -hi;
  }
  const double t = std::trunc(v);
  if (f.sign == 0) {
    hi = stataIntMax(f.width);
    if (missingKind) x = hi + 1 + kind;
    else if (!(t >= static_cast<double>(lo) && t <= static_cast<double>(hi))) x = hi + 1;
    else x = static_cast<int64_t>(t);
  } else if (missingKind || !(t <= static_cast<double>(hi))) {
    x = hi;
  } else {
    x = t < static_cast<double>(lo) ? lo : static_cast<int64_t>(t);
  }
  return packBytes(static_cast<uint64_t>(x), f.width, hilo);
}

// A missing value's text: `.` or `.a`-`.z`.
std::string missingText(int kind) {
  return kind <= 0 || kind > 26 ? std::string(".") : std::string(".") + static_cast<char>('a' + kind - 1);
}

// The scalar text of a %b/%z element read back (x3.do E9-E14): Stata's missing
// codes come back as `.` / `.a`-`.z`.
std::string decodeBinNumber(const BinFmt& f, const std::string& bytes, bool hilo) {
  const uint64_t raw = unpackBytes(bytes, hilo);
  if (f.kind == 'z') {
    double v = 0.0;
    if (f.width == 8) {
      if (raw >= kDoubleMissing && raw <= 0x7fffffffffffffffull) {
        const uint64_t k = (raw - kDoubleMissing) / kDoubleMissingStep;
        return missingText((raw - kDoubleMissing) % kDoubleMissingStep == 0 && k <= 26
                               ? static_cast<int>(k) : 0);
      }
      std::memcpy(&v, &raw, 8);
    } else {
      const uint32_t r32 = static_cast<uint32_t>(raw);
      if (r32 >= kFloatMissing && r32 <= 0x7fffffffu) {
        const uint32_t k = (r32 - kFloatMissing) / kFloatMissingStep;
        return missingText((r32 - kFloatMissing) % kFloatMissingStep == 0 && k <= 26
                               ? static_cast<int>(k) : 0);
      }
      float fv = 0.0f;
      std::memcpy(&fv, &r32, 4);
      v = static_cast<double>(fv);
    }
    return eval::pythonFloatRepr(v);
  }
  int64_t x = static_cast<int64_t>(raw);
  if (f.sign != 'u' && f.width < 8) {
    const uint64_t signBit = uint64_t{1} << (8 * f.width - 1);
    if (raw & signBit) x = static_cast<int64_t>(raw) - static_cast<int64_t>(signBit << 1);
  }
  if (f.sign == 0 && x > stataIntMax(f.width)) {
    return missingText(static_cast<int>(x - stataIntMax(f.width) - 1));
  }
  return eval::pythonFloatRepr(static_cast<double>(x));
}


// `file write h ...` on a binary handle: each %fmt and the value after it, one
// element at a time, so the elements before a bad one are already written
// (StataBE 19.5 x3.do E41-E42: `%1bs (1) %1bs` leaves 1 byte). A value with
// no %fmt, a text directive and a dangling %fmt are r(198) "invalid syntax"
// (E32-E34, E41); a string for %b/%z or a number for %#s is r(109) (E37, E38).
// %#s writes # bytes: the string cut to # or padded with binary zeros.
ojson writeBinary(SessionState::FileHandle& fh, const std::string& rest, SessionState& session) {
  auto invalid = [&]() { return errRc("invalid syntax", "198", "syntax_error"); };
  std::optional<BinFmt> pending;
  if (fh.readWrite) std::fseek(fh.fp, 0, SEEK_CUR);  // a write may follow a read
  for (const std::string& item : splitWriteItems(rest)) {
    if (item.empty()) continue;
    if (item[0] == '%') {
      if (pending) return invalid();
      pending = parseBinFmt(item);
      if (!pending) return errRc(item + ":  invalid binary format", "198", "syntax_error");
      continue;
    }
    const bool quoted = item[0] == '"' || eval::startsCompound(item, 0);
    if (!pending || (!quoted && item[0] != '(')) return invalid();
    std::string text = quoted ? dequote(item) : std::string();
    bool isStr = quoted;
    if (!quoted) {
      const std::string expr = item.size() >= 2 ? item.substr(1, item.size() - 2) : "";
      std::string raised;
      auto v = eval::evalArith(expr, &session.eval, &raised, &isStr);
      if (!v) {
        eval::MacroFnError fe;
        if (eval::classifyFailedExpr(expr, raised, &fe)) return errRc(fe.message, fe.rc);
        return invalid();
      }
      text = *v;
    }
    std::string bytes;
    if (pending->kind == 's') {
      if (!isStr) return errRc("type mismatch", "109");
      bytes = text.substr(0, std::min(text.size(), pending->width));
      bytes.resize(pending->width, '\0');
    } else {
      if (isStr) return errRc("type mismatch", "109");
      std::optional<int> missingKind;
      double v = 0.0;
      if (!text.empty() && text[0] == '.' &&
          (text.size() == 1 || (text.size() == 2 && text[1] >= 'a' && text[1] <= 'z'))) {
        missingKind = text.size() == 1 ? 0 : text[1] - 'a' + 1;
      } else {
        char* end = nullptr;
        v = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) return errRc("type mismatch", "109");
        if (const auto k = eval::stataExtendedMissingKind(v)) missingKind = *k;
      }
      bytes = encodeBinNumber(*pending, v, missingKind, fh.hilo);
    }
    if (std::fwrite(bytes.data(), 1, bytes.size(), fh.fp) != bytes.size())
      return err("file write: write to `" + fh.path + "` failed", "file_write_failed");
    pending.reset();
  }
  if (pending) return invalid();
  return ojson();
}

// `file read h %fmt name ...` on a binary handle: each %b/%z element into the
// scalar named after it, each %#s into a local (cut at its first binary zero,
// x3.do E10, E54); r(eof) 1 once an element runs past the end, which leaves
// the scalar missing (E16). A name without its %fmt, or a %fmt without its
// name, is r(198) (E49-E51); several pairs on one line are read in order (E52).
ojson readBinary(SessionState::FileHandle& fh, const std::string& rest, SessionState& session) {
  std::istringstream iss(rest);
  std::vector<std::string> toks;
  for (std::string t; iss >> t;) toks.push_back(t);
  if (toks.empty() || toks.size() % 2 != 0) return errRc("invalid syntax", "198", "syntax_error");
  std::vector<BinFmt> fmts;
  for (size_t i = 0; i < toks.size(); i += 2) {
    if (toks[i][0] != '%' || toks[i + 1][0] == '%')
      return errRc("invalid syntax", "198", "syntax_error");
    const auto f = parseBinFmt(toks[i]);
    if (!f) return errRc(toks[i] + ":  invalid binary format", "198", "syntax_error");
    fmts.push_back(*f);
  }
  if (fh.readWrite) std::fseek(fh.fp, 0, SEEK_CUR);  // a read may follow a write
  bool atEof = false;
  for (size_t i = 0; i < fmts.size(); ++i) {
    const BinFmt& f = fmts[i];
    const std::string& name = toks[2 * i + 1];
    std::string bytes(f.width, '\0');
    const size_t got = atEof ? 0 : std::fread(&bytes[0], 1, f.width, fh.fp);
    if (got < f.width) atEof = true;
    if (f.kind == 's') {
      bytes.resize(got);
      const size_t nul = bytes.find('\0');
      session.eval.locals.set(name, nul == std::string::npos ? bytes : bytes.substr(0, nul));
    } else {
      session.eval.scalars.set(name, got < f.width ? std::string(".") : decodeBinNumber(f, bytes, fh.hilo));
      session.eval.stringScalars.erase(name);
    }
  }
  fh.eof = atEof;
  piet::eval::OrderedFloatDict r;
  r.set("eof", atEof ? 1.0 : 0.0);
  session.eval.rValues = r;
  eval::declareRPosted(session.eval, "file read", {"eof"});  // binary: r(eof) only (E6)
  return ojson();
}
}  // namespace

ojson runFileCmd(const std::string& rawLine, SessionState& session) {
  std::string rest = trimc(rawLine);
  {
    static const std::regex reKw(R"(^file\s+)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(rest, m, reKw))
      return err("file: internal parse error (keyword missing)");
    rest = trimc(rest.substr(static_cast<size_t>(m.length(0))));
  }
  if (rest.empty())
    return err("file: a subcommand is required (open|write|read|close)", "syntax_error");

  // sub-verb
  size_t sp = rest.find_first_of(" \t");
  const std::string sub = strutil::lower(sp == std::string::npos ? rest : rest.substr(0, sp));
  rest = (sp == std::string::npos) ? std::string() : trimc(rest.substr(sp));

  // Refuse the unimplemented sub-verbs by NAME — never a generic "unknown".
  if (sub == "query" || sub == "sersetfile")
    return err("file " + sub + " is not implemented in this build "
               "(open/write/read/seek/set/close only)",
               "unsupported_command");
  if (sub != "open" && sub != "write" && sub != "read" && sub != "close" && sub != "seek" &&
      sub != "set")
    return err("file: unknown subcommand `" + sub + "` (expected open|write|read|seek|set|close)",
               "syntax_error");

  // ---- close ---------------------------------------------------------------
  if (sub == "close") {
    const std::string h = trimc(rest);
    if (h.empty()) return err("file close: a handle name is required", "syntax_error");
    if (strutil::lower(h) == "_all") {
      for (auto& kv : session.fileHandles)
        if (kv.second.fp != nullptr) std::fclose(kv.second.fp);
      session.fileHandles.clear();
      return ojson();
    }
    auto it = session.fileHandles.find(h);
    if (it == session.fileHandles.end())
      return errRc("file handle " + h + " not found", "111");
    if (it->second.fp != nullptr) std::fclose(it->second.fp);
    session.fileHandles.erase(it);
    return ojson();
  }

  // Every remaining sub-verb starts with a handle name.
  sp = rest.find_first_of(" \t");
  const std::string handle = trimc(sp == std::string::npos ? rest : rest.substr(0, sp));
  rest = (sp == std::string::npos) ? std::string() : trimc(rest.substr(sp));
  if (handle.empty()) return err("file " + sub + ": a handle name is required", "syntax_error");

  // ---- open ----------------------------------------------------------------
  if (sub == "open") {
    if (session.fileHandles.count(handle) > 0)
      return errRc("file handle " + handle + " already exists", "110");
    static const std::regex reUsing(R"(^using\s+)", std::regex::icase);
    std::smatch mu;
    if (!std::regex_search(rest, mu, reUsing))
      return err("file open: `using <filename>` is required", "syntax_error");
    rest = trimc(rest.substr(static_cast<size_t>(mu.length(0))));

    std::string opts;
    const size_t comma = rest.find(',');
    if (comma != std::string::npos) {
      opts = strutil::lower(trimc(rest.substr(comma + 1)));
      rest = trimc(rest.substr(0, comma));
    }
    // Every refusal names the file as typed, in Stata's words (BUG-410).
    const std::string typed = dequote(rest);
    const std::string path = piet::resolveUserPath(session, typed);
    if (path.empty()) return err("file open: a filename is required", "syntax_error");

    bool wantRead = opts.find("read") != std::string::npos;
    bool wantWrite = opts.find("write") != std::string::npos;
    const bool wantAppend = opts.find("append") != std::string::npos;
    const bool wantReplace = opts.find("replace") != std::string::npos;
    // BUG-1526: `binary` is the byte-stream mode (text is the default).
    const bool wantBinary = opts.find("binary") != std::string::npos;
    if (wantAppend) wantWrite = true;
    if (!wantRead && !wantWrite) wantRead = true;  // Stata's default is read
    // `read write`: both verbs on one handle, the file created if it is not
    // there and positioned at its top ([P] file); replace and append belong to
    // `write` alone.
    if (wantRead && wantWrite && (wantAppend || wantReplace))
      return err("file open: `replace` and `append` are allowed only with `write`, not with "
                 "`read write`",
                 "syntax_error");

    SessionState::FileHandle fh;
    fh.path = path;
    fh.write = wantWrite;
    fh.append = wantAppend;
    fh.binary = wantBinary;
    fh.readWrite = wantRead && wantWrite;
    fh.hilo = nativeHilo();
    if (fh.readWrite) {
      fh.fp = std::fopen(path.c_str(), wantBinary ? "r+b" : "r+");
      if (fh.fp == nullptr) fh.fp = std::fopen(path.c_str(), wantBinary ? "w+b" : "w+");
    } else if (wantWrite) {
      // Without `replace` Stata refuses to clobber an existing file — the same
      // no-silent-overwrite rule every other write verb in this codebase honours.
      if (!wantAppend && !wantReplace) {
        if (std::FILE* probe = std::fopen(path.c_str(), "r")) {
          std::fclose(probe);
          return errRc("file " + typed + " already exists", "602");
        }
      }
      fh.fp = std::fopen(path.c_str(), wantAppend ? (wantBinary ? "ab" : "a")
                                                  : (wantBinary ? "wb" : "w"));
    } else {
      fh.fp = std::fopen(path.c_str(), wantBinary ? "rb" : "r");
    }
    if (fh.fp == nullptr) {
      if (!wantWrite) return errRc("file " + typed + " not found", "601");
      return errRc("file " + typed + " could not be opened", "603");
    }
    session.fileHandles[handle] = fh;
    return ojson();
  }

  auto it = session.fileHandles.find(handle);
  if (it == session.fileHandles.end()) return errRc("file handle " + handle + " not found", "111");
  SessionState::FileHandle& fh = it->second;

  // ---- seek ----------------------------------------------------------------
  // BUG-1526: `file seek h query|tof|eof|#` moves to the byte # (the first byte
  // is 0), to the top or to the end, and every form posts r(loc), the position
  // after it (StataBE 19.5 x3.do E1-E5, E15, E19; a text file seeks in bytes
  // too, E25-E27). Past the end of a read file is r(651) "may not seek past end
  // of file", with r() cleared and the file left at its end (E20, E21); a
  // write-append file is r(651) (E48); a negative # is r(198) (E22).
  if (sub == "seek") {
    if (fh.append) return errRc("may not seek in write-append file", "651");
    const std::string what = strutil::lower(trimc(rest));
    long target = 0;
    if (what == "query") {
      target = std::ftell(fh.fp);
    } else if (what == "tof") {
      target = 0;
    } else if (what == "eof") {
      std::fseek(fh.fp, 0, SEEK_END);
      target = std::ftell(fh.fp);
    } else if (!what.empty() && what.size() <= 18 &&
               std::all_of(what.begin(), what.end(),
                           [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
      target = std::stol(what);
      if (!fh.write) {
        std::fseek(fh.fp, 0, SEEK_END);
        if (target > std::ftell(fh.fp)) {
          eval::replaceRClass(session.eval);
          eval::declareRPosted(session.eval, "file seek", {});  // r(loc) reads `.` (E20)
          return errRc("may not seek past end of file", "651");
        }
      }
    } else {
      return errRc("invalid syntax", "198", "syntax_error");
    }
    if (what != "query") {
      std::fseek(fh.fp, target, SEEK_SET);
      fh.eof = false;
    }
    piet::eval::OrderedFloatDict r;
    r.set("loc", static_cast<double>(target));
    session.eval.rValues = r;
    eval::declareRPosted(session.eval, "file seek", {"loc"});
    return ojson();
  }

  // ---- set -----------------------------------------------------------------
  // BUG-1526: `file set h byteorder hilo|lohi|1|2` -- the order the multibyte
  // binary elements are written and read in from now on ([P] file; 1 = hilo,
  // most significant byte first). Anything else is r(198) (x3.do E29, E30).
  if (sub == "set") {
    std::istringstream iss(rest);
    std::vector<std::string> words;
    for (std::string w; iss >> w;) words.push_back(strutil::lower(w));
    if (words.size() != 2 || words[0] != "byteorder" ||
        (words[1] != "hilo" && words[1] != "lohi" && words[1] != "1" && words[1] != "2"))
      return errRc("invalid syntax", "198", "syntax_error");
    fh.hilo = words[1] == "hilo" || words[1] == "1";
    return ojson();
  }

  // ---- write ---------------------------------------------------------------
  // EM7 (help file: the text-output specs): "string" / `"string"', (exp),
  // %fmt (exp), _skip(#), _column(#), _newline[(#)], _char(#), _tab[(#)],
  // _page[(#)], _dup(#). Anything else is r(198) "invalid syntax" (`_s(2)`,
  // w2af_macro_text_file.log:1771-1777), and a %fmt outside the `format`
  // grammar is r(120) "invalid %format" (`%~9.2f`, `%s`). A failing item stops
  // the statement but what came before it is written: StataBE 19.5 leaves
  // `F[` in the file for `file write h "F[" _s(2) "]"`, and the text before
  // an r(120) or an r(111) likewise. A %fmt in front of a quoted string is
  // ignored (:1838 `abc!abc!`).
  if (sub == "write") {
    if (!fh.write) return err("file write: `" + handle + "` is open for reading", "file_wrong_mode");
    if (fh.binary) return writeBinary(fh, rest, session);
    std::string out;
    std::string pendingFmt;
    long long dup = 1;
    // characters since the last end-of-line in this statement (for _column)
    auto column = [&]() -> size_t {
      const size_t nl = out.rfind('\n');
      return nl == std::string::npos ? out.size() : out.size() - nl - 1;
    };
    auto emit = [&](const std::string& piece) {
      for (long long k = 0; k < dup; ++k) out += piece;
      dup = 1;
    };
    // A refusal still writes the items before it (see above).
    auto fail = [&](ojson e) {
      std::fwrite(out.data(), 1, out.size(), fh.fp);
      return e;
    };
    auto invalid = [&]() { return fail(errRc("invalid syntax", "198", "syntax_error")); };
    for (const std::string& item : splitWriteItems(rest)) {
      if (item.empty()) continue;
      if (item[0] == '"' || eval::startsCompound(item, 0)) {
        pendingFmt.clear();
        emit(dequote(item));
        continue;
      }
      if (item[0] == '%') {
        // A %fmt outside the `format` grammar is r(120), a binary element's format
        // among them (BUG-1526, StataBE 19.5 x3.do E23: `%4bu` in a text file).
        if (!piet::handlers::isValidDisplayFormat(item)) return fail(errRc("invalid %format", "120"));
        pendingFmt = item;
        continue;
      }
      if (item[0] == '(') {
        const std::string expr = item.size() >= 2 ? item.substr(1, item.size() - 2) : "";
        std::string raised;
        bool isStr = false;
        auto v = eval::evalArith(expr, &session.eval, &raised, &isStr);
        if (!v) {
          eval::MacroFnError fe;
          if (eval::classifyFailedExpr(expr, raised, &fe, &session.eval))
            return fail(errRc(fe.message, fe.rc));
          return invalid();
        }
        const std::string text = formatWriteValue(pendingFmt, *v, isStr);
        pendingFmt.clear();
        emit(text);
        continue;
      }
      if (item[0] != '_') return invalid();
      static const std::regex reDir(R"(^(_[A-Za-z]+)\s*(?:\(\s*(-?\d+)\s*\))?$)");
      std::smatch md;
      if (!std::regex_match(item, md, reDir)) return invalid();
      const std::string name = strutil::lower(md[1].str());
      const bool hasArg = md[2].matched;
      const long long arg = hasArg ? std::stoll(md[2].str()) : 1;
      auto isAbbrev = [&](const std::string& full, size_t minLen) {
        return name.size() >= minLen && name.size() <= full.size() &&
               full.compare(0, name.size(), name) == 0;
      };
      pendingFmt.clear();
      if (isAbbrev("_newline", 2)) {
        emit(std::string(static_cast<size_t>(std::max<long long>(arg, 0)), '\n'));
      } else if (name == "_tab") {
        emit(std::string(static_cast<size_t>(std::max<long long>(arg, 0)), '\t'));
      } else if (name == "_page") {
        emit(std::string(static_cast<size_t>(std::max<long long>(arg, 0)), '\f'));
      } else if (name == "_skip" && hasArg) {
        emit(std::string(static_cast<size_t>(std::max<long long>(arg, 0)), ' '));
      } else if (isAbbrev("_column", 4) && hasArg) {
        const size_t col = column();
        if (arg > 1 && static_cast<size_t>(arg - 1) > col)
          emit(std::string(static_cast<size_t>(arg - 1) - col, ' '));
        else
          dup = 1;
      } else if (name == "_char" && hasArg) {
        if (arg < 0 || arg > 255) return invalid();
        emit(std::string(1, static_cast<char>(arg & 0xFF)));
      } else if (name == "_dup" && hasArg) {
        if (arg < 0) return invalid();
        dup = arg;
      } else {
        return invalid();
      }
    }
    if (fh.readWrite) std::fseek(fh.fp, 0, SEEK_CUR);  // a write may follow a read
    if (std::fwrite(out.data(), 1, out.size(), fh.fp) != out.size())
      return err("file write: write to `" + fh.path + "` failed", "file_write_failed");
    return ojson();
  }

  // ---- read ----------------------------------------------------------------
  // `file read <h> <localname>`: one LINE into the local, r(eof) set to 0/1.
  if (fh.write && !fh.readWrite)
    return err("file read: `" + handle + "` is open for writing", "file_wrong_mode");
  if (fh.binary) return readBinary(fh, rest, session);
  if (fh.readWrite) std::fseek(fh.fp, 0, SEEK_CUR);  // a read may follow a write
  const std::string localName = trimc(rest);
  if (localName.empty())
    return err("file read: a local macro name to read into is required", "syntax_error");

  std::string line;
  int c = 0;
  bool any = false;
  while ((c = std::fgetc(fh.fp)) != EOF) {
    any = true;
    if (c == '\n') break;
    if (c != '\r') line.push_back(static_cast<char>(c));
  }
  fh.eof = !any;
  session.eval.locals.set(localName, fh.eof ? std::string() : line);
  // r(eof) is what a Stata read loop tests. Setting it through the same r-store
  // every r-class command uses is the whole wiring — BUG-344's lesson was that a
  // value computed but never published is a value that does not exist.
  piet::eval::OrderedFloatDict r;
  r.set("eof", fh.eof ? 1.0 : 0.0);
  session.eval.rValues = r;
  return ojson();
}

}  // namespace piet::handlers
