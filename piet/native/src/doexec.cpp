// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/doexec.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <variant>

#include "piet/cclass.h"         // BUG-1330: kVersionLocal
#include "piet/counters.h"       // B2 NAMED-OWED ③: live_named_bytes_peak high-water accumulator
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/mata_subset.h"
#include <atomic>

#include "piet/parallel.h"
#include "piet/parallel_auto.h"
#include "piet/program.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

namespace piet::doexec {

namespace {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// BUG-139: dispatcher.cpp's `exit` arm halts a do-file by pushing an
// UNTAGGED error block (the same channel a real failure uses — this IS the
// halt mechanism, see the two call sites below), whose message is exactly
// "exit" or "exit <n>" (dispatcher.cpp:2419) — no other error message in
// this codebase has that exact shape (every other one is a descriptive
// sentence). The block's `code` can't be used to tell a clean `exit` apart
// from a real engine_error: CONTRACT.md's CommandErrorResult.code enum is
// frozen and doesn't include a dedicated exit code, so errorBlock() coerces
// dispatcher.cpp's intended "program_exit" down to "engine_error" at
// construction time. Recognize the message shape instead, at the same site
// that already reads the halting block's message into stoppedAtReason, so
// runDoFileHeadless (main.cpp) can return the REQUESTED code instead of
// collapsing every halt to rc 1.
std::optional<int> parseCleanExitMessage(const std::string& message) {
  static const std::regex reCleanExit(R"(^exit(\s+(-?\d+))?$)");
  std::smatch m;
  if (!std::regex_match(message, m, reCleanExit)) return std::nullopt;
  if (!m[2].matched) return 0;
  try {
    return std::stoi(m.str(2));
  } catch (...) {
    return std::nullopt;
  }
}

bool handleUncapturedError(const ojson& block, int lineno, ExecuteResult& out) {
  if (block.value("type", "") != "error" || block.value("captured", false)) return false;
  out.stoppedAtLine = lineno;
  std::string haltMsg = block.value("message", std::string("error"));
  out.stoppedAtExitCode = parseCleanExitMessage(haltMsg);
  if (out.stoppedAtExitCode.has_value()) {
    // `exit` uses an error block only as the executor's halt sentinel. It is
    // not user-visible output, and leaving it in the stream paints a clean
    // trailing exit as a red engine-error card.
    if (!out.blocks.empty() && out.blocks.back().value("type", "") == "error" &&
        out.blocks.back().value("message", std::string()) == haltMsg) {
      out.blocks.erase(out.blocks.size() - 1);
    }
  } else {
    out.stoppedAtReason = haltMsg;
  }
  return true;
}

// BUG-106: RAII depth guard around executeDo's own body-execution call.
// executeDo recurses (a nested `do FILE`/`run FILE` inside a running script
// calls back into doexec::executeDo — see dispatcher.cpp's `do` arm), and
// session.runTempdir is shared across the WHOLE top-level run, so its reap
// (below, in executeDo) must only fire once the outermost call unwinds.
// Plain counter, restored on the destructor path so a throw out of
// executeDoSource still leaves depth correct (mirrors program.cpp's
// ScopeGuard/progCallDepth pattern).
class DoExecDepthGuard {
 public:
  explicit DoExecDepthGuard(SessionState& session) : session_(session) { ++session_.doExecDepth; }
  ~DoExecDepthGuard() { --session_.doExecDepth; }
  DoExecDepthGuard(const DoExecDepthGuard&) = delete;
  DoExecDepthGuard& operator=(const DoExecDepthGuard&) = delete;

 private:
  SessionState& session_;
};

// BUG-112(7): RAII wrapper for the per-run tempdir reap (S5/U4). The reap
// used to sit as plain code AFTER executeDo's body-execution block — so any
// exception propagating out of executeDoSource (dispatchFn is called
// unguarded at this file's single call site) skipped it entirely, leaking
// session.runTempdir's directory on disk and leaving the optional set for
// the NEXT top-level run to trip over. Mirrors DoExecDepthGuard immediately
// above; constructed BEFORE it in executeDo so it destructs AFTER (LIFO) —
// the reap always observes doExecDepth already decremented back for this
// call, on both the normal-return and throwing paths, exactly like the
// pre-fix ordering (block-scope reap after depthGuard's destructor) did on
// the happy path only.
class TempdirReapGuard {
 public:
  explicit TempdirReapGuard(SessionState& session) : session_(session) {}
  ~TempdirReapGuard() {
    if (session_.doExecDepth == 0 && session_.runTempdir.has_value()) {
      // BUG-384: same eraser at the run boundary — every minted tempfile that
      // survived to the end of the top-level run dies here, so its virtual-save
      // registration (worker RAM + a retained root mmap) dies with it.
      for (const auto& [depth, path] : session_.mintedTempfiles) {
        (void)depth;
        session_.residentVirtualDrop(path);
      }
      session_.mintedTempfiles.clear();
      std::error_code rmEc;
      std::filesystem::remove_all(cecon::to_native_path(*session_.runTempdir), rmEc);
      session_.runTempdir.reset();
    }
  }
  TempdirReapGuard(const TempdirReapGuard&) = delete;
  TempdirReapGuard& operator=(const TempdirReapGuard&) = delete;

