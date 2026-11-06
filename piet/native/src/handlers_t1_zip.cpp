// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane ZIP: the ZIP verbs over zip_io.h -- unzipfile zipfile zipuse
// zipsave. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log (arms zip_*) and
// w2ax_t1_community.log (arms zipsave_*/zipuse_*), StataBE 19.5, plus the lane's own StataBE
// probe zp1 (recorded in selftest_w2_t1_zip.cpp's comments where a check rests on it).
//
// NO OS PROCESS. Stata reaches the archive through a Java plugin (zipfile/unzipfile) or the
// OS `zip`/`unzip` (the zipsave package); PIET does it in-process through the ONE container
// implementation it has (zip_io.h: the central-directory walk, INFLATE and the STORED writer
// xlsx.cpp has always carried). Datasets go through the existing verbs: zipsave is a
// dispatched `save` to a PIET-owned scratch .dta, zipuse a dispatched `use` of one, so the
// .dta writer and reader are the ones every other command uses.
//
//   zipfile    implements zipfile per its help file: files, wildcards (`*`/`?` in the last
//              path component, dot-files included) and directories (walked recursively; only
//              the files become entries, zp1 dirzip), `saving(name[, replace])` with `.zip`
//              appended only to a name without a suffix (zp1 suffix), a missing file skipped
//              silently while at least one is found (zp1 partial). Entry names are normalised
//              (`sub/../f.txt` is `f.txt`); a symbolic link is listed and skipped, never
//              followed. Entries are STORED -- the build has no DEFLATE writer -- and every
//              `adding:` line says so.
//   unzipfile  implements unzipfile per its help file: `.zip` appended to a name without a
//              suffix, entries written below the working directory with their directories
//              created (zp1 nodir), an existing file without `replace` skipped with Stata's
//              message at rc 0. Unlike StataBE (zp1 evil, abs: it writes them), an entry
//              whose name is absolute or holds a `..` segment is refused by name before
//              anything is written.
//   zipsave    implements zipsave per its help file: `name` gets `.dta.zip` only when it has
//              no `.` (zp1 zs_dot), no name means the current dataset's file (rc 0 with
//              "invalid file specification" when there is none, zp1 zs_nofile), other
//              options go to `save` (zp1 zs_opt/zs_badopt). The single entry is named after
//              the target without its `.zip` (StataBE names it after its own tempfile,
//              `var/folders/.../S_17264.000001`, which carries nothing a reader can use).
//   zipuse     implements zipuse per its help file: the same naming rule (`zipuse t1zs.zip`
//              looks for t1zs.zip itself, r(601), w2ax_t1_community.log zipuse_ext), `[varlist
//              [if] [in]] using name`, `dtafile()` to pick one dataset from a many-file
//              archive (otherwise the first file entry, as Stata's `unzip -p` pipe reads it),
//              other options to `use`; zipsave and zipuse both make the archive the dataset's
//              current file. Without `clear` it refuses only when the data
//              have changed since the last load or save -- r(4), zp1 zu_changed -- and loads
//              otherwise (zipuse_noclear rc 0), because its own `use` always carries `clear`.
//
// The defect classes of handlers_aer_util.cpp's header apply verbatim: every return is a
// block object with a "type" key or an array of them (SHAPE); every rc other than 198/199
// travels in the `reason` channel as "r(NNN)" (RC); all four command tokens register their
// vocabularies at namespace scope (VOCABULARY). Scratch files are minted through
// mintScratchPath (registered, so the orphan sweep knows them) and removed on every path.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / from_native_path
#include "piet/dispatcher.h"  // zipsave/zipuse: a dispatched `save`/`use`
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma
#include "piet/handlers.h"
#include "piet/scratch_seq.h"  // mintScratchPath
#include "piet/session_state.h"
#include "zip_io.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
namespace {

// zipfile's `syntax` line: SAVING(string asis) [COMPLevel(numlist integer >=0 <10 max=1)].
// complevel() is READ: it is validated, and the entries it would compress are written stored
// all the same -- each `adding:` line says `(stored 0%)`, which is the truth about the bytes.
const piet::eval::OptVocab& kVocabZipfile =
    piet::eval::registerOptVocab({"zipfile"}, {{"saving", "complevel"}, {}});

// unzipfile: replace, plus ifilter()/efilter(), which select entries by a java.util.regex
// pattern -- a dialect this build does not evaluate, so both are refused by name.
const piet::eval::OptVocab& kVocabUnzipfile =
    piet::eval::registerOptVocab({"unzipfile"}, {{"replace"}, {"ifilter", "efilter"}});

// zipsave: `[, replace *]` -- every other option is save's, and save judges it.
const piet::eval::OptVocab& kVocabZipsave =
    piet::eval::registerOptVocab({"zipsave"}, {{"replace"}, {}});

// zipuse: `[, clear dtafile(str) *]` -- every other option is use's, and use judges it.
const piet::eval::OptVocab& kVocabZipuse =
    piet::eval::registerOptVocab({"zipuse"}, {{"clear", "dtafile"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

namespace fs = std::filesystem;

// The writer's limits (zip_io.h): 16-bit entry counts, 32-bit sizes.
constexpr size_t kMaxEntries = 65535;
constexpr std::uint64_t kMaxEntryBytes = 0xFFFFFFFFull;

ojson zipError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson displayText(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  }
  return nullptr;
}

std::string trimCopy(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// The text after the command word and before the first unquoted, unparenthesised comma.
std::string preCommaText(const std::string& raw) {
  std::string s = trimCopy(raw);
  const long long comma = eval::findUnquotedComma(s);
  if (comma >= 0) s = s.substr(0, static_cast<size_t>(comma));
  size_t i = 0;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return trimCopy(s.substr(i));
}

// Blank-separated words; a double-quoted run (or Stata's compound `"..."') is one word with
// its quotes removed.
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '`' && i + 1 < text.size() && text[i + 1] == '"') {
      const size_t j = text.find("\"'", i + 2);
      if (j == std::string::npos) {
        out.push_back(text.substr(i + 2));
        break;
      }
      out.push_back(text.substr(i + 2, j - i - 2));
      i = j + 2;
      continue;
    }
    if (c == '"') {
      const size_t j = text.find('"', i + 1);
      if (j == std::string::npos) {
        out.push_back(text.substr(i + 1));
        break;
      }
      out.push_back(text.substr(i + 1, j - i - 1));
      i = j + 1;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    out.push_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

std::string stripQuotes(std::string s) {
  s = trimCopy(s);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

// The last path component (both separators), and its suffix (`.ext`, "" when none).
std::string baseName(const std::string& p) {
  const size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}
bool hasDot(const std::string& p) { return baseName(p).find('.') != std::string::npos; }
// Mata's pathsuffix(): the text from the last `.` of the last component, or "".
std::string pathSuffix(const std::string& p) {
  const std::string b = baseName(p);
  const size_t dot = b.rfind('.');
  return dot == std::string::npos ? std::string() : b.substr(dot);
}

bool exists(const std::string& path) {
  std::error_code ec;
  return fs::exists(cecon::to_native_path(path), ec) && !ec;
}
bool isDir(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(cecon::to_native_path(path), ec) && !ec;
}
bool isRegular(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(cecon::to_native_path(path), ec) && !ec;
}

std::optional<std::string> readFileBytes(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return std::nullopt;
  std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (f.bad()) return std::nullopt;
  return buf;
}

void removeQuietly(const std::string& path) {
  std::error_code ec;
  fs::remove(cecon::to_native_path(path), ec);
}

// `*` and `?` over one path component, case-sensitive (Mata's dir() pattern).
bool globMatch(const char* p, const char* s) {
  for (; *p != '\0'; ++p, ++s) {
    if (*p == '*') {
      while (p[1] == '*') ++p;
      if (p[1] == '\0') return true;
      for (; *s != '\0'; ++s)
        if (globMatch(p + 1, s)) return true;
      return globMatch(p + 1, s);
    }
    if (*s == '\0') return false;
    if (*p != '?' && *p != *s) return false;
  }
  return *s == '\0';
}

// The option text a pass-through verb (`save`, `use`) receives: every option this command
// does not own, re-spelled as typed.
std::string passThroughOptions(const eval::ParsedCommand& parsed,
                               const std::set<std::string>& owned) {
  std::string out;
  for (const auto& [k, v] : parsed.opts) {
    if (owned.count(k)) continue;
    out += " " + k;
    if (!v.isBool) out += "(" + v.strValue + ")";
  }
  return out;
}

bool hasOpt(const eval::ParsedCommand& parsed, const std::string& key) {
  for (const auto& [k, v] : parsed.opts)
    if (k == key) return true;
  return false;
}

std::optional<std::string> optString(const eval::ParsedCommand& parsed, const std::string& key) {
  for (const auto& [k, v] : parsed.opts)
    if (k == key) return v.isBool ? std::string() : v.strValue;
  return std::nullopt;
}

// An entry name a writer may use below the working directory: relative, no `..` segment.
// On Windows a backslash also separates and a drive prefix (`C:`) is absolute; on POSIX both
// are ordinary file-name characters (StataBE extracts `a:b.txt` as is, review st_zf colon).
bool unsafeEntryName(const std::string& name) {
  if (name.empty()) return true;
#ifdef _WIN32
  const char* seps = "/\\";
  if (name[0] == '\\') return true;
  if (name.size() >= 2 && name[1] == ':') return true;
#else
  const char* seps = "/";
#endif
  if (name[0] == '/') return true;
  size_t i = 0;
  while (i <= name.size()) {
    size_t j = name.find_first_of(seps, i);
    if (j == std::string::npos) j = name.size();
    if (j - i == 2 && name.compare(i, 2, "..") == 0) return true;
    i = j + 1;
  }
  return false;
}

// A user path as an entry name: separators to `/`, normalised lexically (`sub/../f.txt` is
// `f.txt`, as StataBE stores it -- review st_zf dotdot), then a leading `/`, `./` or `../`
// run removed (an archive never names a place above where it is unpacked).
std::string entryNameFor(std::string spelled) {
  std::replace(spelled.begin(), spelled.end(), '\\', '/');
  std::string n = fs::path(spelled).lexically_normal().generic_string();
  while (true) {
    if (!n.empty() && n[0] == '/') {
      n.erase(0, 1);
    } else if (n.compare(0, 2, "./") == 0) {
      n.erase(0, 2);
    } else if (n.compare(0, 3, "../") == 0) {
      n.erase(0, 3);
    } else {
      break;
    }
  }
  if (n == "." || n == "..") n.clear();
  return n;
}

// Stata's numlist number: optional sign, digits with an optional point, optional exponent.
// (std::stod alone also reads `0x2`, `inf` and `nan`.)
bool strictNumber(const std::string& t, double* out) {
  static const std::regex re(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  if (!std::regex_match(t, re)) return false;
  try {
    *out = std::stod(t);
  } catch (...) {
    return false;
  }
  return true;
}

// The leading bytes of a Stata dataset: `<stata_dta>` (formats 117-119), or a format byte
// 102-115 followed by a byte-order byte 1/2 (the older binary formats).
bool looksLikeDta(const std::string& b) {
  if (b.compare(0, 11, "<stata_dta>") == 0) return true;
  if (b.size() < 4) return false;
  const unsigned char v = static_cast<unsigned char>(b[0]);
  const unsigned char o = static_cast<unsigned char>(b[1]);
  return v >= 102 && v <= 115 && (o == 1 || o == 2);
}

// Words at top level: blanks split, a quoted run or a parenthesised run stays whole
// (`if inlist(x, 1, 2)` keeps its argument list).
std::vector<std::string> splitTopLevelWords(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQ = false;
  for (char c : text) {
    if (c == '"') inQ = !inQ;
    if (!inQ && c == '(') ++depth;
    if (!inQ && c == ')' && depth > 0) --depth;
    if (!inQ && depth == 0 && (c == ' ' || c == '\t')) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// A dataset-scratch path the orphan sweep knows (registered at mint), with the extension
// `use`/`save` read the format from.
std::string mintDtaScratch(const SessionState& session, const char* prefix) {
  return mintScratchPath(session.workdir(), prefix) + ".dta";
}

// ---------------------------------------------------------------------------
// zipfile's collection step. Each spelled item (relative to the working directory as typed)
// becomes {entry name, resolved path} pairs; directories are walked. A symbolic link is never
// followed: StataBE lists it and skips it with "error reading" (review st_lp: a `self -> .`
// link), which keeps a link cycle from recursing.
struct ToZip {
  std::string entry;
  std::string path;
  bool link = false;  // a symbolic link: reported and skipped
};

bool isSymlink(const std::string& path) {
  std::error_code ec;
  return fs::symlink_status(cecon::to_native_path(path), ec).type() == fs::file_type::symlink;
}

// The directory's own entries, sorted by name, with their kind read WITHOUT following links.
struct Kid {
  std::string name;
  bool dir = false;
  bool link = false;
  bool operator<(const Kid& o) const { return name < o.name; }
};
std::vector<Kid> listDir(const std::string& resolved) {
  std::vector<Kid> kids;
  std::error_code ec;
  for (fs::directory_iterator it(cecon::to_native_path(resolved), ec), end; !ec && it != end;
       it.increment(ec)) {
    Kid k;
    k.name = cecon::from_native_path(it->path().filename());
    std::error_code ec2;
    const fs::file_type t = it->symlink_status(ec2).type();
    k.link = t == fs::file_type::symlink;
    k.dir = t == fs::file_type::directory;
    kids.push_back(std::move(k));
  }
  std::sort(kids.begin(), kids.end());
  return kids;
}

void addKid(const std::string& sp, const std::string& rp, const Kid& k, std::vector<ToZip>* out);

// Every file below a directory, dot-files included (review st_zf subdir: `sub/.h.txt`); an
// empty directory contributes nothing (review st_zf emptyd: 0 archived, rc 0).
void walkDirectory(const std::string& spelled, const std::string& resolved,
                   std::vector<ToZip>* out) {
  for (const Kid& k : listDir(resolved)) addKid(spelled + "/" + k.name, resolved + "/" + k.name, k, out);
}

void addKid(const std::string& sp, const std::string& rp, const Kid& k, std::vector<ToZip>* out) {
  if (k.link) {
    out->push_back(ToZip{entryNameFor(sp), rp, true});
  } else if (k.dir) {
    walkDirectory(sp, rp, out);
  } else {
    out->push_back(ToZip{entryNameFor(sp), rp, false});
  }
}

// One file-list word: an exact name, or a `*`/`?` pattern in its last component. `*found`
// says whether it named anything at all (a file, a directory -- even an empty one -- or a
// link); a word that names nothing is skipped (zp1 partial).
std::optional<ojson> collect(const SessionState& session, const std::string& word,
                             std::vector<ToZip>* out, bool* found) {
  std::string spelled = word;
  std::replace(spelled.begin(), spelled.end(), '\\', '/');
  while (spelled.size() > 1 && spelled.back() == '/') spelled.pop_back();
  const size_t slash = spelled.rfind('/');
  const std::string dirPart = slash == std::string::npos ? std::string() : spelled.substr(0, slash + 1);
  const std::string pattern = slash == std::string::npos ? spelled : spelled.substr(slash + 1);
  if (dirPart.find_first_of("*?") != std::string::npos)
    return zipError("zipfile: a wildcard in a directory part (" + word +
                        ") is not supported in this build; name the directories",
                    198);
  if (pattern.find_first_of("*?") == std::string::npos) {
    const std::string rp = resolveUserPath(session, spelled);
    Kid k;
    k.link = isSymlink(rp);
    k.dir = !k.link && isDir(rp);
    if (k.link || k.dir || isRegular(rp)) {
      *found = true;
      addKid(spelled, rp, k, out);
    }
    return std::nullopt;
  }
  // Mata's dir() over the pattern: dot-files match `*` too (review st_zf hidden).
  const std::string rdir = resolveUserPath(session, dirPart.empty() ? std::string(".") : dirPart);
  for (const Kid& k : listDir(rdir)) {
    if (!globMatch(pattern.c_str(), k.name.c_str())) continue;
    *found = true;
    addKid(dirPart + k.name, rdir + "/" + k.name, k, out);
  }
  return std::nullopt;
}

// complevel(): `numlist integer >=0 <10 max=1 min=1`, with numlist's codes and zipfile's
// "complevel() invalid -- " prefix (review st_zf cl05/cl50 rc 0, cl15 126, clx 121, cl12 123;
// zp2 zf_badcl 125).
std::optional<ojson> checkComplevel(const std::string& spec) {
  std::string flat = spec;
  for (char& c : flat)
    if (c == ',') c = ' ';
  const std::vector<std::string> toks = words(flat);
  const std::string pre = "complevel() invalid -- ";
  if (toks.empty()) return zipError(pre + "too few numbers", 122);
  std::vector<double> v;
  for (const std::string& t : toks) {
    double d = 0.0;
    if (!strictNumber(t, &d)) return zipError(pre + "invalid number", 121);
    v.push_back(d);
  }
  if (v.size() > 1) return zipError(pre + "single number required", 123);
  if (v[0] != std::floor(v[0])) return zipError(pre + "invalid number, noninteger value", 126);
  if (v[0] < 0.0 || v[0] >= 10.0)
    return zipError(pre + "invalid number, outside of allowed range", 125);
  return std::nullopt;
}

std::string countLines(size_t processed, size_t skipped, const char* doneLabel, size_t done) {
  return "total processed:  " + std::to_string(processed) + "\n        skipped:  " +
         std::to_string(skipped) + "\n" + doneLabel + std::to_string(done);
}

}  // namespace

// `zipfile files, saving(name[, replace]) [complevel(#)]`.
nlohmann::ordered_json runZipfile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabZipfile.known,
                                            kVocabZipfile.unimplemented))
    return ojson(*err);
  const std::vector<std::string> items = words(preCommaText(parsed.raw));
  if (items.empty()) return zipError("files or directories required", 100);
  const std::optional<std::string> saving = optString(parsed, "saving");
  if (!saving.has_value()) return zipError("option saving() required", 198);
  if (auto cl = optString(parsed, "complevel")) {
    if (auto e = checkComplevel(*cl)) return *e;
  }

  // saving(): a filename, then optionally `, replace` (zipfile's own sub-parser).
  std::string target = *saving;
  bool replace = false;
  {
    const long long comma = eval::findUnquotedComma(target);
    if (comma >= 0) {
      const std::string sub = trimCopy(target.substr(static_cast<size_t>(comma) + 1));
      target = target.substr(0, static_cast<size_t>(comma));
      if (sub != "replace") return zipError("invalid option " + sub + " in option saving()", 198);
      replace = true;
    }
    target = stripQuotes(target);
  }
  if (baseName(target).empty()) return zipError("invalid filename", 198);
  if (pathSuffix(target).empty()) target += ".zip";
  const std::string targetPath = resolveUserPath(session, target);
  if (exists(targetPath) && !replace)
    return zipError(
        "file already exists\n    You must specify option replace to overwrite the existing file.",
        130);

  std::vector<ToZip> todo;
  bool found = false;
  for (const std::string& w : items) {
    std::string t = w;
    std::replace(t.begin(), t.end(), '\\', '/');
    if (t == target)
      return zipError("invalid filename\n    " + target +
                          " may not be the name of a file to compress and save.",
                      198);
    if (auto e = collect(session, w, &todo, &found)) return *e;
  }
  if (!found)
    return zipError(
        "invalid file specification\n    You must specify at least one valid file or folder to "
        "include in the zip file",
        198);
  if (todo.size() > kMaxEntries)
    return zipError("zipfile: " + std::to_string(todo.size()) +
                        " entries; this build writes at most 65535 entries per archive (no ZIP64)",
                    198);
  {
    std::set<std::string> seen;
    for (const ToZip& t : todo)
      if (!t.link && !seen.insert(t.entry).second)
        return zipError("duplicate entry: " + t.entry, 692);
  }
  // A target in a directory that does not exist (review st_zf nodir: r(601)).
  {
    const fs::path parent = fs::path(cecon::to_native_path(targetPath)).parent_path();
    if (!parent.empty() && !isDir(cecon::from_native_path(parent)))
      return zipError(target + " (No such file or directory)", 601);
  }

  std::vector<zipio::Entry> entries;
  std::string lines;
  size_t skipped = 0;
  for (const ToZip& t : todo) {
    if (t.link) {
      lines += "    adding: " + t.entry + "\n    error reading " + t.entry + "; skipping\n";
      ++skipped;
      continue;
    }
    zipio::Entry e;
    e.name = t.entry;
    {
      std::error_code ec;
      const std::uintmax_t sz = fs::file_size(cecon::to_native_path(t.path), ec);
      if (!ec && sz >= kMaxEntryBytes)
        return zipError("zipfile: " + t.entry +
                            " is 4 GiB or larger; this build writes no ZIP64 entries",
                        198);
      std::optional<std::string> bytes = readFileBytes(t.path);
      if (!bytes.has_value()) return zipError("file " + t.entry + " could not be opened", 603);
      e.data = std::move(*bytes);
    }
    lines += "    adding: " + t.entry + " (stored 0%)\n";
    entries.push_back(std::move(e));
  }
  try {
    zipio::commit(targetPath, zipio::build(entries));
  } catch (const std::exception& ex) {
    return zipError("file " + target + " could not be opened: " + ex.what(), 603);
  }
  return displayText(lines + "\n" +
                     countLines(entries.size() + skipped, skipped, "       archived:  ",
                                entries.size()));
}

// `unzipfile zipfile [, replace]`.
nlohmann::ordered_json runUnzipfile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabUnzipfile.known,
                                            kVocabUnzipfile.unimplemented))
    return ojson(*err);
  const std::vector<std::string> w = words(preCommaText(parsed.raw));
  if (w.empty()) return zipError("zipfile required", 100);
  if (w.size() > 1) return zipError("invalid syntax", 198);
  std::string name = w[0];
  {
    std::string low = name;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0 ||
        low.rfind("ftp://", 0) == 0)
      return ojson(eval::refusedCommand("unzipfile", "no_network"));
  }
  if (pathSuffix(name).empty()) name += ".zip";
  const std::string zipPath = resolveUserPath(session, name);
  if (!isRegular(zipPath)) return zipError("file " + name + " not found", 601);

