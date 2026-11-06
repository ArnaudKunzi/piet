// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `postfile` / `post` / `postclose` — C++ port of piet/backend/piet/
// postfile.py (S8, M3b). Stata's in-memory collector that accumulates
// scalar rows and writes an hbin on postclose.
//
// hbin WRITE: this monorepo's linked src/io/columnar.h is READ-ONLY (no
// writer API) — every OTHER M2/M3a mutation handler works around this by
// shelling out to `cobra eval --out FILE` (cobra owns hbin writing). postfile
// has no row-context cobra verb to piggyback on (each post is a bare scalar
// row, not a dataset transform), so postclose() builds a CSV of the
// accumulated rows and converts it via `cobra convert` — string columns
// forced to factor with --factor (bug-for-bug with the oracle's independent
// per-column dtype declaration: a postfile `str32` column stays a factor
// even if every value happens to look numeric; see is_string_col()).
#include "piet/handlers.h"
#include "piet/scratch_seq.h"  // BUG-389: registerScratchMint

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// BUG-124: runCobra() does not throw — failures come back in the RESULT (the
// old catch around the postclose convert was dead), so a failed convert left
// a torn hbin at the USER's target path that then passed the exists() check.
// Manual CobraError build from stderr — file-local copy per the established
// one-copy-per-TU convention (see egen.cpp / handlers_data_verbs.cpp).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

// postfile.py's _RE_TYPE: byte|int|long|float|double|str<N>|strL (case-ins).
bool isTypeToken(const std::string& tok) {
  static const std::regex re(R"(^(byte|int|long|float|double|str\d+|strL)$)", std::regex::icase);
  return std::regex_match(tok, re);
}

// _extract_paren_groups: depth-aware `(...)` group extractor (handles
// nested parens like `(r(N))` or a matrix ref `(b[1,1])`).
std::vector<std::string> extractParenGroups(const std::string& s) {
  std::vector<std::string> groups;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '(') {
      int depth = 1;
      ++i;
      size_t start = i;
      while (i < s.size() && depth > 0) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        ++i;
      }
      groups.push_back(s.substr(start, (i - 1) - start));
    } else {
      ++i;
    }
  }
  return groups;
}

// postfile.py's _RE_SAFE_ARITH: ^[0-9\s+\-*/().^eEa-zA-Z_,]+$
bool isSafeArith(const std::string& noSpace) {
  static const std::regex re(R"(^[0-9\+\-\*/\(\)\.\^eEa-zA-Z_,]+$)");
  return std::regex_match(noSpace, re);
}

// postfile.py's _eval_post_expr — evaluate ONE post-expression token.
// Returns (kind, text) where kind is "numeric" or "string"; numeric text is
// already formatted (may be "nan" sentinel via isNan flag).
struct PostExprResult {
  bool isString = false;
  bool isNan = false;
  std::string text;      // formatted numeric text OR the string value
};