 private:
  SessionState& session_;
};

// ---------------------------------------------------------------------------
// _BraceBlock — a loop/if header with its brace-aggregated body (+ optional
// else body). Nested blocks are represented as an Item variant recursively.

struct BraceBlock;

using Item = std::variant<std::string, std::shared_ptr<BraceBlock>>;

struct BraceBlock {
  std::string header;
  std::vector<Item> body;
  std::vector<Item> elseBody;
};

bool isBraceBlock(const Item& item) { return std::holds_alternative<std::shared_ptr<BraceBlock>>(item); }

const std::shared_ptr<BraceBlock>& asBraceBlock(const Item& item) {
  return std::get<std::shared_ptr<BraceBlock>>(item);
}

int countNetBraces(const std::string& line) {
  int opens = 0, closes = 0;
  for (char c : line) {
    if (c == '{') ++opens;
    if (c == '}') ++closes;
  }
  return opens - closes;
}

// ---------------------------------------------------------------------------
// Pre-split single-line loop forms:
//   foreach v in mpg price { summarize `v' }
// -> "foreach v in mpg price {" / "summarize `v'" / "}"

std::vector<std::string> splitIntoBraceTokens(const std::string& source) {
  std::vector<std::string> lines;
  std::istringstream iss(source);
  std::string rawLine;
  while (std::getline(iss, rawLine)) {
    std::string stripped = trim(rawLine);
    if (loops::opensBraceBlock(stripped)) {
      size_t firstBrace = stripped.find('{');
      std::string headerPart = trim(stripped.substr(0, firstBrace));
      std::string rest = stripped.substr(firstBrace + 1);

      int depth = 1;
      long bodyEnd = -1;
      for (size_t k = 0; k < rest.size(); ++k) {
        if (rest[k] == '{') {
          ++depth;
        } else if (rest[k] == '}') {
          --depth;
          if (depth == 0) {
            bodyEnd = static_cast<long>(k);
            break;
          }
        }
      }

      if (bodyEnd >= 0) {
        std::string bodyContent = rest.substr(0, static_cast<size_t>(bodyEnd));
        lines.push_back(headerPart + " {");
        std::istringstream bodyIss(bodyContent);
        std::string bl;
        while (std::getline(bodyIss, bl)) {
          std::string blTrim = trim(bl);
          if (!blTrim.empty()) lines.push_back(blTrim);
        }
        lines.push_back("}");

        std::string afterClose = trim(rest.substr(static_cast<size_t>(bodyEnd) + 1));
        std::string afterCloseLower = lower(afterClose);
        if (afterCloseLower.rfind("else", 0) == 0) {
          std::string elseRest = trim(afterClose.substr(4));
          if (!elseRest.empty() && elseRest[0] == '{') {
            lines.push_back("else {");
            std::string elseBodyPart = elseRest.substr(1);
            int depth2 = 1;
            long body2End = -1;
            for (size_t k2 = 0; k2 < elseBodyPart.size(); ++k2) {
              if (elseBodyPart[k2] == '{') {
                ++depth2;
              } else if (elseBodyPart[k2] == '}') {
                --depth2;
                if (depth2 == 0) {
                  body2End = static_cast<long>(k2);
                  break;
                }
              }
            }
            if (body2End >= 0) {
              std::string ebContent = elseBodyPart.substr(0, static_cast<size_t>(body2End));
              std::istringstream ebIss(ebContent);
              std::string ebl;
              while (std::getline(ebIss, ebl)) {
                std::string eblTrim = trim(ebl);
                if (!eblTrim.empty()) lines.push_back(eblTrim);
              }
              lines.push_back("}");
            }
          }
        }
        continue;  // done with this raw_line
      }
    }
    lines.push_back(stripped);
  }
  return lines;
}

// ---------------------------------------------------------------------------
// Brace aggregation (doexec.py's _aggregate_braces / _collect_body)

std::vector<Item> aggregateBraces(const std::vector<std::string>& lines);

// Returns (bodyItems, elseBodyItems, nextIndex).
struct CollectResult {
  std::vector<Item> body;
  std::vector<Item> elseBody;
  size_t next = 0;
};

CollectResult collectBody(const std::vector<std::string>& lines, size_t start, size_t n);

// FU-602: Stata's braceless branch pair. `if exp command` on one line may be
// followed by `else command` (or `else {` / `else if ...`) on the next one, and
// the `else` runs when that `if` was false (oracle w2_esample_copies.do:39-40,
// program pcnt: `if _rc c_local `name' = .` / `else c_local `name' = r(N)`;
// StataBE writes all 22 PREC records, w2_esample_copies.log). The pair is
// folded here into the same BraceBlock a braced `if {} else {}` builds, so the
// condition and both bodies take runIf's semantics through executeLoop; a
// one-line `if` that no `else` follows stays a plain line and keeps the
// dispatcher's braceless intercept. The split is splitOneLineIf's, the same
// walk that intercept uses, so the two forms cannot cut a line differently.
bool startsWithElseWord(const std::string& s) {
  if (s.size() < 4 || lower(s.substr(0, 4)) != "else") return false;
  return s.size() == 4 || s[4] == '{' || std::isspace(static_cast<unsigned char>(s[4])) != 0;
}

bool splitBracelessIf(const std::string& stripped, std::string& cond, std::string& body) {
  if (stripped.size() <= 3 || lower(stripped.substr(0, 3)) != "if " ||
      stripped.find('{') != std::string::npos)
    return false;
  return loops::splitOneLineIf(trim(stripped.substr(3)), cond, body) && !body.empty();
}

// The else part that may follow an if body ending before lines[start]. Returns
// false (nothing consumed) when the next non-empty line is not an else form.
bool collectElse(const std::vector<std::string>& lines, size_t start, size_t n,
                 std::vector<Item>& elseBody, size_t& next) {
  size_t j = start;
  while (j < n && trim(lines[j]).empty()) ++j;
  if (j >= n) return false;
  std::string nextLine = trim(lines[j]);
  std::string nlLower = lower(nextLine);

  static const std::regex reElseIf(R"(^else\s+if\b(.*))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(nextLine, m, reElseIf)) {
    const std::string tail = trim(m.str(1));
    // FU-602: `else if exp command` on one line, with no brace on it or on the
    // next line, is a braceless if of its own and may carry its own else.
    if (tail.find('{') == std::string::npos) {
      size_t k = j + 1;
      while (k < n && trim(lines[k]).empty()) ++k;
      std::string c, b;
      if (!(k < n && trim(lines[k]) == "{") && loops::splitOneLineIf(tail, c, b) && !b.empty()) {
        auto bb = std::make_shared<BraceBlock>();
        bb->header = "if " + c + " {";
        bb->body = {Item(b)};
        size_t after = j + 1;
        if (!collectElse(lines, j + 1, n, bb->elseBody, after)) after = j + 1;
        elseBody = {Item(bb)};
        next = after;
        return true;
      }
    }
    ++j;
    std::string innerHeader = trim("if " + tail);
    if (innerHeader.empty() || innerHeader.back() != '{') {
      while (j < n && trim(lines[j]).empty()) ++j;
      if (j < n && trim(lines[j]) == "{") ++j;
      innerHeader += " {";
    }
    CollectResult inner = collectBody(lines, j, n);
    auto bb = std::make_shared<BraceBlock>();
    bb->header = innerHeader;
    bb->body = inner.body;
    bb->elseBody = inner.elseBody;
    elseBody = {Item(bb)};
    next = inner.next;
    return true;
  }
  if (nlLower == "else {" || nlLower == "else{" || nlLower == "else") {
    ++j;
    if (nlLower != "else {" && nlLower != "else{") {
      while (j < n && trim(lines[j]).empty()) ++j;
      if (j < n && trim(lines[j]) == "{") ++j;
    }
    CollectResult rawElse = collectBody(lines, j, n);
    elseBody = rawElse.body;
    next = rawElse.next;
    return true;
  }
  // FU-602: `else command` — one command, no brace block on the line.
  if (startsWithElseWord(nextLine) && nextLine.size() > 4 &&
      std::isspace(static_cast<unsigned char>(nextLine[4])) != 0) {
    const std::string cmd = trim(nextLine.substr(5));
    if (!cmd.empty() && cmd[0] != '{' && countNetBraces(cmd) == 0) {
      elseBody = {Item(cmd)};
      next = j + 1;
      return true;
    }
  }
  return false;
}

CollectResult collectBody(const std::vector<std::string>& lines, size_t start, size_t n) {
  std::vector<std::string> rawBody;
  int depth = 1;
  size_t i = start;

  while (i < n && depth > 0) {
    std::string stripped = trim(lines[i]);
    ++i;
    if (stripped.empty()) {
      rawBody.emplace_back("");
      continue;
    }
    int net = countNetBraces(stripped);
    if (net > 0) {
      rawBody.push_back(stripped);
      depth += net;
    } else if (net < 0) {
      depth += net;
      if (depth > 0) rawBody.push_back(stripped);
      // else: depth hit 0 — this '}' closes OUR block, don't include it.
    } else {
      rawBody.push_back(stripped);
    }
  }

  CollectResult result;
  result.body = aggregateBraces(rawBody);
  if (collectElse(lines, i, n, result.elseBody, result.next)) return result;
  result.next = i;
  return result;
}

std::vector<Item> aggregateBraces(const std::vector<std::string>& lines) {
  std::vector<Item> result;
  size_t i = 0;
  size_t n = lines.size();

  while (i < n) {
    std::string raw = lines[i];
    std::string stripped = trim(raw);
    ++i;

    if (stripped.empty()) continue;
    if (stripped == "}") continue;  // lone closer — malformed guard, drop.

    // DO-BRACE (W2AK): a trailing `{` opens a block only when the head is a
    // control word or EVERY token before the brace is a prefix keyword
    // (loops::opensBraceBlock's rule). `capture noisily tex \resizebox{..}{!}{`
    // is an ordinary command whose TEXT ends in a brace; reading it as a block
    // header silently swallowed it and the `capture noisily tex }` line after it
    // (w2at wt_x04).
    bool opensWithBrace = !stripped.empty() && stripped.back() == '{' &&
                          loops::isLoopHeader(trim(stripped.substr(0, stripped.size() - 1))) &&
                          (loops::isParallelHeader(stripped) || loops::opensBraceBlock(stripped));

    if (!opensWithBrace && loops::opensBraceBlock(stripped)) {
      size_t firstBrace = stripped.find('{');
      std::string headerCandidate = trim(stripped.substr(0, firstBrace));
      if (loops::isLoopHeader(headerCandidate)) {
        std::string rest = stripped.substr(firstBrace + 1);
        std::vector<std::string> bodyInline;
        int depth = 1;
        long bodyEnd = -1;
        for (size_t k = 0; k < rest.size(); ++k) {
          if (rest[k] == '{') {
            ++depth;
          } else if (rest[k] == '}') {
            --depth;
            if (depth == 0) {
              bodyEnd = static_cast<long>(k);
              break;
            }
          }
        }
        if (bodyEnd >= 0) {
          std::string bodyContent = trim(rest.substr(0, static_cast<size_t>(bodyEnd)));
          if (!bodyContent.empty()) bodyInline = {bodyContent};
          std::string after = trim(rest.substr(static_cast<size_t>(bodyEnd) + 1));
          std::vector<Item> bodyAgg = aggregateBraces(bodyInline);
          std::vector<Item> elseAgg;
          if (lower(after).rfind("else", 0) == 0) {
            std::string elseContent = trim(after.substr(4));
            if (!elseContent.empty() && elseContent.front() == '{') elseContent.erase(0, 1);
            if (!elseContent.empty() && elseContent.back() == '}') elseContent.pop_back();
            elseContent = trim(elseContent);
            elseAgg = aggregateBraces(elseContent.empty() ? std::vector<std::string>{}
                                                            : std::vector<std::string>{elseContent});
          }
          auto bb = std::make_shared<BraceBlock>();
          bb->header = headerCandidate + " {";
          bb->body = bodyAgg;
          bb->elseBody = elseAgg;
          result.emplace_back(bb);
          continue;
        }
      }
    }

    bool nextIsBrace = false;
    if (!opensWithBrace && loops::isLoopHeader(stripped)) {
      size_t j = i;
      while (j < n && trim(lines[j]).empty()) ++j;
      if (j < n && trim(lines[j]) == "{") {
        nextIsBrace = true;
        i = j + 1;
      }
    }

    if (opensWithBrace || nextIsBrace) {
      std::string header = stripped;
      CollectResult cr = collectBody(lines, i, n);
      i = cr.next;
      auto bb = std::make_shared<BraceBlock>();
      bb->header = header;
      bb->body = cr.body;
      bb->elseBody = cr.elseBody;
      result.emplace_back(bb);
    } else {
      // FU-602: a braceless `if exp command` followed by an else form.
      std::string c, b;
      std::vector<Item> eb;
      size_t after = i;
      if (splitBracelessIf(stripped, c, b) && collectElse(lines, i, n, eb, after)) {
        auto bb = std::make_shared<BraceBlock>();
        bb->header = "if " + c + " {";
        bb->body = {Item(b)};
        bb->elseBody = std::move(eb);
        result.emplace_back(bb);
        i = after;
        continue;
      }
      result.emplace_back(stripped);
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Flatten a brace-aggregated body back to plain source lines (doexec.py's
// _flatten_body) for loops::executeLoop, which re-parses nested blocks via
// executeDoSource.

std::vector<std::string> flattenBody(const std::vector<Item>& body) {
  std::vector<std::string> flat;
  for (const auto& item : body) {
    if (isBraceBlock(item)) {
      const auto& bb = asBraceBlock(item);
      std::string hdr = bb->header;
      if (hdr.empty() || hdr.back() != '{') hdr += " {";
      flat.push_back(hdr);
      auto innerFlat = flattenBody(bb->body);
      flat.insert(flat.end(), innerFlat.begin(), innerFlat.end());
      flat.push_back("}");
      if (!bb->elseBody.empty()) {
        flat.push_back("else {");
        auto elseFlat = flattenBody(bb->elseBody);
        flat.insert(flat.end(), elseFlat.begin(), elseFlat.end());
        flat.push_back("}");
      }
    } else {
      const std::string& s = std::get<std::string>(item);
      if (!s.empty()) flat.push_back(s);
    }
  }
  return flat;
}

ojson cancelledBlock() {
  ojson b;
  b["type"] = "error";
  b["code"] = "engine_error";
  b["message"] = "run cancelled";
  b["reason"] = "cancelled";
  return b;
}

// ---------------------------------------------------------------------------
// P7 + F6/F7 (B.3.0 "bounded transcripts", MAXPERF Stage 0 Ship 2): a runaway
// `.do` run must not be able to assemble an unbounded STORED transcript (the
// incident: 2,742 mutations × a 2.3 MB varlist card = a multi-GB bridge
// payload + RSS blowup). Two independent ceilings bound what gets STORED in
// out.blocks — a block COUNT cap (kMaxRunBlocks) and a stored-BYTE budget
// (maxRunBytes). Generous / classroom-day-safe: a normal run — even a long,
// thorough problem-set .do file — emits at most a few thousand small blocks;
// these only bite a genuine runaway. Execution itself is NEVER cut short (a
// loop that mutates the dataset still runs to completion so its side effects
// are correct) and — the F6a change — live STREAMING is never cut short
// either: once a ceiling trips we push ONE elision marker (stored AND
// streamed), then keep calling onBlock for every later block WITHOUT storing
// it, so the live console keeps painting while the retained transcript stays
// bounded. (console.js keeps its own separate DOM-side session-history cap.)
constexpr size_t kMaxRunBlocks = 20000;

// F6b stored-byte budget: default 64 MB; env PIET_MAX_RUN_BYTES overrides it
// (tests set it tiny to force the elision path deterministically; "0" trips on
// the very first block). Parsed fresh each run (trivial); a bad/empty value
// keeps the default.
size_t maxRunBytes() {
  size_t budget = 64ull * 1024 * 1024;
  if (const char* env = std::getenv("PIET_MAX_RUN_BYTES"); env && *env) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env) budget = static_cast<size_t>(v);
  }
  return budget;
}

// The elision marker, pushed (stored AND streamed) the moment a ceiling trips.
// A plain display block so it renders in the console and tees to the log
// verbatim; the additive `elided:true` field is a stable discriminator for the
// frontend/tests (a display block never otherwise carries it).
ojson elisionBlock(const std::string& reason) {
  ojson b;
  b["type"] = "display";
  b["text"] = "(stored transcript truncated: " + reason +
              " -- the run continues to completion and its results are unaffected; "
              "further blocks are not retained in the saved transcript)";
  b["elided"] = true;
  return b;
}

// F8 do-run coalescing: among the blocks ONE top-level command produced, only
// the LAST data_mutation needs to carry `varlist_after`. A program/loop body
// that runs N mutations otherwise stores N full varlist cards, but the
// intermediate ones are never seen (Stata shows nothing per-mutation inside a
// body; the frontend only needs the final column state per command). Strip the
// varlist ARRAY — field ABSENT, not null (the frontend's withMutationSummaries
// guards on presence) — from every data_mutation EXCEPT the last that has it;
// the scalars (action / n_rows_after / new_vars / dropped_vars) stay on all.
}  // namespace  (exported below: doexec::coalesceMutationVarlists)

// Exported for loops::runParallelBlock — see doexec.h.
void coalesceMutationVarlists(ojson& blocks) {
  if (!blocks.is_array()) return;
  auto isMutWithVarlist = [](const ojson& b) {
    return b.is_object() && b.value("type", std::string()) == "data_mutation" &&
           b.contains("varlist_after");
  };
  long lastIdx = -1;
  for (long i = 0; i < static_cast<long>(blocks.size()); ++i) {
    if (isMutWithVarlist(blocks[static_cast<size_t>(i)])) lastIdx = i;
  }
  if (lastIdx < 0) return;  // no varlist-bearing mutation — nothing to thin.
  for (long i = 0; i < lastIdx; ++i) {
    ojson& b = blocks[static_cast<size_t>(i)];
    if (isMutWithVarlist(b)) b.erase("varlist_after");
  }
}

ExecuteResult executeDoSource(const std::string& source, SessionState& session,
                               const DispatchFn& dispatchFn, const std::vector<std::string>& args,
                               bool progressOwner, std::optional<std::string> doPath,
                               BlockSink onBlock) {
  // ATTR-20260812: doexec nesting depth. Depth 1 is the do-source the run was
  // asked for; loop bodies (loops.cpp runBody), program bodies and replayed
  // `do` files re-enter here and are depth 2+. Everything the depth-split
  // instrument does hangs off this one scope, and it is maintained
  // unconditionally so the value is right even for a run that never emits.
  struct DepthScope {
    DepthScope() { ++piet::counters::g_doexecDepth; }
    ~DepthScope() { --piet::counters::g_doexecDepth; }
  } __depth;

  if (progressOwner) {
    // Provisional running=true (total_lines finalized after aggregation).
    session.progressStart(0, doPath);
  }

  // S3/U3-args: seed positional args into _locals BEFORE running any line.
  if (!args.empty()) {
    std::string joined;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) joined += " ";
      joined += args[i];
    }
    session.eval.locals.set("0", joined);
    for (size_t i = 0; i < args.size(); ++i) {
      session.eval.locals.set(std::to_string(i + 1), args[i]);
    }
  }

