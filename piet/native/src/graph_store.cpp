// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_store.cpp -- the graph-store subcommands (graph-family AI-4/AI-6/AI-7):
// `graph describe`, `graph copy`, `graph rename` and `graph display`/`graph draw`.
// The store is session.namedGraphs (the blocks),
// session.graphMeta (what describe reports, serset::makeGraphMeta),
// session.currentGraph and the graphs' sersets (piet/serset.h).
// Oracle: tests/eval/golden/aer_w2/w2ai_graph_family.log.
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/serset.h"
#include "piet/strutil.h"
#include "io/win_compat.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson refuseG(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson textBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

std::string metaStr(const ojson& m, const char* k) {
  if (!m.is_object() || !m.contains(k) || !m[k].is_string()) return std::string();
  return m[k].get<std::string>();
}

// The words of `text`, a "quoted" word kept whole without its quotes;
// `quoted[i]` says whether word i was quoted.
std::vector<std::string> quoteWords(const std::string& text, std::vector<bool>* quoted) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) break;
    if (text[i] == '"') {
      const size_t close = text.find('"', i + 1);
      out.push_back(text.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1));
      if (quoted) quoted->push_back(true);
      i = close == std::string::npos ? text.size() : close + 1;
      continue;
    }
    if (text.compare(i, 2, "`\"") == 0) {
      const size_t close = text.find("\"'", i + 2);
      out.push_back(text.substr(i + 2, close == std::string::npos ? std::string::npos : close - i - 2));
      if (quoted) quoted->push_back(true);
      i = close == std::string::npos ? text.size() : close + 2;
      continue;
    }
    size_t j = i;
    while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
    out.push_back(text.substr(i, j - i));
    if (quoted) quoted->push_back(false);
    i = j;
  }
  return out;
}

void postDescribe(SessionState& session, const std::string& fn, const ojson& meta) {
  eval::replaceRClass(session.eval);
  std::vector<std::string> keys;
  auto put = [&](const std::string& k, const std::string& v) {
    if (v.empty()) return;  // an empty macro is an unset one
    session.eval.rStrings.set(k, v);
    keys.push_back(k);
  };
  put("fn", fn);
  put("ft", "live");
  put("family", metaStr(meta, "family"));
  put("command", metaStr(meta, "command"));
  put("scheme", metaStr(meta, "scheme"));
  put("ysize", metaStr(meta, "ysize"));
  put("xsize", metaStr(meta, "xsize"));
  put("command_date", metaStr(meta, "date"));
  put("command_time", metaStr(meta, "time"));
  eval::declareRPosted(session.eval, "graph describe", keys);
}

std::string describeText(const std::string& bname, bool onDisk, const std::string& fn, const ojson& meta) {
  std::string t;
  t += bname + (onDisk ? " stored on disk" : " stored in memory") + "\n\n";
  t += "        name:  " + fn + "\n";
  t += "      format:  live\n";
  t += "     created:  " + metaStr(meta, "date") + " " + metaStr(meta, "time") + "\n";
  const std::string sch = metaStr(meta, "scheme");
  t += "      scheme:  " + (sch.empty() ? std::string("unknown") : sch) + "\n";
  t += "        size:  " + metaStr(meta, "ysize") + " x " + metaStr(meta, "xsize") + "\n";
  t += "    dta file:  (none)\n";
  t += "     command:  " + metaStr(meta, "command");
  return t;
}

}  // namespace

