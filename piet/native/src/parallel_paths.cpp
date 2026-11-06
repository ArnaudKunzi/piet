// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · call-site path substitution. See parallel_paths.h for the
// measured shapes this models and the one-sidedness argument.
#include "piet/parallel_paths.h"

#include <atomic>
#include <cctype>
#include <regex>
#include <set>

#include "piet/strutil.h"

namespace piet::parallel {

namespace {

std::string lower(const std::string& s) { return strutil::lower(s); }
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string firstWord(const std::string& s) {
  std::string t = trim(s);
  size_t i = 0;
  while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != ',') ++i;
  return t.substr(0, i);
}

// A program's parameter names, by 1-based position. Two spellings, both measured
// in the acceptance corpus:
//   `args a b c`                       — the textbook form
//   `local rest `"`0'"'`               — followed by a run of
//   `gettoken NAME rest : rest, bind`    gettoken, which IS positional
//                                        extraction written long-hand.
// The second is what the corpus actually uses, and a model that only knew the
// first would resolve nothing there.
std::map<std::string, int> paramPositions(const std::vector<std::string>& body) {
  std::map<std::string, int> pos;
  std::string argStream;  // the local currently holding the unconsumed arguments
  int next = 1;
  static const std::regex reLocalFromZero(
      R"(^local\s+([A-Za-z_][A-Za-z0-9_]*)\s+`\"?`0'\"?'?\s*$)");
  static const std::regex reGettoken(
      R"(^gettoken\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*))");
  for (const std::string& raw : body) {
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '*') continue;
    const std::string head = lower(firstWord(line));
    if (head == "args") {
      std::string rest = trim(line.substr(firstWord(line).size()));
      for (const std::string& n : splitStataArgs(rest)) pos[n] = next++;
      continue;
    }
    std::smatch m;
    if (std::regex_search(line, m, reLocalFromZero)) {
      argStream = m.str(1);
      continue;
    }
    if (!argStream.empty() && std::regex_search(line, m, reGettoken)) {
      // gettoken NAME REST : SRC — only counts while it is consuming the stream
      // we know came from `0'.
      if (m.str(3) == argStream) {
        pos[m.str(1)] = next++;
        argStream = m.str(2);  // the remainder carries on
      }
    }
  }
  return pos;
}

bool parsePathTemplate(const std::string& quoted, const std::map<std::string, int>& params,
                       PathTemplate* out, const std::set<std::string>* temps,
                       const std::map<std::string, std::string>* aliases, int depth);

// Parse a quoted path into literal chunks and `name' symbols.
bool parsePathTemplate(const std::string& quoted, const std::map<std::string, int>& params,
                       PathTemplate* out, const std::set<std::string>* temps = nullptr,
                       const std::map<std::string, std::string>* aliases = nullptr,
                       int depth = 0) {
  std::string lit;
  for (size_t i = 0; i < quoted.size();) {
    if (quoted[i] == '`') {
      const size_t close = quoted.find('\'', i + 1);
      if (close == std::string::npos) return false;
      const std::string name = quoted.substr(i + 1, close - i - 1);
      if (name.empty() || name.find('`') != std::string::npos) return false;
      if (!lit.empty()) {
        out->parts.push_back(PathPart{true, lit, 0});
        lit.clear();
      }
      // Resolve the symbol. A parameter becomes a hole; a `tempfile` becomes a
      // discriminator; a local assigned earlier in the body is EXPANDED into its
      // own parts, recursively — the corpus builds its graph stub through
      // several hops of composition (`local stub "`a'_`b'"`), and a one-hop
      // model leaves that as one opaque blob that every task renders alike.
      auto it = params.find(name);
      if (it != params.end()) {
        out->parts.push_back(PathPart{false, name, it->second, false});
      } else if (temps != nullptr && temps->count(name) != 0) {
        PathPart tp{false, name, 0, false};
        tp.isTemp = true;
        out->parts.push_back(tp);
      } else if (aliases != nullptr && depth < 6 && aliases->count(name) != 0) {
        PathTemplate sub;
        if (!parsePathTemplate(aliases->at(name), params, &sub, temps, aliases, depth + 1))
          return false;
        for (const PathPart& sp : sub.parts) out->parts.push_back(sp);
      } else {
        out->parts.push_back(PathPart{false, name, 0, false});
      }
      i = close + 1;
      continue;
    }
    if (quoted[i] == '$') return false;  // a global in a path: not modelled, refuse
    lit += quoted[i++];
  }
  if (!lit.empty()) out->parts.push_back(PathPart{true, lit, 0});
  return true;
}

// The first double-quoted run in a line (the destination of every write form the
// corpus uses: `graph export "..."`, `save "..."`, `export delimited using "..."`).
bool firstQuoted(const std::string& line, std::string* out) {
  const size_t a = line.find('"');
  if (a == std::string::npos) return false;
  const size_t b = line.find('"', a + 1);
  if (b == std::string::npos) return false;
  *out = line.substr(a + 1, b - a - 1);
  return true;
}