  // ---- A5: the pre-parse cache (P2PERF-WAVE-PLAN ARC-A rung 5) -------------
  //
  // executeDoSource runs once per LOOP ITERATION (runBody joins the body and
  // calls it again), so everything between here and `aggregated` — line
  // splitting, comment stripping, /// joining, the program scan, the brace
  // pre-split and the brace aggregation — was recomputed 30,000 times for a
  // body whose text never changes. A symbolized profile put ~54% of a pure
  // macro loop's samples in this stretch, outside dispatch entirely; it is the
  // single largest remaining term and the same one Tcl 8.0 removed by attaching
  // a compiled representation to the source string (2-20x there; parsing was
  // ~50% of pre-8.0 time). This is that, at line granularity.
  //
  // SAFE BY CONSTRUCTION, three ways:
  //  1. Keyed on the source TEXT, not on the address of the caller's vector. A
  //     pointer key is fast and wrong: a later body can land on a freed
  //     address and silently execute the previous body's items.
  //  2. Only the PURE prefix is cached. Everything after `aggregated` — args
  //     seeding, the byte budget, progress-cancel, ItemTimer, stopped_at/halt
  //     handling — still runs per call. This function was deliberately not
  //     split into "preprocess + a second item loop somewhere else", because a
  //     second loop is where the two copies drift.
  //  3. `program define` REGISTERS into the session (captureProgams' one side
  //     effect), so a body carrying one is never cached — the entry is not
  //     stored at all and every iteration re-runs the real thing.
  struct PreparsedEntry {
    std::string source;
    std::vector<Item> aggregated;
  };
  static thread_local std::vector<PreparsedEntry> preparseCache;
  constexpr size_t kPreparseCacheMax = 8;  // a nest of loops, not a corpus