ojson runGraphDescribe(SessionState& session, const std::string& argText) {
  ojson out = ojson::array();
  std::vector<bool> q;
  std::vector<std::string> w = quoteWords(argText, &q);
  std::string name = w.empty() ? std::string() : w[0];
  const bool quoted = !q.empty() && q[0];
  if (name.empty()) {
    name = session.currentGraph;
    if (name.empty()) {
      out.push_back(refuseG("no current graph", 111));
      return out;
    }
  }
  // gr_describe nametype: quoted, or a dot in the base name, is a file
  std::string base = name;
  {
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
  }
  const bool isFile = quoted || base.find('.') != std::string::npos;
  if (!isFile) {
    auto it = session.namedGraphs.find(name);
    if (it == session.namedGraphs.end()) {
      out.push_back(refuseG("graph " + name + " not found", 111));
      return out;
    }
    const ojson meta = session.graphMeta.count(name) ? session.graphMeta[name] : serset::makeGraphMeta("twoway", "", "");
    postDescribe(session, name, meta);
    out.push_back(textBlock(describeText(name, false, name, meta)));
    return out;
  }
  // gs_fileinfo "name", suffix: .gph is assumed
  std::string fn = name;
  if (base.find('.') == std::string::npos) fn += ".gph";
  const std::string fp = resolveUserPath(session, fn);
  if (!cecon::native_exists(fp)) {
    out.push_back(refuseG("file " + fn + " not found", 601));
    return out;
  }
  ojson doc;
  {
    std::ifstream f(cecon::to_native_path(fp), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
      doc = ojson::parse(ss.str());
    } catch (...) {
      doc = ojson();
    }
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1") {
    out.push_back(refuseG("file " + fn + " is not a Stata .gph file", 610));
    return out;
  }
  const ojson meta = doc.contains("meta") ? doc["meta"] : serset::makeGraphMeta("twoway", "", "");
  postDescribe(session, fn, meta);
  std::string bname = fn;
  {
    const size_t slash = bname.find_last_of("/\\");
    if (slash != std::string::npos) bname = bname.substr(slash + 1);
  }
  out.push_back(textBlock(describeText(bname, true, fn, meta)));
  return out;
}

ojson runGraphCopyRename(SessionState& session, bool rename, const std::vector<std::string>& names,
                         bool replace) {
  ojson out = ojson::array();
  const std::string verb = rename ? "rename" : "copy";
  if (names.empty()) {
    out.push_back(refuseG("varlist required", 100));
    return out;
  }
  if (names.size() > 2) {
    out.push_back(refuseG("too many names specified", 103));
    return out;
  }
  for (const auto& n : names) {
    if (!strutil::isValidStataIdentifier(n)) {
      out.push_back(refuseG(n + " invalid name", 198));
      return out;
    }
  }
  std::string from, to;
  if (names.size() == 1) {
    from = session.currentGraph;
    to = names[0];
    if (from.empty()) {
      out.push_back(refuseG("no current graph", 111));
      return out;
    }
  } else {
    from = names[0];
    to = names[1];
  }
  if (session.namedGraphs.count(from) == 0) {
    out.push_back(refuseG("graph " + from + " not found", 111));
    return out;
  }
  if (from == to) return out;
  if (replace) {
    if (session.namedGraphs.count(to) != 0) {
      session.namedGraphs.erase(to);
      session.graphMeta.erase(to);
      serset::dropGraphSersets(session, {to}, false);
    } else {
      out.push_back(textBlock("(note: graph " + to + " not found)"));
    }
  } else if (session.namedGraphs.count(to) != 0) {
    out.push_back(refuseG("graph " + to + " already exists", 110));
    return out;
  }
  ojson block = session.namedGraphs[from];
  ojson meta = session.graphMeta.count(from) ? session.graphMeta[from] : serset::makeGraphMeta("twoway", "", "");
  if (block.is_object() && block.contains("name")) block["name"] = to;
  session.namedGraphs[to] = block;
  session.graphMeta[to] = meta;
  if (rename) {
    session.namedGraphs.erase(from);
    session.graphMeta.erase(from);
    session.sersets.renameOwner(from, to);
    if (session.currentGraph == from) session.currentGraph = to;
  } else {
    // gr_replay `from', name(`to') nodraw: the copy is the current graph
    session.currentGraph = to;
  }
  return out;
}

ojson runGraphDisplay(SessionState& session, const std::string& nameArg,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  ojson out = ojson::array();
  std::string name = strutil::trim(nameArg);
  if (name.empty()) name = session.currentGraph;
  if (name.empty() || session.namedGraphs.count(name) == 0) {
    out.push_back(refuseG("graph " + (name.empty() ? std::string("Graph") : name) + " not found", 111));
    return out;
  }
  ojson& meta = session.graphMeta[name];
  if (!meta.is_object()) meta = serset::makeGraphMeta("twoway", "", "");
  for (const auto& [k0, v] : opts) {
    const std::string k = strutil::lower(k0);
    if (v.isBool) continue;
    const std::string val = strutil::trim(v.strValue);
    if (k == "xsize" || k == "xsiz") meta["xsize"] = val;
    if (k == "ysize" || k == "ysiz") meta["ysize"] = val;
  }
  session.currentGraph = name;
  session.lastGraph = session.namedGraphs[name];
  ojson block = session.namedGraphs[name];
  decimateGraphBlockForView(block);
  out.push_back(block);
  return out;
}

}  // namespace piet::handlers