PostExprResult evalPostExpr(const std::string& rawExpr, SessionState& session) {
  std::string expr = trim(rawExpr);
  PostExprResult r;

  if (expr == ".") {
    r.isNan = true;
    return r;
  }

  if (expr.size() >= 2 && ((expr.front() == '"' && expr.back() == '"') ||
                           (expr.front() == '\'' && expr.back() == '\''))) {
    r.isString = true;
    r.text = expr.substr(1, expr.size() - 2);
    return r;
  }

  // BUG-512: `post h (_N)` posted MISSING at rc 0 -- this evaluator keeps its
  // own namespace, `_N` was not in it, and a raised name is answered with nan
  // below. Same substitution the macro layer's two entry points run.
  std::string cleaned = eval::substituteObsSpecials(expr, session.eval);
  {
    std::string tmp;
    for (char c : cleaned) {
      if (c == '^') tmp += "**";
      else tmp.push_back(c);
    }
    cleaned = tmp;
  }
  static const std::regex reLn(R"(\bln\b)");
  cleaned = std::regex_replace(cleaned, reLn, "log");

  std::string lowerCleaned = lower(cleaned);
  if (cleaned.find('[') != std::string::npos || lowerCleaned.find("colsof") != std::string::npos ||
      lowerCleaned.find("rowsof") != std::string::npos ||
      lowerCleaned.find("el(") != std::string::npos) {
    cleaned = eval::substituteMatrixRefs(cleaned, session.eval);
  }

  std::string noSpace;
  for (char c : cleaned) {
    if (c != ' ') noSpace.push_back(c);
  }
  if (!isSafeArith(noSpace)) {
    // Contains characters outside the safe set — treat as string (oracle's
    // fallback: `return ("string", expr)` — the ORIGINAL expr, not cleaned).
    r.isString = true;
    r.text = expr;
    return r;
  }

  eval::PyNamespace ns = eval::mathNamespace(/*withInfNan=*/true);
  // Bare scalar references (Stata allows `post ... (myscalar)`) resolve against
  // the session scalar store as ordinary numeric operands. mathNamespace only
  // carries math constants + the `scalar(name)` function form, so inject the
  // numeric scalars here (without shadowing an existing math name).
  for (const auto& [sname, sval] : session.eval.scalars.items()) {
    if (ns.hasName(sname)) continue;
    std::optional<double> sf = eval::pythonFloat(sval);
    if (sf.has_value()) ns.consts[sname] = eval::PyValue::floating(*sf);
  }
  eval::PyEvalOutcome out = eval::evalPyExprDetailed(cleaned, ns);
  if (out.status == eval::PyEvalOutcome::Status::Raised) {
    r.isNan = true;  // `except Exception: return ("numeric", nan)`
    return r;
  }
  if (out.status == eval::PyEvalOutcome::Status::FnObject) {
    r.isString = true;
    r.text = "<built-in function " + out.fnName + ">";
    return r;
  }
  const eval::PyValue& v = out.value;
  if (v.isBool()) {
    r.text = v.boolVal() ? "1" : "0";
    return r;
  }
  if (v.isInt() || v.isFloat()) {
    double fval = v.isFloat() ? v.floatVal() : static_cast<double>(v.intVal());
    if (std::isnan(fval)) {
      r.isNan = true;
      return r;
    }
    r.text = eval::pythonFloatRepr(fval);  // exact double round-trip (postclose parses back)
    return r;
  }
  if (v.isStr()) {
    r.isString = true;
    r.text = v.strVal();
    return r;
  }
  r.isNan = true;
  return r;
}

}  // namespace

// `postfile H type1 name1 type2 name2 ... using PATH [, replace every(#)]`
ojson runPostfile(const std::string& rawLine, SessionState& session) {
  std::string rest = trim(rawLine);
  // strip leading "postfile" keyword
  {
    static const std::regex reKw(R"(^postfile\s+)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(rest, m, reKw)) {
      return engineErrorBlock("postfile: internal parse error (keyword missing)");
    }
    rest = trim(rest.substr(static_cast<size_t>(m.length(0))));
  }

  size_t sp = rest.find_first_of(" \t");
  std::string handle = (sp == std::string::npos) ? rest : rest.substr(0, sp);
  if (handle.empty()) return engineErrorBlock("postfile: handle name required");
  rest = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp));

  // Find ` using ` (last occurrence, case-insensitive).
  std::string restLower = lower(rest);
  size_t usingIdx = restLower.rfind(" using ");
  std::string typeNamePart, pathPart;
  if (usingIdx == std::string::npos) {
    if (restLower.rfind("using ", 0) == 0) {
      typeNamePart = "";
      pathPart = trim(rest.substr(6));
    } else {
      return engineErrorBlock("postfile: missing `using` clause in: " + rawLine);
    }
  } else {
    typeNamePart = trim(rest.substr(0, usingIdx));
    pathPart = trim(rest.substr(usingIdx + std::string(" using ").size()));
  }

  // Strip a trailing options clause (`, replace` / `, every(#)`) — comma
  // OUTSIDE quotes. `replace` is read: without it an existing file is r(602).
  bool replaceOpt = false;
  {
    bool inQ = false;
    for (size_t i = 0; i < pathPart.size(); ++i) {
      char ch = pathPart[i];
      if (ch == '"') {
        inQ = !inQ;
      } else if (ch == ',' && !inQ) {
        std::istringstream os(lower(pathPart.substr(i + 1)));
        for (std::string w; os >> w;) {
          if (w == "replace") replaceOpt = true;
        }
        pathPart = trim(pathPart.substr(0, i));
        break;
      }
    }
  }
  if (pathPart.size() >= 2 && ((pathPart.front() == '"' && pathPart.back() == '"') ||
                               (pathPart.front() == '\'' && pathPart.back() == '\''))) {
    pathPart = pathPart.substr(1, pathPart.size() - 2);
  }
  if (pathPart.empty()) return engineErrorBlock("postfile: empty filename after `using`");
  // [P] postfile: "If filename is specified without an extension, .dta is
  // assumed", and an existing file needs `replace` -- r(602) `file X already
  // exists` (w2d_uncovered.log postfile_exists_noreplace `602|1|1|`: the old
  // file is untouched).
  {
    const size_t slash = pathPart.find_last_of('/');
    const size_t dot = pathPart.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) pathPart += ".dta";
  }
  if (!replaceOpt && cecon::native_exists(resolveUserPath(session, pathPart))) {
    return ojson(eval::errorBlock("engine_error", "file " + pathPart + " already exists",
                                  std::nullopt, std::nullopt, std::nullopt, "r(602)"));
  }

  std::vector<std::string> names, types;
  {
    std::istringstream iss(typeNamePart);
    std::string tok;
    std::optional<std::string> pendingType;
    while (iss >> tok) {
      if (isTypeToken(tok)) {
        pendingType = lower(tok);
      } else {
        names.push_back(tok);
        types.push_back(pendingType.value_or("double"));
        pendingType.reset();
      }
    }
  }
  if (names.empty()) {
    return engineErrorBlock("postfile: no variable names found in: '" + typeNamePart + "'");
  }

  if (session.postfiles.count(handle)) {
    return engineErrorBlock("postfile: handle `" + handle + "` is already open");
  }

  PostHandle ph;
  ph.names = names;
  ph.types = types;
  ph.target = pathPart;
  session.postfiles[handle] = std::move(ph);
  return ojson::array();  // Stata silent on success
}