  std::vector<Item> aggregated;
  bool cacheHit = false;
  for (size_t i = 0; i < preparseCache.size(); ++i) {
    if (preparseCache[i].source == source) {
      aggregated = preparseCache[i].aggregated;
      if (i != 0) std::swap(preparseCache[i], preparseCache[0]);  // MRU to front
      cacheHit = true;
      break;
    }
  }

  if (!cacheHit) {
    std::vector<std::string> preprocessed = eval::preprocessDo(source);
    // DO-TABEXPAND (W2AK), narrowly: a texdoc `tex` line writes its own text,
    // and Stata's file carries the line's TABs as blanks to 8-column stops
    // counted from the start of the do-file line (w2at wt_x01_L2, wt_x02_L1:
    // Python's expandtabs(8) over the whole line reproduces both byte for
    // byte). Applied to `tex` lines only, where it is measured.
    for (auto& line : preprocessed) {
      if (line.find('\t') == std::string::npos) continue;
      static const std::unordered_set<std::string> kPfx = {
          "cap", "capt", "captu", "captur", "capture", "n", "no", "noi", "nois",
          "noisi", "noisil", "noisily", "qui", "quie", "quiet", "quietl", "quietly"};
      std::istringstream iss(line);
      std::string w;
      bool isTex = false;
      while (iss >> w) {
        std::string lw = lower(w);
        if (!lw.empty() && lw.back() == ':') lw.pop_back();
        if (kPfx.count(lw) != 0) continue;
        isTex = (lw == "tex");
        break;
      }
      if (!isTex) continue;
      std::string out;
      for (char c : line) {
        if (c == '\t') {
          out.append(8 - (out.size() % 8), ' ');
        } else {
          out += c;
        }
      }
      line = out;
    }

    // U5 (S6): capture `program [define] NAME … end` blocks. Must run
    // POST-preprocess (so /// continuations are already joined and each
    // program block is intact) and BEFORE the loop pre-split/brace
    // aggregation (so the aggregator never sees program/end).
    // AER-SCOPE1 bundle F: fold `input ... end` into ONE line, the same shape
    // `program ... end` is folded just below. `input` is the only other Stata
    // construct whose body is DATA rather than statements, so letting the
    // execution loop see its rows would dispatch each one as a command.
    //
    // ORDER IS LOAD-BEARING, and measured: this MUST run BEFORE captureProgams,
    // which drops a bare `end` outside a program block silently
    // (program.cpp:233). Running it after meant the fold never saw its
    // terminator and every top-level `input` reported "block not terminated by
    // `end`" -- nine selftest legs, one cause. captureInputBlocks passes a
    // `program ... end` region through untouched so the two folds cannot eat
    // each other's terminator.
    //
    // ml-getmata T4: `mata:` ... `end` is the third such block, and its body is
    // Mata, not Stata -- dispatched line by line, `result = st_matrix(...)` was
    // refused as a Stata command named `result` and stopped the do-file with a
    // message about a Mata variable (BUG-696). It folds FIRST, for the same
    // reason input folds before programs: captureProgams drops a bare `end`, and
    // an `input`-headed line inside a Mata block must not open a data block that
    // runs to the Mata block's own `end`. Both of the other folds' regions are
    // passed through untouched by it.
    preprocessed = mata::captureMataBlocks(preprocessed);
    preprocessed = handlers::captureInputBlocks(preprocessed);

    const size_t beforeProg = preprocessed.size();
    preprocessed = program::captureProgams(preprocessed, session);
    const bool definedProgram = preprocessed.size() != beforeProg;

    // Pre-split single-line loop forms.
    std::vector<std::string> splitLines;
    for (const auto& line : preprocessed) {
      if (!trim(line).empty() && loops::opensBraceBlock(trim(line))) {
        auto expanded = splitIntoBraceTokens(line);
        splitLines.insert(splitLines.end(), expanded.begin(), expanded.end());
      } else {
        splitLines.push_back(line);
      }
    }

    aggregated = aggregateBraces(splitLines);

    if (!definedProgram) {
      if (preparseCache.size() >= kPreparseCacheMax) preparseCache.pop_back();
      preparseCache.insert(preparseCache.begin(), PreparsedEntry{source, aggregated});
    }
  }

  if (progressOwner) {
    // BUG-342: the install sentinels are not statements — they neither run nor
    // consume a line number, so they must not inflate the progress total either
    // (a do-file with programs would otherwise report a denominator larger than
    // the number of steps the user can see happen).
    int steps = 0;
    for (const auto& it : aggregated) {
      std::string ignored;
      if (!isBraceBlock(it) &&
          program::isProgInstallSentinel(std::get<std::string>(it), &ignored))
        continue;
      ++steps;
    }
    session.progressStart(steps, doPath);
  }

  ExecuteResult out;
  int lineno = 0;

  // B2 NAMED-OWED ③ (PHASE-B-DECISION-MEMO §6-3 ADDENDUM, ratified 2026-07-23):
  // sample liveNamedBytes() after each item and keep the running max
  // (`live_named_bytes_peak`, the ratified peak-vs-peak ② denominator). Gated on
  // the counter seam so env-off runs never call it — --do stdout stays
  // byte-identical to every captured freeze fixture. NOT restricted to the
  // top-level call: the frame's high-water often occurs INSIDE a program or loop
  // body (e.g. a2_mini's `use soccer_game_panel` at run_one_depvar's line 187,
  // which is then collapsed to cell means) — a top-level-only hook misses it
  // entirely and reports only the post-`exit,clear` residue. executeDoSource is
  // the one path every program/loop body replays through, so sampling in every
  // invocation is what makes the peak faithful. The sample itself is counter-
  // and cache-neutral (SessionState::liveNamedBytesPeakSample()).
  //
  // ⚠ The claim that once stood here — "reads only frame metadata (lazy mmap,
  // not the payload), so the per-item cost stays low" — was FALSE as written
  // and was REFUTED by measurement (instruments/alloc_churn_scoping.md §1,
  // 2026-07-29): the walk went through Datastore::varlist(), whose
  // buildVarInfo() computes n_missing by scanning every column end to end,
  // faulting the whole mmap payload in. Per-item × per-column, that made
  // PIET_EMIT_COUNTERS=1 cost a2_mini +11.6% wall (+45% PIET wall, +98k page
  // faults, only +0.79s CPU — fault-wait, not compute), which silently taxed
  // every counters-on orbit figure the campaign has quoted.
  // FIX-PIET-COUNTERTAX-PEAKSAMPLE made the walk metadata-only for real
  // (session_state.h liveNamedBytes §1), and selftest pins it. If you add
  // anything to that walk, the property to preserve is "no payload read" —
  // not this comment.
  // A1: `static` — this is a per-executeDoSource call, and executeDoSource runs
  // once per loop ITERATION, so an unhoisted getenv here is a syscall-adjacent
  // lookup on the interpreter's hottest path. The environment cannot change
  // inside the process.
  static const bool sampleNamedPeak = [] {
    const char* e = std::getenv("PIET_EMIT_COUNTERS");
    return e && *e;
  }();