  std::vector<zipio::Entry> entries;
  try {
    entries = zipio::readEntries(zipPath);
  } catch (const std::exception& ex) {
    const std::string why = ex.what();
    if (why.find("is not a .zip file") != std::string::npos)
      return zipError("zip END header not found\n    File " + name + " not a zip archive.", 603);
    return zipError("unzipfile: " + name + ": " + why, 603);
  }
  // Nothing is written unless every entry lands below the working directory.
  for (const auto& e : entries) {
    if (unsafeEntryName(e.name))
      return zipError("unzipfile: entry \"" + e.name + "\" in " + name +
                          " names a place outside the current directory (an absolute path or a "
                          "`..` segment); nothing was extracted",
                      198);
  }
  const bool replace = hasOpt(parsed, "replace");
  const std::string root = resolveUserPath(session, ".");
  std::string lines;
  size_t processed = 0, skipped = 0, extracted = 0;
  for (const auto& e : entries) {
    std::string rel = e.name;
#ifdef _WIN32
    std::replace(rel.begin(), rel.end(), '\\', '/');
#endif
    const std::string dest = root + "/" + rel;
    if (!rel.empty() && rel.back() == '/') {
      std::error_code ec;
      fs::create_directories(cecon::to_native_path(dest), ec);
      if (ec) return zipError("unzipfile: could not create directory " + rel, 693);
      continue;
    }
    ++processed;
    {
      const fs::path parent = fs::path(cecon::to_native_path(dest)).parent_path();
      std::error_code ec;
      if (!parent.empty()) fs::create_directories(parent, ec);
      if (ec) return zipError("unzipfile: could not create the directory for " + rel, 693);
    }
    if (exists(dest) && !replace) {
      lines += "    can't overwrite \"" + rel + "\"; skipping\n";
      ++skipped;
      continue;
    }
    if (isDir(dest)) return zipError("unzipfile: " + rel + " is a directory", 693);
    try {
      zipio::commit(dest, e.data);
    } catch (const std::exception& ex) {
      return zipError("unzipfile: " + rel + ": " + ex.what(), 693);
    }
    lines += "    inflating: " + rel + "\n";
    ++extracted;
  }
  return displayText(lines + "\nsuccessfully unzipped " + name + " to current directory\n" +
                     countLines(processed, skipped, "      extracted:  ", extracted));
}