// `post H (e1)(e2)...`
ojson runPost(const std::string& rawLine, SessionState& session) {
  std::string rest = trim(rawLine);
  {
    static const std::regex reKw(R"(^post\s+)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(rest, m, reKw)) {
      return engineErrorBlock("post: internal parse error (keyword missing)");
    }
    rest = trim(rest.substr(static_cast<size_t>(m.length(0))));
  }
  size_t sp = rest.find_first_of(" \t");
  std::string handle = (sp == std::string::npos) ? rest : rest.substr(0, sp);
  if (handle.empty()) return engineErrorBlock("post: handle name required");
  std::string exprPart = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp));

  auto it = session.postfiles.find(handle);
  if (it == session.postfiles.end()) {
    // BUG-1079: Stata answers `post h not found`, r(111) (w2ax_t1_official.log
    // postutil_post_after). The old text said "not found" with no rc, and the
    // message-to-rc mapping read that as a missing FILE, r(601).
    return ojson(eval::errorBlock("engine_error", "post " + handle + " not found", std::nullopt,
                                  std::nullopt, std::nullopt, "r(111)"));
  }
  PostHandle& ph = it->second;

  std::vector<std::string> groups = extractParenGroups(exprPart);
  if (groups.size() != ph.nCols()) {
    return engineErrorBlock("post: expected " + std::to_string(ph.nCols()) +
                            " expression group(s) for handle `" + handle + "`, got " +
                            std::to_string(groups.size()) + ": '" + exprPart + "'");
  }

  std::vector<std::pair<std::string, std::string>> row;
  for (size_t idx = 0; idx < groups.size(); ++idx) {
    PostExprResult res = evalPostExpr(groups[idx], session);
    bool isStrCol = ph.isStringCol(idx);
    if (isStrCol) {
      if (res.isString) {
        row.emplace_back("string", res.text);
      } else if (res.isNan) {
        row.emplace_back("string", "");  // Stata missing string
      } else {
        row.emplace_back("string", res.text);  // number -> str(value)
      }
    } else {
      if (!res.isString) {
        row.emplace_back("numeric", res.isNan ? "nan" : res.text);
      } else {
        row.emplace_back("numeric", "nan");  // mismatch -> missing
      }
    }
  }
  ph.rows.push_back(std::move(row));
  return ojson::array();  // Stata silent on success
}