  // Bounded-transcript state (P7 + F6). `truncated` flips once a STORED ceiling
  // (block COUNT or BYTE budget) trips: from then on pushBlock stores nothing
  // more but KEEPS STREAMING (F6a). `storedBytes` accumulates the documented
  // size proxy (each stored block's JSON dump length). The caps bound out.blocks
  // (the retained transcript); the aggregated-item loop below keeps running
  // exactly as without them (same error/_control detection, same side effects).
  bool truncated = false;
  size_t storedBytes = 0;
  // serset-twoway (2026-10-21): the COUNT ceiling counts the blocks that carry
  // output, not the per-line command echoes. A program or loop body echoes every
  // dispatched line on every iteration (Stata's log echoes none of them), so a
  // do-file whose helper loops over graph sersets row by row stored 16,600
  // echoes against 3,400 output blocks and lost its last records to the cap
  // (w2aq_histogram_family hk_kd_aw_g onward). Echoes stay STORED and stay
  // under the byte budget below, which is what bounds a runaway.
  size_t storedCounted = 0;
  static const size_t byteBudget = maxRunBytes();  // A1: getenv, hoisted (see above)

  // N6 (P8.1-W2) + F6c: pushBlock is the ONE append+stream point for top-level
  // blocks. onBlock (supplied only by runDoInBackground) fires for every block
  // in ORDER — INCLUDING those streamed AFTER storage stops. out.streamedCount
  // tracks how many entries of out.blocks were ALSO streamed: streaming and
  // storing advance together over the stored PREFIX, and the only stored-but-
  // never-streamed block is the terminal cancel (pushed directly below, without
  // bumping the count), so out.streamedCount is exactly the tail-slice offset
  // main.cpp uses to return only the UN-streamed remainder (F6c). With
  // onBlock==nullptr (every non-streaming caller: --do CLI, loop/program bodies,
  // selftest) nothing streams, the count stays 0, and out.blocks is returned
  // whole and byte-identical. Storing is independent of onBlock, so out.blocks
  // is byte-identical with and without the hook (N6 freeze leg).
  auto pushBlock = [&](const ojson& b) {
    // ATTR-20260812 coverage fingerprint. Counted HERE, before the budget trip,
    // and only at the outermost depth: a loop body's blocks pass through its own
    // nested pushBlock and then again through this one as they bubble up, so
    // counting at every depth would multiply them by their nesting. Truncation
    // stops storage, not emission, and the number wanted is what the run
    // emitted.
    //
    // What this counts, precisely (measured 2026-08-12, after an earlier
    // comment here overstated it): every error block that reaches the run's
    // output. A plain `capture` suppresses the block, so its errors are not
    // counted; `capture noisily` shows the error, so it IS a block and IS
    // counted, carrying `captured: true`. That is the honest line — "an error
    // the run displayed" — not "an error that killed the run".
    if (piet::counters::g_doexecDepth <= 1 && b.is_object()) {
      piet::counters::noteEmittedBlockType(b.value("type", std::string()));
    }
    // name-binding-display-literals: `display ... _continue` suppresses the
    // newline at the end of its line, so the next display's text continues it
    // (w2av_name_binding bd_dy_cont `[ab]`, bd_dy_contc `[a b]`). The block
    // says so (handleDisplay's `continue`); the text is joined onto it here,
    // past any command echo between them. The stored transcript holds the
    // joined line; a streaming console was sent the first block already, and
    // joins the next one's text onto it itself (console.js appendBlock,
    // BUG-1473), so both show one line.
    if (!truncated && b.is_object() && b.value("type", std::string()) == "display") {
      for (size_t k = out.blocks.size(); k-- > 0;) {
        ojson& prev = out.blocks[k];
        if (!prev.is_object()) break;
        const std::string pt = prev.value("type", std::string());
        if (pt == "echo") continue;
        if (pt != "display" || !prev.value("continue", false)) break;
        prev["text"] = prev.value("text", std::string()) + b.value("text", std::string());
        if (b.value("continue", false)) {
          prev["continue"] = true;
        } else {
          prev.erase("continue");
        }
        if (onBlock) onBlock(b);  // streamed, not stored: not counted (see below)
        return;
      }
    }
    if (!truncated) {
      // Size proxy for the byte budget — the block's own JSON dump length.
      // Only paid for blocks we actually STORE (post-trip blocks skip it), so
      // the measurement work is itself bounded by the budget.
      // BUG-1372: byte-based string functions can leave a split UTF-8
      // sequence in a block's text (substr("h\xc3\xa9llo",3,2), reverse()),
      // and a throwing dump() failed the whole run; such a byte is counted
      // as the U+FFFD the transcript will carry. So is a byte that is not
      // UTF-8 at all (`type` of a Latin-1 file, BUG-1131).
      size_t est = b.is_null() ? 0 : b.dump(-1, ' ', false, ojson::error_handler_t::replace).size();
      const bool isEcho = b.is_object() && b.value("type", std::string()) == "echo";
      if ((!isEcho && storedCounted >= kMaxRunBlocks) || storedBytes + est > byteBudget) {
        std::string reason =
            (!isEcho && storedCounted >= kMaxRunBlocks)
                ? ("this run exceeded " + std::to_string(kMaxRunBlocks) + " stored blocks")
                : ("this run exceeded its " + std::to_string(byteBudget) +
                   "-byte stored-transcript budget");
        ojson marker = elisionBlock(reason);
        out.blocks.push_back(marker);
        if (onBlock) {
          onBlock(marker);
          ++out.streamedCount;
        }
        truncated = true;
        return;
      }
      out.blocks.push_back(b);
      storedBytes += est;
      if (!isEcho) ++storedCounted;
      if (onBlock) {
        onBlock(b);
        ++out.streamedCount;
      }
    } else {
      // Storage bounded — keep the LIVE console painting (F6a) without
      // retaining anything. NOT counted in out.streamedCount: streamed but NOT
      // stored, so it is not part of out.blocks / the tail.
      if (onBlock) onBlock(b);
    }
  };

  // P9 automode (default OFF). Hoisted once: with the switch absent this whole
  // layer costs one predictable branch per top-level item.
  const bool autoOn = parallel::autoModeEnabled();
  // A refused run must announce itself ONCE, not once per statement in it. This
  // marks how far the last refusal reached so the remaining items of the same
  // run are neither re-planned nor re-reported — which also keeps the scan
  // linear instead of quadratic in the run length.
  size_t autoRefusedUntil = 0;