// `zipsave [name] [, replace save_options]`.
nlohmann::ordered_json runZipsave(const eval::ParsedCommand& parsed, SessionState& session) {
  // No reject net here: zipsave's `syntax` ends in `*`, so every option it does not own
  // (kVocabZipsave: replace) is save's, and the dispatched `save` below judges it.
  (void)kVocabZipsave;
  const std::vector<std::string> w = words(preCommaText(parsed.raw));
  if (w.size() > 1) return zipError("invalid syntax", 198);
  std::string file;
  if (w.empty()) {
    // No name: the current dataset's file, as Stata's $S_FN.
    file = session.currentFile();
    if (file.empty()) return displayText("invalid file specification");  // rc 0 (zp1 zs_nofile)
  } else {
    file = w[0];
  }
  file.erase(std::remove(file.begin(), file.end(), '"'), file.end());
  if (!hasDot(file)) file += ".dta.zip";
  const bool replace = hasOpt(parsed, "replace");
  const std::string target = resolveUserPath(session, file);
  if (exists(target) && !replace) return zipError("file " + file + " already exists", 602);
  // Stata's own `save` of an empty dataset (review st_zu zs_nodata: r(111), no file).
  if (!session.hasData()) return zipError("no variables defined", 111);

  const std::string scratch = mintDtaScratch(session, "zipsave");
  removeQuietly(scratch);
  ojson saved = dispatch("quietly save \"" + scratch + "\", replace" +
                             passThroughOptions(parsed, {"replace"}),
                         session);
  if (const ojson* e = firstError(saved)) {
    removeQuietly(scratch);
    return *e;
  }
  std::optional<std::string> bytes = readFileBytes(scratch);
  removeQuietly(scratch);
  if (!bytes.has_value()) return zipError("zipsave: the saved dataset could not be read back", 693);
  if (bytes->size() >= kMaxEntryBytes)
    return zipError("zipsave: the dataset is 4 GiB or larger; this build writes no ZIP64 entries",
                    198);
  std::string entryName = baseName(file);
  if (entryName.size() > 4 && entryName.compare(entryName.size() - 4, 4, ".zip") == 0)
    entryName.resize(entryName.size() - 4);
  try {
    zipio::commit(target, zipio::build({zipio::Entry{entryName, std::move(*bytes)}}));
  } catch (const std::exception& ex) {
    return zipError("file " + file + " could not be opened: " + ex.what(), 603);
  }
  // The archive is now the dataset's file, as Stata's $S_FN: c(filename) names it, c(changed)
  // is 0, and a later name-less `zipsave` writes there (review st_zu zs1/zs_noname).
  session.setCurrentFile(target);
  return displayText("data compressed with zip and saved in file " + file);
}