// `postclose H` — finalize: build a CSV of accumulated rows, `cobra
// convert` it (string columns forced to factor via --factor), remove the
// handle.
ojson runPostclose(const std::string& handle, SessionState& session) {
  auto it = session.postfiles.find(handle);
  if (it == session.postfiles.end()) {
    return engineErrorBlock("postclose: handle `" + handle + "` is not open");
  }
  PostHandle ph = std::move(it->second);
  session.postfiles.erase(it);

  std::string csvPath = session.workdir() + "/postclose_" +
                        std::to_string(static_cast<long long>(std::time(nullptr)) * 1000) + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  {
    std::ofstream csv(cecon::to_native_path(csvPath));
    for (size_t i = 0; i < ph.names.size(); ++i) {
      if (i) csv << ",";
      csv << ph.names[i];
    }
    csv << "\n";
    for (const auto& row : ph.rows) {
      for (size_t i = 0; i < row.size(); ++i) {
        if (i) csv << ",";
        const auto& [kind, text] = row[i];
        if (kind == "numeric" && text == "nan") {
          // blank field -> missing for f64
        } else if (kind == "string") {
          // quote to protect embedded commas/quotes (CSV-escape doubled
          // quotes, matching RFC4180 — cobra's reader is expected to
          // dequote symmetrically, same convention `cobra convert` uses
          // elsewhere in this codebase for string fields).
          std::string esc;
          esc.reserve(text.size() + 2);
          esc += '"';
          for (char c : text) {
            if (c == '"') esc += '"';
            esc += c;
          }
          esc += '"';
          csv << esc;
        } else {
          csv << text;
        }
      }
      csv << "\n";
    }
    // BUG-124 sibling: surface a silently-failed CSV write (ENOSPC swallowed
    // into badbit) before convert can build a row-truncated dataset from it.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(csvPath);
      return engineErrorBlock("postclose: failed writing scratch CSV (disk full or I/O error)");
    }
  }

  std::vector<std::string> factorCols;
  for (size_t i = 0; i < ph.names.size(); ++i) {
    if (ph.isStringCol(i)) factorCols.push_back(ph.names[i]);
  }

  // resolve target path against session.cwd, NOT session.workdir() (native-
  // paths stabilization item 2 — mirrors the oracle's `using` path already
  // being pre-expanded by step-0b macro expansion: tempfile resolution
  // happens before postfile ever sees the path).
  std::string target = resolveUserPath(session, ph.target);
  // A `.dta` target (runPostfile adds the extension Stata assumes) gets genuine
  // Stata bytes: the posted rows are converted to a scratch hbin and written
  // out by the engine's .dta writer, the one `save X.dta` uses, so a later
  // `use` of the name reads what Stata would (w2d_uncovered.log
  // postfile_new_ctl / postfile_replace_098).
  std::string lowerTarget = target;
  for (auto& ch : lowerTarget) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const bool toDta =
      lowerTarget.size() > 4 && lowerTarget.compare(lowerTarget.size() - 4, 4, ".dta") == 0;
  const std::string finalTarget = target;
  if (toDta) {
    target = csvPath + ".hbin";
    registerScratchMint(target);
  }

  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", target, "--delim", ","};
  if (!factorCols.empty()) {
    std::string joined;
    for (size_t i = 0; i < factorCols.size(); ++i) {
      if (i) joined += ",";
      joined += factorCols[i];
    }
    args.push_back("--factor");
    args.push_back(joined);
  }
  // BUG-124: check the result (runCobra never throws — the old catch was
  // dead). On failure remove the torn partial at the USER's target path: the
  // convert already truncated any pre-existing file the moment it opened the
  // ofstream, so removal loses nothing and prevents a corrupt file that
  // passes existence checks from surviving as an apparent success.
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    cecon::native_unlink(target);
    return engineErrorBlock("postclose: error writing hbin to '" + target +
                            "': " + errorFromRaw(cr).what());
  }
  if (!cecon::native_exists(target)) {
    return engineErrorBlock("postclose: error writing hbin to '" + target + "'");
  }
  if (toDta) {
    CobraRunResult dr = runCobra({"export-dta", "--data", target, "--out", finalTarget});
    cecon::native_unlink(target);
    if (!dr.ok) {
      cecon::native_unlink(finalTarget);
      return engineErrorBlock("postclose: error writing '" + finalTarget +
                              "': " + errorFromRaw(dr).what());
    }
    cecon::native_unlink(csvPath);
    return ojson::array();  // Stata silent on success
  }
  // RED-fix (piet_2.1.0_gate Leg 3): record this session's own hbin write so
  // a later `use` of the SAME resolved path (the routine tempfile/postfile
  // round-trip idiom) skips runUse's BUG-A hbin-sniff honesty note — see
  // session_state.h's postfileWrittenPaths comment for the full rationale.
  session.recordPostfileWrite(target);
  return ojson::array();  // Stata silent on success
}

}  // namespace piet::handlers