bool isWriteVerb(const std::string& head, const std::string& line) {
  if (head == "save" || head == "outsheet" || head == "outfile") return true;
  if (head == "export") return true;
  if (head == "graph") {
    const std::string rest = lower(trim(line.substr(5)));
    return rest.rfind("export", 0) == 0 || rest.rfind("save", 0) == 0;
  }
  return false;
}


// An actual argument is itself a small template: `"`a'_`b'_`c'"` is three
// symbols and two literals, not one opaque blob. Treating it as a blob was a
// real defect — every call site produced the SAME key for
// `save_empty_plot "`pg_output_dir'" "`pg_season_scope'_`pg_analysis_mode'_`pg_group_name'"`,
// so the disjointness check reported a collision between tasks that in fact
// write different files.
std::vector<PathPart> spliceActual(const std::string& actual,
                                   const std::map<std::string, int>& params,
                                   const std::set<std::string>* temps = nullptr) {
  PathTemplate t;
  if (!parsePathTemplate(actual, params, &t, temps)) {
    // Unparsable: keep it whole and OPAQUE, which is the conservative reading —
    // two such arguments compare equal only if their text is identical.
    return {PathPart{false, actual, 0}};
  }
  return t.parts;
}

}  // namespace

std::vector<std::string> splitStataArgs(const std::string& text) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) break;
    std::string tok;
    if (text.compare(i, 2, "`\"") == 0) {  // compound quote `"..."'
      const size_t close = text.find("\"'", i + 2);
      if (close == std::string::npos) {
        tok = text.substr(i);
        i = text.size();
      } else {
        tok = text.substr(i + 2, close - i - 2);
        i = close + 2;
      }
    } else if (text[i] == '"') {
      const size_t close = text.find('"', i + 1);
      if (close == std::string::npos) {
        tok = text.substr(i + 1);
        i = text.size();
      } else {
        tok = text.substr(i + 1, close - i - 1);
        i = close + 1;
      }
    } else {
      const size_t start = i;
      while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      tok = text.substr(start, i - start);
    }
    out.push_back(tok);
  }
  return out;
}