  for (size_t itemIdx = 0; itemIdx < aggregated.size(); ++itemIdx) {
    const auto& item = aggregated[itemIdx];
    ++lineno;

    // BUG-342: a `program define` block's install sentinel. Installing here —
    // rather than at preprocess time, where captureProgams filled the archive —
    // is what makes a program defined at its own source position, so a `clear
    // all` earlier in the file no longer deletes a definition that has not been
    // reached yet. It emits nothing and gives the line number back, so every
    // do-file that does not clear the table keeps byte-identical output and
    // identical stopped_at_line values.
    if (!isBraceBlock(item)) {
      std::string progName;
      if (program::isProgInstallSentinel(std::get<std::string>(item), &progName)) {
        auto it = session.programArchive.find(progName);
        if (it != session.programArchive.end()) {
          session.programs[progName] = it->second;
          ++session.programsEpoch;  // the table changed shape; summaries must re-derive
        }
        --lineno;
        continue;
      }
    }

    // ---- P9 automode · L1 discovery + approved-region execution -------------
    //
    // Depth 1 only. The parallelizable shape this wave targets is a run of
    // top-level statements; going deeper would put the recognizer on the
    // interpreter's hottest path for no corpus benefit, and it complicates the
    // halt story for nothing.
    //
    // Everything below preserves the two invariants the serial path owns:
    // blocks reach `out` through pushBlock (so streaming, storage caps and the
    // fingerprint counters stay correct by construction), and each replayed
    // block goes through handleUncapturedError with its OWN task's line number,
    // so a failing task halts exactly where serial would.
    if (autoOn && piet::counters::g_doexecDepth <= 1 && !isBraceBlock(item) &&
        itemIdx >= autoRefusedUntil) {
      // A run is a maximal stretch of flat items. A `program define` install
      // sentinel ENDS it: the sentinel must be executed by the parent (it is how
      // the program becomes defined, BUG-342), and shipping it to a worker would
      // install the program in a process that then exits with it.
      auto isRunnableFlat = [&](size_t k) {
        if (isBraceBlock(aggregated[k])) return false;
        std::string ignored;
        return !program::isProgInstallSentinel(std::get<std::string>(aggregated[k]), &ignored);
      };
      size_t runEnd = itemIdx;
      while (runEnd < aggregated.size() && isRunnableFlat(runEnd)) ++runEnd;
      std::vector<std::string> stmts;
      for (size_t k = itemIdx; k < runEnd; ++k) stmts.push_back(std::get<std::string>(aggregated[k]));
      std::vector<std::string> following;
      for (size_t k = runEnd; k < aggregated.size() && isRunnableFlat(k); ++k)
        following.push_back(std::get<std::string>(aggregated[k]));

      parallel::RegionPlan plan = parallel::planRegion(stmts, following, session);
      // A per-statement refusal at position k does not condemn the statements
      // BEFORE it. Re-plan the prefix once — measured on the acceptance corpus,
      // the 18-call batch is followed immediately by `timer off`, and without
      // this retry that one control statement took the whole batch down with it.
      // One retry, not a loop: the prefix's own refusal (if any) is reported as
      // the region's reason, and a second bad statement inside the prefix means
      // the prefix was never viable either.
      if (!plan.approved && plan.failedAt >= parallel::kAutoMinRegion &&
          plan.failedAt < stmts.size()) {
        std::vector<std::string> head(stmts.begin(),
                                      stmts.begin() + static_cast<long>(plan.failedAt));
        std::vector<std::string> tailAfter(stmts.begin() + static_cast<long>(plan.failedAt),
                                           stmts.end());
        tailAfter.insert(tailAfter.end(), following.begin(), following.end());
        parallel::RegionPlan headPlan = parallel::planRegion(head, tailAfter, session);
        if (headPlan.approved) {
          plan = headPlan;
          stmts = head;
          runEnd = itemIdx + head.size();
        }
      }
      const bool forced = !plan.approved && parallel::autoModeUnsafeForce();
      if (forced) {
        std::fprintf(stderr,
                     "[piet_native] auto-parallel: PIET_PARALLEL_AUTO_UNSAFE_FORCE is set — "
                     "approving a region the predicate REFUSED (%s). This exists to prove the byte "
                     "gate can go RED; never set it in a real run.\n",
                     plan.refusalReason.c_str());
      }
      if (plan.approved || forced) {
        const int firstLine = lineno;
        if (plan.approved && plan.count < stmts.size()) stmts.resize(plan.count);
        // 2026-09-02: resolve ONCE, with the region's own work as input (task
        // count, inherited dataset, scratch volume) — the second resolve that
        // used to re-enter the derived N on the explicit path is gone.
        parallel::ResolveContext rctx;
        rctx.taskCount = static_cast<int64_t>(stmts.size());
        rctx.residentBytes = parallel::residentBytesEstimate(session);
        rctx.scratchDir = session.workdir();
        parallel::WorkerBudget budget =
            parallel::resolveWorkers(parallel::autoModeRequestedWorkers(), rctx);
        const int workers = budget.n;
        const int lastLine = lineno + static_cast<int>(stmts.size()) - 1;
        std::string parentHead;
        bool canRun = workers > 1 && stmts.size() > 1;
        if (canRun && session.hasData()) {
          try {
            session.datastore();
            if (session.currentHbin().has_value()) parentHead = *session.currentHbin();
          } catch (const std::exception&) {
            parentHead.clear();
          }
          if (parentHead.empty()) canRun = false;
        }
        std::string serialWhy;
        if (workers <= 1)
          serialWhy = "the worker budget resolved to 1 (" + budget.reason + ")";
        else if (stmts.size() <= 1)
          serialWhy = "the region is a single statement";
        else if (!canRun)
          serialWhy = "the resident dataset could not be snapshotted for the workers";
        std::vector<parallel::TaskOutcome> outcomes;
        if (canRun) {
          static std::atomic<unsigned long long> autoSeq{0};
          const std::string scratchDir =
              session.workdir() + "/auto_parallel_" + std::to_string(autoSeq.fetch_add(1));
          outcomes = parallel::runTasksInWorkers(stmts, session, budget, parentHead, scratchDir);
        }
        if (!outcomes.empty()) {
          parallel::reportApproved(stmts.size(), workers, firstLine, lastLine);
          bool halted = false;
          for (size_t t = 0; t < outcomes.size(); ++t) {
            const int taskLine = firstLine + static_cast<int>(t);
            parallel::TaskOutcome& o = outcomes[t];
            if (!o.delivered) {
              ojson eb = eval::engineError(
                  "auto-parallel: task " + std::to_string(t + 1) + " did not complete (" +
                  (o.driverError.empty() ? std::string("worker died") : o.driverError) + ")");
              pushBlock(eb);
              if (handleUncapturedError(eb, taskLine, out)) { halted = true; break; }
              continue;
            }
            for (const auto& [cname, slot] : piet::counters::foldableAccumulators()) {
              if (!o.countersDelta.contains(cname)) continue;
              const auto& v = o.countersDelta[cname];
              if (v.is_number_integer()) *slot += v.get<int64_t>();
            }
            coalesceMutationVarlists(o.blocks);
            session.logTee(o.blocks, 1);
            for (const auto& b : o.blocks) {
              pushBlock(b);
              if (handleUncapturedError(b, taskLine, out)) { halted = true; break; }
            }
            if (halted) break;
            if (o.hasExitCode) {
              out.stoppedAtLine = taskLine;
              out.stoppedAtExitCode = o.exitCode;
              halted = true;
              break;
            }
          }
          lineno = lastLine;
          itemIdx = itemIdx + stmts.size() - 1;
          if (halted) {
            if (progressOwner) session.progressFinish("stopped", static_cast<int>(out.blocks.size()));
            return out;
          }
          continue;
        }
        if (serialWhy.empty()) serialWhy = "fan-out could not be started";
        if (plan.reportable)
          parallel::reportApprovedButSerial(firstLine, lastLine, serialWhy);
        // The region runs serially from here; re-planning its remaining
        // statements would say the same thing once per statement.
        autoRefusedUntil = itemIdx + stmts.size();
      } else {
        // Resume at the statement AFTER the one that failed. Writing off the
        // whole run would make every statement behind a single `local` or
        // `capture` unreachable — measured on the acceptance corpus, that alone
        // hid the entire 18-call batch behind a preamble assignment.
        autoRefusedUntil = itemIdx + plan.failedAt + 1;
        if (autoRefusedUntil <= itemIdx) autoRefusedUntil = itemIdx + 1;
        if (plan.reportable)
          parallel::reportRefused(lineno, lineno + static_cast<int>(stmts.size()) - 1,
                                  plan.refusalReason);
      }
      // Refused, or fan-out unavailable: fall through to the ordinary serial
      // path for THIS item. The run is re-examined only at the next flat item
      // that begins a new candidate, which keeps the scan linear.
    }

    if (session.progressCancelRequested()) {
      ojson cancelBlk = cancelledBlock();
      out.blocks.push_back(cancelBlk);
      out.stoppedAtLine = lineno;
      out.stoppedAtReason = cancelBlk.value("message", "run cancelled");
      if (progressOwner) session.progressFinish("cancelled", static_cast<int>(out.blocks.size()));
      return out;
    }

    if (progressOwner) {
      std::string cur = isBraceBlock(item) ? asBraceBlock(item)->header : std::get<std::string>(item);
      session.progressUpdate(lineno, cur, static_cast<int>(out.blocks.size()));
    }

    // T6 PIET-SIDE-SPLIT (report-only): time this top-level item and, of that,
    // how long it sat blocked on the engine. `wall - engine` is the PIET-side
    // cost the campaign's shim could never see, bucketed by the item's leading
    // token so the split reads per verb. Scoped so `continue`/`return` inside
    // the body still bill correctly.
    //
    // CORRECTED 2026-08-12 (ATTR). This comment used to end: "Only the
    // TOP-LEVEL owner records (nested executeDoSource calls run under a live
    // scope already) — otherwise a loop body's items would be double-counted
    // inside their header's own total." No such guard was ever written, and
    // `loops::executeLoop` runs the body BACK through executeDoSource
    // (loops.cpp), so every item is billed at every depth while its parent's
    // timer stays open around it — the double-counting the comment said could
    // not happen is precisely what happens. Measured, 50,000 iterations, body
    // 1 line vs 3, all else fixed:
    //     body=1  forvalues wall  79.21ms   local n= 50,000 wall  66.73ms
    //     body=3  forvalues wall 235.39ms   local n=150,000 wall 213.86ms
    // The header's wall tracks its body's: parents are INCLUSIVE, rows sit at
    // mixed depths, and they do not sum. That is what forced "capture ×23,237
    // = 59.8 min / 행 합산 금지" onto the 2026-07-24 full-run table
    // (p0_landings §T6). verbTimings() is deliberately left as it was so that
    // record stays comparable; the levels are separated in the new surfaces
    // instead (counters.h, ATTR block).
    // A3/A1: the accumulator is inert unless someone reads it, but it was not
    // FREE — two steady_clock reads, a per-item verb string, and a
    // std::map<std::string,…> lookup, on every line of every loop body. A
    // symbolized profile of a 200k-iteration macro loop put **9.3% of all
    // samples in mach_absolute_time** on a workload that never touches the
    // engine. Gated on the same env var that gates the EMIT (main.cpp), so the
    // instrument is exactly as available as before when asked for, and costs
    // one predictable branch when not. Static: the answer cannot change inside
    // a process.
    // ATTR-20260812: was a function-local static reading getenv directly. Now
    // one plain global load through timingLevel(), so the level (not just
    // on/off) is available here and a selftest leg can arm the depth-split
    // surfaces in-process. Same single predictable branch when not asked for.
    const bool kTimingOn = piet::counters::timingLevel() > 0;
    struct ItemTimer {
      bool active;
      int depth;
      int line;
      std::string verb;  // raw leading token — the level-1 key, unchanged
      std::string text;  // depth-1 only: the item's own source text (level >= 2)
      std::chrono::steady_clock::time_point t0;
      int64_t eng0;
      // INSPECT-2608 (level 4): the counter snapshot this item opened with.
      // Empty below level 4, so the 17 loads are not paid when unasked.
      std::vector<int64_t> cnt0;
      ~ItemTimer() {
        if (!active) return;
        const int64_t wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        const int64_t eng = piet::counters::engineWaitNs() - eng0;
        piet::counters::addVerbTiming(verb, wall, eng);
        // ATTR-20260812: same measurement, told at ONE nesting level at a time.
        // Depth 1 rows sum to the run; deeper rows are the inside-the-loop view
        // and are kept in a separate map so the two can never be added together
        // by accident (see counters.h for the probe that found the mixing).
        const int lvl = piet::counters::timingLevel();
        if (lvl >= 2) {
          std::string canon = piet::counters::takeResolvedVerb(depth);
          if (canon.empty()) canon = verb;  // brace headers never reach the registry
          if (depth <= 1) {
            piet::counters::addItemRecord(line, canon, text, wall, eng);
          } else if (lvl >= 3) {
            piet::counters::addNestedTiming(canon, wall, eng);
          }
          // The only view that SUMS: this item's wall minus its children's.
          // Every depth participates, because the corpus's whole run is one
          // depth-1 program call and an exclusive table that skipped depth 1
          // would drop the driver's own cost on the floor.
          std::pair<int64_t, int64_t> self{0, 0};
          if (lvl >= 3) self = piet::counters::addExclusiveTiming(canon, depth, wall, eng);
          // INSPECT-2608: the same exclusive discipline, applied to the byte and
          // mechanism counters. Keyed by FORM (verb + qualifier presence) so
          // `summarize` and `summarize|if` do not average together; the
          // dispatcher fills the slot, and a brace header that never reaches the
          // registry falls back to the plain verb.
          if (lvl >= 4) {
            std::string form = piet::counters::takeResolvedForm(depth);
            if (form.empty()) form = canon;
            piet::counters::addPercmdDelta(form, depth, cnt0, self.first, self.second);
          }
        }
      }
    } __itimer{kTimingOn,
               piet::counters::g_doexecDepth,
               static_cast<int>(lineno),
               [&]() {
                 if (!kTimingOn) return std::string();
                 // Open this item's resolution slot. First resolution at THIS
                 // depth wins; a body line resolves at depth+1 and cannot reach
                 // back into its header's slot.
                 piet::counters::clearResolvedVerb(piet::counters::g_doexecDepth);
                 piet::counters::clearResolvedForm(piet::counters::g_doexecDepth);
                 std::string cur = isBraceBlock(item) ? asBraceBlock(item)->header
                                                      : std::get<std::string>(item);
                 size_t a = cur.find_first_not_of(" \t");
                 if (a == std::string::npos) return std::string("(blank)");
                 size_t b = cur.find_first_of(" \t", a);
                 std::string v = cur.substr(a, b == std::string::npos ? std::string::npos : b - a);
                 if (!v.empty() && (v.back() == '{' || v.back() == ',')) v.pop_back();
                 return v.empty() ? std::string("(blank)") : v;
               }(),
               // Depth-1 item text, for levels that report per item. Depth 2+
               // is verb-summed, so the string would be built and dropped —
               // and depth 2+ is the hot side (every loop-body line).
               (kTimingOn && piet::counters::g_doexecDepth <= 1 &&
                piet::counters::timingLevel() >= 2)
                   ? (isBraceBlock(item) ? asBraceBlock(item)->header
                                         : std::get<std::string>(item))
                   : std::string(),
               // The clock read has to be conditional too: an aggregate
               // initializer evaluates every member regardless of what the
               // destructor later decides to do with them, so gating only
               // `active` left mach_absolute_time at 8.3% of a profile that
               // was supposed to have stopped paying for it.
               kTimingOn ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{},
               kTimingOn ? piet::counters::engineWaitNs() : 0,
               // Same conditional-member rule as the clock read above: an
               // aggregate initializer evaluates every member, so this must be
               // gated here and not only in the destructor.
               (kTimingOn && piet::counters::timingLevel() >= 4)
                   ? piet::counters::percmdSnapshot()
                   : std::vector<int64_t>{}};

    if (isBraceBlock(item)) {
      const auto& bb = asBraceBlock(item);
      std::vector<std::string> bodyFlat = flattenBody(bb->body);
      std::vector<std::string> elseFlat = flattenBody(bb->elseBody);
      ojson blocks = loops::executeLoop(bb->header, bodyFlat, elseFlat, session, dispatchFn);
      // F8: one varlist card per top-level command.
      //
      // EXCEPT for a `parallel` block, which already thinned PER TASK inside
      // loops::runParallelBlock. Its contract (design brief §2.5) is that the
      // tasks emit what they would have emitted as ordinary top-level
      // statements, so one card per task is the correct count and re-thinning
      // the whole block here would strip every task's card but the last. Every
      // other brace form genuinely IS one command and keeps the single-card
      // rule. Caught by the probe's wrapper-less A0 arm: the arms under test
      // lost the cards identically, so cmp(A1,AN) was blind to it.
      if (!loops::isParallelHeader(bb->header)) coalesceMutationVarlists(blocks);

      // U7/S11: a nested if/loop may bubble a _control sentinel as the LAST
      // block. Filter it out and propagate.
      std::optional<std::string> ctrl;
      for (const auto& b : blocks) {
        if (b.value("type", "") == "_control") {
          ctrl = b.value("action", "");
        } else {
          pushBlock(b);
        }
      }
      if (ctrl.has_value()) {
        out.control = ctrl;
        if (progressOwner) session.progressFinish("completed", static_cast<int>(out.blocks.size()));
        return out;
      }
      for (const auto& b : blocks) {
        // BS-127: capture-forwarded errors (tagged captured:true by the
        // dispatcher's capture arm) are display-only — Stata shows them,
        // sets _rc, and CONTINUES the do-file. Only untagged errors halt.
        if (handleUncapturedError(b, lineno, out)) {
          if (progressOwner) {
            session.progressFinish(out.stoppedAtExitCode.has_value() ? "completed" : "error",
                                   static_cast<int>(out.blocks.size()));
          }
          return out;
        }
      }
    } else {
      const std::string& stripped = std::get<std::string>(item);
      if (trim(stripped).empty()) continue;

      // BUG-920: a close brace with code after it (`} else if c {`, `} else {`)
      // is r(198) "code follows on the same line as close brace" in Stata
      // (oracle w2_prog_controlflow pc_if_brace_else, log 858-860: the block
      // runs up to that line, then the line refuses). collectBody keeps such a
      // line in the body (its braces net to zero), so it reached the
      // dispatcher as a command named `}` and came back r(199).
      ojson blocks;
      {
        const std::string t = trim(stripped);
        const std::string after = t.size() > 1 && t[0] == '}' ? trim(t.substr(1)) : std::string();
        if (!after.empty() && after.rfind("//", 0) != 0 && after.rfind("/*", 0) != 0) {
          blocks = ojson::array({ojson(eval::errorBlock(
              "syntax_error", "code follows on the same line as close brace", t, std::nullopt,
              std::nullopt, std::string("r(198)")))});
        } else {
          blocks = dispatchFn(stripped, session);
        }
      }
      coalesceMutationVarlists(blocks);  // F8: one varlist card per top-level command
      std::optional<std::string> ctrl;
      for (const auto& b : blocks) {
        if (b.value("type", "") == "_control") {
          ctrl = b.value("action", "");
        } else {
          pushBlock(b);
        }
      }
      if (ctrl.has_value()) {
        out.control = ctrl;
        if (progressOwner) session.progressFinish("completed", static_cast<int>(out.blocks.size()));
        return out;
      }
      for (const auto& b : blocks) {
        // BS-127: capture-forwarded errors (tagged captured:true by the
        // dispatcher's capture arm) are display-only — Stata shows them,
        // sets _rc, and CONTINUES the do-file. Only untagged errors halt.
        if (handleUncapturedError(b, lineno, out)) {
          if (progressOwner) {
            session.progressFinish(out.stoppedAtExitCode.has_value() ? "completed" : "error",
                                   static_cast<int>(out.blocks.size()));
          }
          return out;
        }
      }
    }

    // B2 NAMED-OWED ③: high-water sample after this top-level item completed
    // (counter/cache-neutral; env-gated + top-level-only via sampleNamedPeak).
    if (sampleNamedPeak) {
      counters::updateLiveNamedBytesPeak(session.liveNamedBytesPeakSample());
    }
  }