// `zipuse name [, clear dtafile() use_options]` and `zipuse varlist [if] [in] using name, ...`.
nlohmann::ordered_json runZipuse(const eval::ParsedCommand& parsed, SessionState& session) {
  // No reject net here: zipuse's `syntax` ends in `*`, so every option it does not own
  // (kVocabZipuse: clear dtafile) is use's, and the dispatched `use` below judges it.
  (void)kVocabZipuse;
  const std::string pre = preCommaText(parsed.raw);
  // `[varlist [if] [in]] using name`: the first unquoted `using` word splits the two.
  std::string initlist, fileText = pre;
  {
    size_t i = 0;
    bool inQ = false;
    while (i < pre.size()) {
      if (pre[i] == '"') inQ = !inQ;
      if (!inQ && (i == 0 || std::isspace(static_cast<unsigned char>(pre[i - 1]))) &&
          pre.compare(i, 5, "using") == 0 &&
          (i + 5 == pre.size() || std::isspace(static_cast<unsigned char>(pre[i + 5])))) {
        initlist = trimCopy(pre.substr(0, i));
        fileText = trimCopy(pre.substr(i + 5));
        break;
      }
      ++i;
    }
  }
  const std::vector<std::string> w = words(fileText);
  if (w.empty()) return zipError("something required", 100);  // zp2 zu_empty
  if (w.size() > 1) return zipError("invalid syntax", 198);
  std::string file = w[0];
  file.erase(std::remove(file.begin(), file.end(), '"'), file.end());
  if (!hasDot(file)) file += ".dta.zip";
  // The subset before `using`: a varlist, then `if` and `in` qualifiers in either order.
  std::string subVars, subIf, subIn;
  {
    std::string* cur = &subVars;
    for (const std::string& tok : splitTopLevelWords(initlist)) {
      if (tok == "if") {
        cur = &subIf;
        continue;
      }
      if (tok == "in") {
        cur = &subIn;
        continue;
      }
      *cur += (cur->empty() ? "" : " ") + tok;
    }
    if ((initlist.find(" if ") != std::string::npos || initlist.rfind("if ", 0) == 0) &&
        trimCopy(subIf).empty())
      return zipError("invalid syntax", 198);
  }
  std::optional<std::string> dtafile = optString(parsed, "dtafile");
  if (dtafile.has_value()) {
    *dtafile = stripQuotes(*dtafile);
    dtafile->erase(std::remove(dtafile->begin(), dtafile->end(), '"'), dtafile->end());
    if (!hasDot(*dtafile)) *dtafile += ".dta";
  }
  const std::string zipPath = resolveUserPath(session, file);
  if (!isRegular(zipPath)) return zipError("file not found", 601);
  if (!hasOpt(parsed, "clear") && session.hasData() && session.datasetChanged)
    return zipError("no; dataset in memory has changed since last saved", 4);

  std::vector<zipio::Entry> entries;
  try {
    entries = zipio::readEntries(zipPath);
  } catch (const std::exception& ex) {
    // An archive this build cannot read (ZIP64, encrypted, an unknown method) is refused by
    // name at r(603), as unzipfile refuses it; anything else unreadable is what Stata's empty
    // `unzip -p` pipe makes of it, "not Stata format", r(610).
    const std::string why = ex.what();
    return zipError("zipuse: " + file + ": " + why,
                    why.find("not supported") != std::string::npos ? 603 : 610);
  }
  // Without dtafile(), the first file entry: Stata pipes every entry through `unzip -p` and
  // its `use` reads the dataset at the front (review st_zu finder: a Finder archive with a
  // `__MACOSX/._x.dta` entry after the dataset loads, 74 obs; two: rc 0).
  const zipio::Entry* pick = nullptr;
  for (const auto& e : entries) {
    if (!e.name.empty() && e.name.back() == '/') continue;
    if (dtafile.has_value() ? e.name == *dtafile : true) {
      pick = &e;
      break;
    }
  }
  // zp2 zu_dtafile_missing / review st_zf notdta: nothing to read, or bytes that are not a
  // Stata dataset, are r(610).
  if (pick == nullptr || !looksLikeDta(pick->data))
    return zipError("file " + (pick != nullptr ? pick->name : (dtafile ? *dtafile : file)) +
                        " in " + file + " not Stata format",
                    610);

  const std::string scratch = mintDtaScratch(session, "zipuse");
  try {
    zipio::commit(scratch, pick->data);
  } catch (const std::exception& ex) {
    removeQuietly(scratch);
    return zipError("zipuse: could not stage " + file + ": " + ex.what(), 693);
  }
  // `use [varlist] [if] [in] using f, clear` as the whole file, then the subset through
  // `keep` (observations first, so `if` may name a variable the varlist leaves out). A
  // subset that fails leaves no data, as Stata's `use` does (zp2 use_badvar_left: k 0, N 0).
  ojson used = dispatch("quietly use \"" + scratch + "\", clear" +
                            passThroughOptions(parsed, {"clear", "dtafile"}),
                        session);
  removeQuietly(scratch);
  // The `use` memoised its conversion under the scratch path, which no later `use` can name;
  // left alone, each zipuse would pin one converted copy (and a worker stash) until the
  // byte-LRU evicts it. dropUseCaches() is the public release (the per-entry
  // dropUseCacheEntry is private to SessionState).
  session.dropUseCaches();
  if (const ojson* e = firstError(used)) return *e;
  std::vector<std::string> keeps;
  if (!subIf.empty() || !subIn.empty()) {
    std::string k = "quietly keep";
    if (!subIf.empty()) k += " if (" + subIf + ")";
    if (!subIn.empty()) k += " in " + subIn;
    keeps.push_back(k);
  }
  if (!subVars.empty()) keeps.push_back("quietly keep " + subVars);
  for (const std::string& k : keeps) {
    ojson r = dispatch(k, session);
    if (const ojson* e = firstError(r)) {
      ojson err = *e;
      dispatch("quietly clear", session);
      return err;
    }
  }
  // c(filename) is the archive, as Stata's $S_FN after zipuse (zp1 zu_nolabel).
  session.setCurrentFile(zipPath);
  return ojson::array();
}

}  // namespace piet::handlers