ProgramPaths programWritePaths(const std::string& program, SessionState& session, int maxDepth) {
  ProgramPaths out;
  if (maxDepth <= 0) {
    out.why = "program nesting deeper than this layer follows";
    return out;
  }
  auto it = session.programs.find(program);
  if (it == session.programs.end()) {
    out.why = "`" + program + "` is not a defined program";
    return out;
  }
  const std::map<std::string, int> params = paramPositions(it->second.body);
  out.ok = true;
  // Body-minted tempfiles are already contained by the effects layer: the name is
  // minted per invocation and, in a worker, per process. Modelling them here as
  // ordinary destinations produces a FALSE collision — every task renders the
  // same single opaque symbol (measured: `opponent_map`), so two tasks that
  // cannot possibly share a file were reported as writing the same one.
  std::set<std::string> tempNames;
  std::map<std::string, std::string> aliases;   // local -> the text it copies
  std::set<std::string> conditional;            // locals assigned differently on two paths

  for (const std::string& raw : it->second.body) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '*') continue;
    // Prefixes that do not change the destination.
    static const std::regex rePrefix(
        R"(^\s*(cap(?:ture)?|noisily|noi|quietly|qui)\s+)", std::regex::icase);
    std::smatch pm;
    while (std::regex_search(line, pm, rePrefix))
      line = trim(line.substr(static_cast<size_t>(pm.length(0))));
    const std::string head = lower(firstWord(line));

    if (head == "local") {
      // `local X `Y'` (optionally wrapped in quotes) is a one-hop alias. When a
      // local is assigned more than once we keep the NON-temp binding: the
      // destination could take either branch at run time, and the branch we
      // cannot prove disjoint is the one that has to decide.
      static const std::regex reLocalAssign(
          R"(^local\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.*)$)");
      std::smatch am;
      if (std::regex_search(line, am, reLocalAssign)) {
        const std::string lhs = am.str(1);
        std::string rhs = trim(am.str(2));
        // Strip one layer of quoting so the stored text is the path expression
        // itself: `"..."' and "..." both wrap the value, not part of it.
        if (rhs.size() > 3 && rhs.compare(0, 2, "`\"") == 0 && rhs.compare(rhs.size() - 2, 2, "\"'") == 0)
          rhs = rhs.substr(2, rhs.size() - 4);
        else if (rhs.size() > 1 && rhs.front() == '"' && rhs.back() == '"')
          rhs = rhs.substr(1, rhs.size() - 2);
        // An `=` assignment is an EXPRESSION, not a path composition — it cannot
        // be expanded textually and must stay opaque.
        if (!rhs.empty() && rhs[0] == '=') { aliases.erase(lhs); continue; }
        auto prev = aliases.find(lhs);
        if (prev != aliases.end() && prev->second != rhs) {
          // Assigned twice with DIFFERENT text: the destination depends on a
          // run-time branch this layer cannot evaluate. Measured in the corpus:
          //   local graph_stub "`plot_stub'"      (when the caller supplied one)
          //   local graph_stub "`output_suffix'"  (otherwise)
          // Picking either branch would be a guess, and a guess in this
          // direction is a wrong-answer generator — so the name becomes
          // UNRESOLVABLE and rule 10 refuses with that as its reason, rather
          // than claiming two tasks write the same file when they do not.
          conditional.insert(lhs);
          aliases.erase(lhs);
        } else if (conditional.count(lhs) == 0) {
          aliases[lhs] = rhs;
        }
      }
      continue;
    }

    if (head == "tempfile" || head == "tempname" || head == "tempvar") {
      for (const std::string& n : splitStataArgs(trim(line.substr(firstWord(line).size()))))
        tempNames.insert(n);
      continue;
    }

    if (isWriteVerb(head, line)) {
      bool viaTemp = false;
      for (const std::string& tn : tempNames)
        if (line.find("`" + tn + "'") != std::string::npos) { viaTemp = true; break; }
      if (viaTemp) continue;  // task-local by construction
      std::string quoted;
      if (!firstQuoted(line, &quoted)) {
        // A write with no quoted destination — a bare `save \`tmp'` say. The
        // effects layer already contains tempfile-destined writes; anything else
        // here is a shape this modeller does not know.
        out.ok = false;
        out.why = "in `" + program + "`: a file write whose destination is not a quoted path";
        return out;
      }
      bool cond = false;
      for (const std::string& c : conditional)
        if (quoted.find("`" + c + "'") != std::string::npos) { cond = true; break; }
      if (cond) {
        out.ok = false;
        out.why = "in `" + program + "`: the destination depends on a run-time branch";
        return out;
      }
      PathTemplate t;
      if (!parsePathTemplate(quoted, params, &t, &tempNames, &aliases, 0)) {
        out.ok = false;
        out.why = "in `" + program + "`: could not parse the destination `" + quoted + "`";
        return out;
      }
      out.templates.push_back(t);
      continue;
    }

    // A forwarded call: recurse and remap the callee's holes onto OUR positions.
    if (session.programs.count(firstWord(line)) != 0) {
      const std::string callee = firstWord(line);
      ProgramPaths sub = programWritePaths(callee, session, maxDepth - 1);
      if (!sub.ok) {
        out.ok = false;
        out.why = sub.why;
        return out;
      }
      const std::vector<std::string> actuals =
          splitStataArgs(trim(line.substr(firstWord(line).size())));
      for (const PathTemplate& st : sub.templates) {
        PathTemplate rt;
        for (const PathPart& p : st.parts) {
          if (p.isLiteral || p.paramPos == 0) {
            rt.parts.push_back(p);
            continue;
          }
          // The callee's position N is filled by OUR argument N.
          if (p.paramPos > static_cast<int>(actuals.size())) {
            out.ok = false;
            out.why = "in `" + program + "`: call to `" + callee + "` has no argument " +
                      std::to_string(p.paramPos);
            return out;
          }
          const std::string actual = actuals[static_cast<size_t>(p.paramPos) - 1];
          // Splice the actual in as parts: each `name' it contains rebinds to OUR
          // position when it names one of our parameters, so a hole survives one
          // more level up, and its literal chunks stay literal.
          for (const PathPart& sp : spliceActual(actual, params, &tempNames))
            rt.parts.push_back(sp);
        }
        out.templates.push_back(rt);
      }
    }
  }
  return out;
}

RenderedPath renderPath(const PathTemplate& t, const std::vector<std::string>& args) {
  RenderedPath out;
  static std::atomic<unsigned long long> tempSeq{0};
  std::string key;
  for (const PathPart& p : t.parts) {
    if (p.isLiteral) {
      key += p.text;
      continue;
    }
    if (p.isTemp) {
      // Unique per rendering: a tempfile path differs between any two
      // invocations, so two tasks going through one can never be the same file.
      key += "\x02" + std::to_string(tempSeq.fetch_add(1)) + "\x02";
      continue;
    }
    if (p.paramPos > 0) {
      if (p.paramPos > static_cast<int>(args.size())) return out;  // ok stays false
      const std::string a = args[static_cast<size_t>(p.paramPos) - 1];
      // The argument is spliced, not blobbed (see spliceActual): its literal
      // chunks contribute literally and each macro stays OPAQUE by name, so two
      // call sites differing only in a literal tag produce different keys.
      for (const PathPart& sp : spliceActual(a, {})) {
        if (sp.isLiteral) key += sp.text;
        else key += "\x01" + sp.text + "\x01";
      }
      continue;
    }
    key += "\x01" + p.text + "\x01";  // opaque local, unbound to any position
  }
  out.ok = true;
  out.key = key;
  return out;
}

}  // namespace piet::parallel