  if (progressOwner) session.progressFinish("completed", static_cast<int>(out.blocks.size()));
  return out;
}

PrivateLocalScope::PrivateLocalScope(SessionState& session) : session_(session) {
  const std::string* callerRc = session_.eval.locals.find("_rc");
  eval::OrderedDict fresh;
  if (callerRc != nullptr) fresh.set("_rc", *callerRc);
  // BUG-1330: a do/run file starts under its caller's `version`; its own
  // `version #` ends with it, as the scope pops.
  for (const char* key : {kVersionLocal, kUserVersionLocal})
    if (const std::string* ver = session_.eval.locals.find(key)) fresh.set(key, *ver);
  session_.scopeStack.push_back(std::move(session_.eval.locals));
  session_.eval.locals = std::move(fresh);
  depth_ = session_.scopeStack.size();
}

PrivateLocalScope::~PrivateLocalScope() {
  if (session_.scopeStack.size() < depth_) return;  // already unwound by someone else
  const std::string* fileRc = session_.eval.locals.find("_rc");
  std::optional<std::string> carry;
  if (fileRc != nullptr) carry = *fileRc;
  session_.eval.locals = std::move(session_.scopeStack[depth_ - 1]);
  session_.scopeStack.resize(depth_ - 1);
  if (carry.has_value()) session_.eval.locals.set("_rc", *carry);
}

std::string withDefaultDoExtension(const std::string& filepath) {
  const std::string leaf = std::filesystem::path(filepath).filename().u8string();
  if (!leaf.empty() && leaf.find('.') == std::string::npos) return filepath + ".do";
  return filepath;
}

ExecuteFileResult executeDo(const std::string& filepath, SessionState& session,
                            const DispatchFn& dispatchFn, const std::vector<std::string>& args,
                            BlockSink onBlock) {
  ExecuteFileResult out;

  // W2 doinclude (BUG-528): [R] limits allows 64 nested do-files. Refuse the
  // 65th level instead of recursing without bound -- a file that does or
  // includes itself used to run until the C++ stack overflowed. The rc Stata
  // answers here is not oracle-covered (engine_error, coarse rc 1).
  if (session.doExecDepth >= kMaxDoNesting) {
    const std::string msg = "do-file nesting too deep: more than " +
                            std::to_string(kMaxDoNesting) + " nested do/run/include files";
    out.result.blocks.push_back(ojson(eval::engineError(msg)));
    out.result.stoppedAtLine = 0;
    out.result.stoppedAtReason = msg;
    return out;
  }

  // BUG-174 (W:P8, User-found): a RELATIVE do-file path is resolved against the
  // SESSION's working directory, exactly like every other file-taking verb.
  // This used to call std::filesystem::absolute() and open `filepath` directly,
  // both of which use the PROCESS cwd — so inside one script `cd "/somewhere"`
  // then `use "data.dta"` worked (that path goes through resolveUserPath) while
  // `do "helper.do"` on the next line looked in the process's own directory and
  // reported "file not found". A GUI-launched session makes that split
  // permanent: its process cwd is wherever the launcher left it, and
  // session.cwd is the seeded ~/Documents (main.cpp::defaultUserCwd), so a
  // do-file calling another do-file beside it could not be made to work by any
  // amount of `cd`. resolveUserPath() is the one resolver — reused here rather
  // than reimplemented, so `~` expansion and the absolute-path rules cannot
  // drift between the two.
  const std::string userPath = piet::resolveUserPath(session, filepath);

  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::absolute(cecon::to_native_path(userPath), ec);
  if (!ec) {
    resolved = std::filesystem::weakly_canonical(resolved, ec);
  }
  out.resolvedPath = ec ? userPath : resolved.u8string();

  std::ifstream f(cecon::to_native_path(userPath), std::ios::binary);
  if (!f) {
    out.result.blocks.push_back(ojson(eval::engineError("file not found: " + filepath)));
    out.result.stoppedAtLine = 0;
    out.result.stoppedAtReason = "file not found: " + filepath;
    out.source = "";
    return out;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out.source = ss.str();

  {
    // S5/U4: clean up the per-run tempdir (best-effort — mirrors doexec.py's
    // finally block). BUG-112(7): reapGuard constructed BEFORE depthGuard so
    // it destructs AFTER (LIFO on scope exit, including via an exception
    // unwinding out of executeDoSource) — see TempdirReapGuard above.
    TempdirReapGuard reapGuard(session);
    // BUG-106: depth-guarded so a nested `do`/`run` FILE command (which
    // recurses back into this same function) does not trip the reap above —
    // only the outermost executeDo call should. session.runTempdir backs
    // every `tempfile` local for the WHOLE top-level run (including any
    // nested do-files it calls), so a nested `do FILE`/`run FILE` finishing
    // must NOT reap it out from under a tempfile the script still means to
    // `use` later at the outer scope.
    DoExecDepthGuard depthGuard(session);
    out.result = executeDoSource(out.source, session, dispatchFn, args, /*progressOwner=*/true,
                                  out.resolvedPath, std::move(onBlock));
  }

  return out;
}

}  // namespace piet::doexec
