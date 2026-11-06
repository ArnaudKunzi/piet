// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// gtools / ftools -- AER-SCOPE1 bundle C (2026-09-03).
//
// Ten community command names that are drop-in FAST replacements for commands
// PIET already implements. The campaign therefore spends ONE translation step
// instead of ten reimplementations: fold the head onto the native name, and
// decide -- by name, never by silence -- what happens to every option gtools
// adds on top of it.
//
// The three-way split, and why each arm is what it is:
//
//   DROP     The option describes SPEED or PRESENTATION only. `fast`, `verbose`,
//            `silent`, `nochecks`. The native answer is identical without them,
//            so forwarding them to a native net that has never heard of them
//            would turn a correct command into r(198) for no reason.
//
//   RENAME   Same meaning, different spelling (`greshape, by()` is `reshape,
//            i()`). Renaming is not cosmetic: the native handler compares
//            canonical keys, so an unrenamed key is ACCEPTED by nobody or, far
//            worse, accepted by the reject net and then never read.
//
//   REFUSE   Forwarding would change the ANSWER rather than the speed.
//            `gcollapse, merge` is a collapse-then-merge-back, i.e. not a
//            collapse at all; `gquantiles, by()` wants per-group cutpoints,
//            which is a different set of numbers under the same variable name.
//            These are refused BY NAME with rc 199, never dropped.
//
// Everything not in one of those three lists passes through UNTOUCHED, on
// purpose: the native command's own reject net is the grammar of record. A shim
// that also enumerated the native surface would be a second grammar, and two
// grammars drift.
//
// WHAT THIS FILE IS NOT. It is not a place to be clever about options the
// native command already handles. The only keys named below are the ones gtools
// ADDS or SPELLS DIFFERENTLY.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// Small shared shapes.
// ---------------------------------------------------------------------------

ojson gtErr(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// dispatcher.cpp's rawArgsAfterCmd, kept LOCAL. That copy lives in dispatcher's
// anonymous namespace and is not linkable from here; the rule it implements
// ("everything after the leading identifier token") carries no command-word
// knowledge at all, so this is not a second copy of a spelling set -- which is
// the thing BUG-232/BUG-251 was actually about. handlers_m3.cpp::runLog made
// the same call for the same reason.
//
// The npos discipline is load-bearing and is copied verbatim: a bare one-token
// line (`gisid`) has nothing after the identifier, and `substr(npos)` throws
// rather than returning empty. That threw once already, on the line `u`.
std::string rawArgsAfterCmdLocal(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return std::string();  // no leading identifier token at all
  size_t a = raw.find_first_not_of(" \t", e);
  return (a == std::string::npos) ? std::string() : strutil::trim(raw.substr(a));
}

// The pre-comma head of an argument string. eval::findUnquotedComma is the
// SAME scanner the tokenizer used to build parsed.opts from this very line, so
// reusing it guarantees the head this function returns and the options the
// caller walks came from one split point rather than two that can disagree.
// It is also already paren- and quote-aware, which is what keeps
// `percentiles(10, 50)` in one piece.
std::string headBeforeOpts(const std::string& args) {
  const long long comma = eval::findUnquotedComma(args);
  if (comma < 0) return strutil::trim(args);
  return strutil::trim(args.substr(0, static_cast<size_t>(comma)));
}

// Render an option list back into Stata option-tail text.
//
// Reassembly is NOT optional bookkeeping. runCollapse re-parses parsed.raw with
// its own `\bcollapse\b(.+?)` regex to recover the stat list, and runMerge
// re-parses it with `^\s*merge\s+(1:1|m:1|1:m)` to recover the merge type. An
// untouched raw line would still say `gcollapse` / `fmerge` there, and those
// two regexes would then match nothing (merge: an immediate syntax error) or
// the wrong span (collapse). The rewritten head has to reach them.
//
// eval::parseOpts only ever mints OptValue::ofBool(true) for a bare flag
// (tokenizer.cpp:373), so `isBool` means "present" and needs no value.
std::string serialiseOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string out;
  for (const auto& kv : opts) {
    if (!out.empty()) out += " ";
    out += kv.first;
    if (!kv.second.isBool) out += "(" + kv.second.strValue + ")";
  }
  return out;
}

// Whole-string numeric parse. A trailing-garbage value such as cuts(5x 95) must
// be REJECTED, not silently truncated to 5 (std::strtod's own behaviour) --
// a truncated cut is a different winsorization with no diagnostic. Same
// discipline the winsor2 arm already applies to its own cuts().
bool parseDoubleStrict(const std::string& s, double* out) {
  if (s.empty()) return false;
  const char* begin = s.c_str();
  char* end = nullptr;
  const double v = std::strtod(begin, &end);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

bool parseIntStrict(const std::string& s, long long* out) {
  if (s.empty()) return false;
  const char* begin = s.c_str();
  char* end = nullptr;
  const long long v = std::strtoll(begin, &end, 10);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

// ---------------------------------------------------------------------------
// (1)(2) The alias table, its option policy, and its option vocabulary.
// ---------------------------------------------------------------------------

struct AliasPolicy {
  std::string native;  // the command the head folds onto
  // The native command's OWN option vocabulary, COPIED rather than read back
  // from eval::findOptVocab(native) at registration time. Static-init order
  // across translation units is unspecified, and this file's registration must
  // be complete before dispatcher.cpp's canonicalizeForCommand runs on the
  // FIRST gtools line -- so a lazy read is too late and an eager read could see
  // an empty vocabulary. An empty vocabulary here is exactly defect class 3
  // (the option resolves to nothing, the reject net still accepts the prefix,
  // and the handler's canonical-key compare never matches), so the copy is the
  // safe side of that trade. Sources, to be edited in step with these:
  //   collapse   -- dispatcher.cpp's kVocabCollapse; the collapse arm runs a
  //                 net of its own per flavor, and runCollapse reads the
  //                 option keys the dispatcher resolved.
  //   egen       -- egen.cpp:38
  //   duplicates, levelsof, xtile -- dispatcher.cpp's file-scope block
  //   isid       -- dispatcher.cpp's arm rejects every option (empty vocabulary)
  //   reshape, merge -- handlers_data_verbs.cpp's file-scope block
  std::vector<std::string> nativeKnown;
  std::vector<std::string> nativeUnimplemented;
  // Surface gtools ADDS that the native command has no equivalent for and that
  // this file itself reads (the gquantiles mode words, gstats' own options).
  std::vector<std::string> extraKnown;
  std::vector<std::string> extraUnimplemented;
  std::vector<std::string> drop;
  std::vector<std::pair<std::string, std::string>> rename;  // from -> to
  std::vector<std::string> refuse;
};

const std::unordered_map<std::string, AliasPolicy>& aliasPolicies() {
  static const std::unordered_map<std::string, AliasPolicy> kTable = [] {
    std::unordered_map<std::string, AliasPolicy> t;

    // gcollapse / fcollapse -> collapse.
    // `merge` heads the refuse list because it is the one that looks most like a
    // modifier and is least like one: it collapses and then merges the result
    // BACK onto the uncollapsed data, so the row count after it is the row count
    // before, which is the opposite of what every consumer of a collapse assumes.
    //
    // `double` asks for a different storage width of the collapsed results,
    // which PIET cannot honour. It is refused by name rather than dropped:
    // dropping it would report success for something it did not do, a silent
    // answer change rather than a silent speed change.
    // `cw` is casewise deletion in all three (gcollapse.ado and fcollapse.ado
    // both declare it); runCollapse reads it off the option keys
    // (w2au_collapse_family.log cz_g_cw rc 0). `fast` and `verbose` reach the
    // collapse arm, which reads neither but answers either given an argument
    // r(198), as gtools does (BUG-1100, StataBE 19.5: `option fast()
    // incorrectly specified`, `option verbose() incorrectly specified`).
    const AliasPolicy collapseLike{
        "collapse",
        {"by", "cw"},
        {},
        {"fast", "verbose"},
        {},
        {},
        {},
        {"merge", "labelformat", "labelprogram", "freq", "rawstat", "unsorted", "missing",
         "wild", "sumcheck", "double", "pool", "register"}};
    t.emplace("gcollapse", collapseLike);
    // fcollapse (ftools, fcollapse) is its own row (BUG-1100): `fast`
    // reaches the collapse arm, where `smart` reads it, and nocompress (and its
    // positive spelling `compress`, the default), append and smart are served
    // there; merge, freq/freqvar(), register(), pool() and method() are refused
    // by name. gtools' own words are no fcollapse options at all, so the
    // collapse arm's net answers them r(198), as fcollapse does. No option
    // abbreviates below its syntax-line minimum (dispatcher.cpp
    // kOptMinCompleteLater), so a key reaches this row spelled in full.
    // `verbose` reaches the arm too, which prints nothing for it and answers
    // `verbose(1)` or `v(1)` r(198), as fcollapse does.
    t.emplace("fcollapse", AliasPolicy{"collapse",
                                       {"by", "cw", "fast"},
                                       {},
                                       {"nocompress", "compress", "append", "smart", "verbose"},
                                       {},
                                       {},
                                       {},
                                       {"merge", "freq", "freqvar", "register", "pool", "method"}});

    // gegen -> egen. `replace` overwrites an existing target, `counts`/`fill`
    // change what is computed; none of the three is a speed knob.
    t.emplace("gegen", AliasPolicy{"egen",
                                   {"by", "missing", "p"},
                                   {},
                                   {},
                                   {},
                                   {},
                                   {},
                                   {"replace", "counts", "fill"}});

    t.emplace("gduplicates", AliasPolicy{"duplicates",
                                         {"gen", "generate"},
                                         {},
                                         {},
                                         {},
                                         {"force"},
                                         {},
                                         {}});

    // glevelsof -> levelsof. `numfmt` changes how each level is SPELLED in the
    // returned macro, `unsorted` changes the order it is returned in, and `gen`
    // creates a variable levelsof would not have created: all three are visible
    // in the answer, none is a speed knob.
    t.emplace("glevelsof", AliasPolicy{"levelsof",
                                       {"local", "clean", "missing", "separate"},
                                       {},
                                       {},
                                       {},
                                       {"silent"},
                                       {},
                                       {"numfmt", "unsorted", "gen"}});

    // gisid -> isid. `missok` makes missing values legal keys, which is the
    // difference between "this is an id" and "this is not".
    t.emplace("gisid", AliasPolicy{"isid", {}, {}, {}, {}, {"fast"}, {}, {"missok"}});

    // greshape -> reshape. by()/keys() are i()/j() under different names.
    // `dropmiss` silently deletes rows reshape would have kept.
    t.emplace("greshape", AliasPolicy{"reshape",
                                      {"i", "j"},
                                      {},
                                      {},
                                      {},
                                      {"string", "nochecks", "fast"},
                                      {{"by", "i"}, {"keys", "j"}},
                                      {"dropmiss"}});

    // fmerge -> merge. uniqmaster/uniqusing assert a uniqueness the native
    // merge does not check, so accepting them would promise a check nobody
    // runs; `nolabel` changes the labels carried across.
    t.emplace("fmerge", AliasPolicy{"merge",
                                    {"keepusing", "nogenerate", "keep", "assert"},
                                    {"generate", "nolabel", "nonotes", "update", "replace",
                                     "noreport", "force", "sorted"},
                                    {},
                                    {},
                                    {},
                                    {},
                                    {"uniqmaster", "uniqusing", "nolabel"}});

    // gquantiles. The native twin depends on the MODE word (see readModeWord
    // below): xtile has one, _pctile and pctile do not and get their own arm in
    // this file. The vocabulary is registered once, for the alias name, and
    // covers both destinations.
    //
    // n()/nq() are gtools spellings of nquantiles(). `strict` only controls
    // whether gtools errors on a degenerate request, not what the answer is.
    // Everything in the refuse list changes the numbers: by() computes
    // per-group cutpoints, altdef switches to the alternate quantile
    // definition, cutpoints/cutoffs/cutquantiles/quantiles supply the bin edges
    // from somewhere other than the data, genp/binfreq/minmax create extra
    // variables, method picks a different algorithm, replace overwrites, and
    // dedup drops duplicate cutpoints so the bin COUNT stops matching nq.
    t.emplace("gquantiles",
              AliasPolicy{"xtile",
                          {"nquantiles", "cutpoints"},
                          {"altdef"},
                          {"percentiles", "xtile", "_pctile", "pctile", "n", "nq"},
                          {},
                          {"strict"},
                          {{"n", "nquantiles"}, {"nq", "nquantiles"}},
                          {"by", "altdef", "cutpoints", "quantiles", "cutoffs", "cutquantiles",
                           "genp", "binfreq", "method", "replace", "dedup", "minmax"}});

    // gstats is NOT an alias: it is a multiplex over two different native
    // handlers plus a set of refused sub-commands, and it keeps its own name all
    // the way to its own arm. It is in this table only so its options get a
    // vocabulary (defect class 3) and so the raw line is reassembled by the same
    // code path as every other row. by() is UNIMPLEMENTED rather than unknown:
    // runWinsor2 recognises it and refuses it, so "not yet implemented" is the
    // honest wording and "not allowed" is not.
    //
    // `label` is DROPPED, not refused. It only controls whether gstats attaches
    // a variable label to the winsorized output, which no number in the block
    // depends on -- and a refusal for a presentation-only key is the same size
    // of mistake as silently ignoring one that changes the answer, in the other
    // direction. The drop is alias-wide rather than winsor-only, so `gstats sum
    // ..., label` is accepted-and-dropped where gtools would reject it; that
    // widening is deliberate and is the cheaper half of the trade.
    t.emplace("gstats", AliasPolicy{"gstats",
                                    {},
                                    {},
                                    {"cuts", "suffix", "trim", "replace", "gen", "detail"},
                                    {"by"},
                                    {"label"},
                                    {},
                                    {}});
    return t;
  }();
  return kTable;
}

const AliasPolicy* findPolicy(const std::string& name) {
  const auto& t = aliasPolicies();
  auto it = t.find(name);
  return it == t.end() ? nullptr : &it->second;
}

// Defect class 3, closed for all ten names.
//
// Without a registered vocabulary under the ALIAS token, dispatcher.cpp's
// canonicalizeForCommand (which runs on `cmd` BEFORE this file's translation
// step) has nothing to resolve against: `gquantiles x, _pctile nq(4)` reaches
// the walk below with the key still spelled `nq`, `gstats winsor x, cut(1 99)`
// with `cut`, and the canonical-key comparisons never match. Meanwhile
// rejectUnknownOptions downstream still ACCEPTS the prefix. That combination is
// the shape that shipped `ivreg, ro` running without robust standard errors.
//
// known = native pass-through + what gtools adds + what we drop + what we
// rename FROM, minus anything refused; unimplemented = the native command's own
// unimplemented set + the refused keys. The refused keys have to be resolvable
// too, or `gcollapse, labelfor(...)` would slip past the refusal by being an
// abbreviation of a name the vocabulary never heard of.
bool registerGtoolsVocabs() {
  for (const auto& kv : aliasPolicies()) {
    const AliasPolicy& p = kv.second;
    std::unordered_set<std::string> refused(p.refuse.begin(), p.refuse.end());
    eval::OptVocab v;
    for (const auto& k : p.nativeKnown) {
      if (refused.count(k) == 0) v.known.insert(k);
    }
    for (const auto& k : p.extraKnown) {
      if (refused.count(k) == 0) v.known.insert(k);
    }
    for (const auto& k : p.drop) v.known.insert(k);
    for (const auto& r : p.rename) v.known.insert(r.first);
    for (const auto& k : p.nativeUnimplemented) v.unimplemented.insert(k);
    for (const auto& k : p.extraUnimplemented) v.unimplemented.insert(k);
    for (const auto& k : p.refuse) v.unimplemented.insert(k);
    // registerOptVocab takes an initializer_list<const char*>, so the token has
    // to outlive the call; kv.first is a key in the function-local static map
    // above and does.
    eval::registerOptVocab({kv.first.c_str()}, std::move(v));
  }
  return true;
}

// Static init, deliberately: dispatcher.cpp canonicalizes options at line ~1841
// and only reaches isGtoolsAlias at ~1888, so a registration deferred to first
// use would miss the first gtools command of the session.
const bool kGtoolsVocabsRegistered = registerGtoolsVocabs();

// Refusals for a translated option. The token is `alias, key()` so a reader
// sees WHICH option on WHICH command was refused, and the reason field carries
// `refused:<alias>_<key>` so the registry drift guard can tell a refusal apart
// from an unrecognised token.
//
// eval::unsupportedCommand emits code "unsupported_command", which is the ONE
// code eval::coarseRc answers 199 for. Building this with the default
// "engine_error" would silently downgrade `capture gcollapse ..., merge(x)` to
// _rc 1, and a do-file branching on 199 would take the wrong branch.
std::string refusalRecipe(const std::string& alias, const std::string& key) {
  if ((alias == "gcollapse" || alias == "fcollapse") && key == "merge") {
    return " `merge` collapses and then merges the result back onto the "
           "uncollapsed data, so the row count is unchanged -- that is not a collapse. Write it "
           "out: `preserve`, then the `collapse`, then `save` to a tempfile, then `restore`, then "
           "`merge m:1 <by-vars> using <tempfile>`.";
  }
  return std::string();
}

ojson refuseOption(const std::string& alias, const std::string& key) {
  ojson block = ojson(eval::unsupportedCommand(alias + ", " + key + "()", std::nullopt,
                                                std::nullopt, "refused:" + alias + "_" + key,
                                                eval::UnsupportedCommandKind::kNotImplemented));
  const std::string recipe = refusalRecipe(alias, key);
  // Appended to the message the helper already built rather than passed as a
  // new field: `message` is the one string every renderer shows, and a key the
  // block schema does not carry would be dropped on the floor.
  if (!recipe.empty() && block.contains("message") && block["message"].is_string()) {
    block["message"] = block["message"].get<std::string>() + recipe;
  }
  return block;
}

// ---------------------------------------------------------------------------
// gquantiles mode word.
// ---------------------------------------------------------------------------

// Stata's xtile defaults to nquantiles(2); the engine's --xtile flag defaults
// to 4. That is BUG-370, and it is why the xtile rewrite below ALWAYS emits an
// explicit nquantiles() instead of forwarding the user's option list as typed:
// a missing nq() would be answered by the engine's 4 and reported as a success.
// Same constant, same reason, as handlers_data_verbs3.cpp's kXtileDefaultNq.
constexpr long long kGquantilesDefaultNq = 2;

// gtools' _pctile with no options returns the median, i.e. the nquantiles(2)
// split, matching Stata's own `_pctile`. Same number as above, different
// command, so it is named separately rather than shared by coincidence.
constexpr long long kPctileDefaultNq = 2;

bool isModeWord(const std::string& lowered) {
  return lowered == "xtile" || lowered == "_pctile" || lowered == "pctile";
}

struct ModeWord {
  std::string mode;      // "" when absent
  bool fromVarlist = false;
  size_t varlistIndex = 0;
  bool fromOpts = false;
  size_t optsIndex = 0;
  bool conflict = false;  // two DIFFERENT mode words given
};

// The mode word can arrive in either half of the line, and both spellings are
// real gtools usage, so both are scanned. In `gquantiles x, _pctile nq(4)` the
// tokenizer splits at the comma and the mode lands in parsed.opts as a bare
// flag; a caller who writes it before the comma leaves it in parsed.varlist.
// Reading only one of the two would leave the other silently unrecognised, and
// "unrecognised mode" here means the wrong one of three different commands.
ModeWord readModeWord(const eval::ParsedCommand& parsed) {
  ModeWord out;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    const std::string low = strutil::lower(parsed.varlist[i]);
    if (!isModeWord(low)) continue;
    out.mode = low;
    out.fromVarlist = true;
    out.varlistIndex = i;
    break;
  }
  for (size_t i = 0; i < parsed.opts.size(); ++i) {
    const std::string low = strutil::lower(parsed.opts[i].first);
    if (!parsed.opts[i].second.isBool || !isModeWord(low)) continue;
    if (!out.mode.empty() && out.mode != low) out.conflict = true;
    if (out.mode.empty()) out.mode = low;
    out.fromOpts = true;
    out.optsIndex = i;
    break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Percentile rule, and the scratch/setcol sequence the pctile form writes with.
// ---------------------------------------------------------------------------

// TRANSCRIBED FROM src/data/frame.cpp::stata_pctile, term for term.
//
// This is not a convenience copy. PIET and the engine both answer percentile
// questions -- egen median, summarize,detail and egen pctile go through the
// engine's copy, `gquantiles ..., pctile` goes through this one -- and if the
// two rules differ then the same dataset has two different medians depending on
// which command asked. The averaging branch is the whole rule: when N*p/100
// lands exactly on an integer rank, Stata averages that observation with the
// next one instead of taking either.
double stataPctile(const std::vector<double>& sorted, double p) {
  const int64_t N = static_cast<int64_t>(sorted.size());
  if (N == 0) return std::numeric_limits<double>::quiet_NaN();
  if (N == 1) return sorted[0];
  const double Pi = static_cast<double>(N) * p / 100.0;
  const double fl = std::floor(Pi);
  if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < N) {
    return (sorted[static_cast<size_t>(static_cast<int64_t>(Pi) - 1)] +
            sorted[static_cast<size_t>(static_cast<int64_t>(Pi))]) /
           2.0;
  }
  int64_t idx = static_cast<int64_t>(fl) + 1;
  if (idx > N) idx = N;
  if (idx < 1) idx = 1;
  return sorted[static_cast<size_t>(idx - 1)];
}

// mintScratchPath always names a ".hbin"; the setcol payload is a CSV.
// Registering the minted .hbin name and then writing a .csv beside it would
// leave the file that actually exists unregistered, which is the BUG-389 orphan
// class the mint registry exists to close (sweepOrphanScratch only removes
// paths it knows). Same helper, same reason, as handlers_aer_abbrev.cpp.
std::string mintCsvPath(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  return csv;
}

// The headerless single-column payload `cobra setcol` reads
// (Frame::read_csv_column -- one raw value per row, row count == n_rows). An
// EMPTY line is how a missing value is spelled in that format; writing "nan" or
// "." instead would land a literal token in a numeric column.
bool writeValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// cobra reports a structured failure as {"error":{"code","message"}} on stderr.
// Surfacing the raw stderr blob instead would put JSON punctuation in front of
// the user; surfacing nothing would make a failed setcol indistinguishable from
// a successful one at the call site.
std::string cobraFailure(const CobraRunResult& r) {
  std::string text = r.stderr_text;
  if (!text.empty()) {
    try {
      const nlohmann::json parsed = nlohmann::json::parse(text);
      if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object() &&
          parsed["error"].contains("message")) {
        text = parsed["error"]["message"].get<std::string>();
      }
    } catch (const std::exception&) {
      // Not structured: the raw text below is the best available account.
    }
  }
  if (text.empty()) text = r.error;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  if (text.empty()) text = "cobra setcol failed with exit code " + std::to_string(r.exit_code);
  return text;
}

// r(r1), r(r2), ... are POSITIONAL, and PIET's r() store is never cleared
// between commands (dispatcher.cpp clears it only for `return clear`). So a
// `gquantiles x, _pctile nq(5)` leaves r(r3)/r(r4) behind, and a following
// nq(3) call that writes only r(r1)/r(r2) would let those two stale numbers be
// read as this call's answer. Stata REPLACES r() on every r-class command; the
// sweep below is how that is spelled here. Only the rN family is removed --
// r(N) and anything else are rewritten or left alone on their own terms.
void clearPositionalRValues(SessionState& session) {
  std::vector<std::string> doomed;
  for (const auto& kv : session.eval.rValues.items()) {
    const std::string& k = kv.first;
    if (k.size() < 2 || k[0] != 'r') continue;
    bool allDigits = true;
    for (size_t i = 1; i < k.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(k[i])) == 0) allDigits = false;
    }
    if (allDigits) doomed.push_back(k);
  }
  for (const auto& k : doomed) session.eval.rValues.erase(k);
}

// Publishing k scalars into an OrderedFloatDict costs O(k^2) -- its set() is a
// linear scan by design (eval/session.h). A typo'd nq(1000000) is therefore a
// hang wearing a computation's clothes, so the count is bounded and the bound
// is REPORTED rather than silently truncated.
constexpr size_t kMaxPercentileCount = 10000;

}  // namespace

// ---------------------------------------------------------------------------
// (1) Name folding.
// ---------------------------------------------------------------------------

bool isGtoolsAlias(const std::string& canonical) {
  // The reference is what keeps the registration from being elided as an unused
  // internal-linkage constant, and it records the ordering this file depends on:
  // the vocabularies must already be in the registry when dispatcher.cpp
  // canonicalizes options, which happens BEFORE it asks this question.
  (void)kGtoolsVocabsRegistered;
  // True for gquantiles and gstats as well, even though neither folds onto a
  // differently-named native command: both still need the translation step to
  // normalise their options and to reassemble parsed.raw.
  return findPolicy(canonical) != nullptr;
}

std::string gtoolsCanonicalName(const std::string& canonical) {
  const AliasPolicy* p = findPolicy(canonical);
  // gquantiles maps to itself here, NOT to xtile: which native command it is
  // depends on the mode word, and this fold is name-only by contract (the
  // by-prefix path calls it before any line has been parsed for options).
  if (p == nullptr) return canonical;
  if (canonical == "gquantiles" || canonical == "gstats") return canonical;
  return p->native;
}

// ---------------------------------------------------------------------------
// (2) Translation.
// ---------------------------------------------------------------------------

GtoolsTranslation translateGtoolsAlias(const eval::ParsedCommand& parsed,
                                       const std::string& canonical, SessionState& session) {
  // Unused by every row today. It stays in the signature because the dispatcher
  // hands every translation the same three arguments, and an alias whose
  // refusal depends on session state (a mode that is only legal with a panel
  // set, say) would need it without a call-site change.
  (void)session;

  GtoolsTranslation out;
  out.parsed = parsed;
  out.canonical = canonical;

  const AliasPolicy* pol = findPolicy(canonical);
  if (pol == nullptr) {
    // Unreachable through the dispatcher, which gates this call on
    // isGtoolsAlias. The identity is the only honest answer for a name this
    // file does not translate -- it is not a no-op standing in for a decision.
    return out;
  }

  const std::unordered_set<std::string> dropSet(pol->drop.begin(), pol->drop.end());
  const std::unordered_set<std::string> refuseSet(pol->refuse.begin(), pol->refuse.end());
  std::unordered_map<std::string, std::string> renameMap;
  for (const auto& r : pol->rename) renameMap.emplace(r.first, r.second);

  // The mode word decides which native command gquantiles becomes, so it is
  // read before the option walk (the walk has to know whether the mode flag is
  // an option to carry or one to strip).
  ModeWord mode;
  if (canonical == "gquantiles") {
    mode = readModeWord(parsed);
    if (mode.conflict) {
      out.error = gtErr(
          "gquantiles: `xtile`, `_pctile` and `pctile` are three different commands, not "
          "modifiers of one -- specify exactly one of them.",
          "syntax_error");
      return out;
    }
    if (mode.mode.empty()) {
      out.error = gtErr(
          "gquantiles: a mode is required -- write one of `xtile`, `_pctile` or `pctile` "
          "(syntax: `gquantiles [newvar =] varname, {xtile|_pctile|pctile} [nquantiles(#)]`).",
          "syntax_error");
      return out;
    }
  }

  // The mode flag is stripped from the option list ONLY for the xtile form: the
  // native xtile arm's reject net knows nothing about a bare `xtile` option and
  // would answer r(198) for it. The _pctile/pctile forms keep it, because it is
  // the only thing that tells runGQuantilesPctile which of the two it is.
  const bool stripMode = (canonical == "gquantiles" && mode.mode == "xtile");

  std::vector<std::pair<std::string, eval::OptValue>> translated;
  translated.reserve(parsed.opts.size());
  bool haveNquantiles = false;
  for (size_t i = 0; i < parsed.opts.size(); ++i) {
    const std::string& key = parsed.opts[i].first;
    if (stripMode && mode.fromOpts && i == mode.optsIndex) continue;
    if (refuseSet.count(key) != 0) {
      out.error = refuseOption(canonical, key);
      return out;
    }
    if (dropSet.count(key) != 0) continue;  // speed/presentation only
    auto ren = renameMap.find(key);
    const std::string outKey = (ren == renameMap.end()) ? key : ren->second;
    if (outKey == "nquantiles") haveNquantiles = true;
    translated.emplace_back(outKey, parsed.opts[i].second);
  }

  std::string native = pol->native;
  if (canonical == "gquantiles") {
    if (mode.mode == "xtile") {
      out.canonical = "xtile";
      native = "xtile";
      // BUG-370, stated as a rule rather than a fix: the handler owns the
      // default because the engine's differs from Stata's. Forwarding the
      // option list as typed answers nq(4) to a question Stata answers with 2,
      // reports success, and looks right.
      if (!haveNquantiles) {
        translated.emplace_back("nquantiles",
                                eval::OptValue::ofString(std::to_string(kGquantilesDefaultNq)));
      }
    } else {
      // _pctile / pctile have no native twin; the gquantiles arm in the
      // dispatcher routes them to runGQuantilesPctile below.
      out.canonical = "gquantiles";
      native = "gquantiles";
    }
  }

  // Rebuild the varlist, dropping the mode word when it was written before the
  // comma and this is the xtile form (the xtile arm reads joinVarlist and would
  // otherwise see "xtile new = x" and take "xtile new" for the new variable).
  std::vector<std::string> varlist = parsed.varlist;
  bool droppedFromVarlist = false;
  if (stripMode && mode.fromVarlist && mode.varlistIndex < varlist.size()) {
    varlist.erase(varlist.begin() + static_cast<std::ptrdiff_t>(mode.varlistIndex));
    droppedFromVarlist = true;
  }

  std::string head = headBeforeOpts(rawArgsAfterCmdLocal(parsed.raw));
  if (droppedFromVarlist) {
    // The head is raw text, so the token has to come out of it too or the
    // reassembled line would still read `xtile xtile new = x`. Only the FIRST
    // exact match is removed, and only for the form that strips it.
    std::vector<std::string> words = splitWords(head);
    for (size_t i = 0; i < words.size(); ++i) {
      if (strutil::lower(words[i]) != mode.mode) continue;
      words.erase(words.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    head.clear();
    for (size_t i = 0; i < words.size(); ++i) {
      if (i != 0) head += " ";
      head += words[i];
    }
  }

  std::string raw = native;
  if (!head.empty()) raw += " " + head;
  const std::string optTail = serialiseOpts(translated);
  if (!optTail.empty()) raw += ", " + optTail;

  // Both halves, or neither. dispatcher.cpp routes on the returned `canonical`
  // and the generic fallthrough REPORTS parsed.cmd, so setting only parsed.cmd
  // produced the most confusing possible symptom: an error saying `collapse` is
  // not supported, raised because the router was still looking for `gcollapse`.
  // The two fields answer different questions and have to move together.
  out.canonical = native;
  out.parsed.cmd = native;
  out.parsed.varlist = std::move(varlist);
  out.parsed.opts = std::move(translated);
  out.parsed.raw = std::move(raw);
  return out;
}

// ---------------------------------------------------------------------------
// (3) gquantiles _pctile / pctile.
// ---------------------------------------------------------------------------

ojson runGQuantilesPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  const eval::OptVocab* vocab = eval::findOptVocab("gquantiles");
  if (vocab != nullptr) {
    // The refused keys were already answered upstream in translateGtoolsAlias,
    // so this net is here for the keys nobody named: without it an unknown
    // option is accepted and never read, which is the silent-ignore shape.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, vocab->known, vocab->unimplemented)) {
      return ojson(*err);
    }
  }

  const ModeWord mode = readModeWord(parsed);
  if (mode.mode != "_pctile" && mode.mode != "pctile") {
    // The dispatcher only routes here after the translation step set canonical
    // to "gquantiles", which happens for exactly these two modes.
    return gtErr("gquantiles: expected the `_pctile` or `pctile` form here", "syntax_error");
  }
  const bool wantVariable = (mode.mode == "pctile");

  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    // Same refusal, for the same reason, as runXtile's: Stata computes the
    // cutpoints from the RESTRICTED sample. Computing them over every row and
    // then reporting them would be the right shape carrying the wrong numbers.
    return ojson(eval::unsupportedCommand(
        "gquantiles with `if` (percentiles must come from the restricted sample; restrict first "
        "with `preserve` + `keep if ...`, then run gquantiles)",
        std::nullopt, std::nullopt, "refused:gquantiles_if",
        eval::UnsupportedCommandKind::kNotImplemented));
  }

  // `[newvar =] source`, with the mode word removed. The tokenizer splits the
  // pre-comma head on whitespace, so `new = x` arrives as three tokens and
  // `new=x` as one -- rejoining and splitting on '=' handles both.
  std::string joined;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    if (mode.fromVarlist && i == mode.varlistIndex) continue;
    if (!joined.empty()) joined += " ";
    joined += parsed.varlist[i];
  }
  std::string target;
  std::string source;
  const size_t eq = joined.find('=');
  if (eq == std::string::npos) {
    source = strutil::trim(joined);
  } else {
    target = strutil::trim(joined.substr(0, eq));
    source = strutil::trim(joined.substr(eq + 1));
  }
  if (source.empty()) {
    return gtErr("gquantiles: no source variable specified", "syntax_error");
  }
  if (wantVariable && target.empty()) {
    return gtErr("gquantiles ... , pctile requires a new variable: `gquantiles newvar = varname, "
                 "pctile nquantiles(#)`",
                 "syntax_error");
  }
  if (!wantVariable && !target.empty()) {
    // `_pctile` returns r() only. Accepting a target and never creating it
    // would report success for a variable that does not exist afterwards.
    return gtErr("gquantiles ... , _pctile does not create a variable (it returns r(r1), r(r2), "
                 "...); use `pctile` if you want one",
                 "syntax_error");
  }

  // Percentile list: percentiles() as typed, else the nquantiles() split.
  std::vector<double> pcts;
  std::string pctSpec;
  bool havePctSpec = false;
  bool haveNq = false;
  long long nq = kPctileDefaultNq;
  for (const auto& kv : parsed.opts) {
    if (kv.first == "percentiles") {
      if (kv.second.isBool) {
        return gtErr("gquantiles: percentiles() requires a list of numbers", "syntax_error");
      }
      pctSpec = kv.second.strValue;
      havePctSpec = true;
    } else if (kv.first == "nquantiles") {
      if (kv.second.isBool || !parseIntStrict(strutil::trim(kv.second.strValue), &nq)) {
        return gtErr("gquantiles: nquantiles() requires a whole number", "syntax_error");
      }
      haveNq = true;
    }
  }
  if (havePctSpec && haveNq) {
    // The two are ALTERNATIVE sources of the same list, and percentiles() wins
    // below. Letting it win silently would mean `nq(10)` produced a 2-way split
    // whenever a percentiles() sat beside it -- an ignored option that changes
    // the answer, which is the shape this bundle exists to refuse.
    return gtErr("gquantiles: percentiles() and nquantiles() are alternatives -- percentiles() "
                 "lists the cut percentages directly, nquantiles(#) asks for an even split into "
                 "# groups. Give one of them.");
  }
  if (havePctSpec) {
    // `percentiles(10, 50)` and `percentiles(10 50)` are both real spellings,
    // and the option value arrives with its interior comma intact because the
    // tokenizer's split is paren-aware.
    std::string normalised = pctSpec;
    std::replace(normalised.begin(), normalised.end(), ',', ' ');
    for (const auto& w : splitWords(normalised)) {
      double p = 0.0;
      if (!parseDoubleStrict(w, &p)) {
        return gtErr("gquantiles: percentiles(" + pctSpec + ") -- `" + w + "` is not a number",
                     "syntax_error");
      }
      if (!(p > 0.0 && p < 100.0)) {
        return gtErr("gquantiles: percentiles(" + pctSpec +
                         ") -- every percentile must satisfy 0 < p < 100");
      }
      pcts.push_back(p);
    }
    if (pcts.empty()) {
      return gtErr("gquantiles: percentiles() is empty", "syntax_error");
    }
  } else {
    if (nq < 2) {
      return gtErr("gquantiles: nquantiles() must be at least 2", "syntax_error");
    }
    if (static_cast<unsigned long long>(nq - 1) > kMaxPercentileCount) {
      return gtErr("gquantiles: nquantiles(" + std::to_string(nq) + ") asks for " +
                       std::to_string(nq - 1) + " percentiles; this build publishes at most " +
                       std::to_string(kMaxPercentileCount) + ".");
    }
    for (long long i = 1; i < nq; ++i) {
      pcts.push_back(100.0 * static_cast<double>(i) / static_cast<double>(nq));
    }
  }
  if (pcts.size() > kMaxPercentileCount) {
    return gtErr("gquantiles: " + std::to_string(pcts.size()) +
                     " percentiles requested; this build publishes at most " +
                     std::to_string(kMaxPercentileCount) + ".");
  }

  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  // contract_A section 38 projection: this handler reads ONE column, and on a
  // dirty virtual head opening the whole datastore would materialize the entire
  // frame to get at it. `target` rides in as an EXPRESSION rather than via
  // alsoInclude because referencedColumns only reports identifiers the shadow
  // card CONFIRMS -- so "the projection came back carrying target" is exactly
  // the statement "that variable already exists", which is the check this
  // handler owes before it writes. When the card cannot be trusted the
  // projection is nullopt and both questions fall back to the real datastore,
  // where they are direct. Same shape as runLevelsof and runWinsorCox.
  const std::string* targetExpr = target.empty() ? nullptr : &target;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  if (auto cols = referencedColumns(session, {targetExpr}, source)) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  if (mini && !mini->has(source)) mini.reset();  // subject absent -> not usable
  Datastore* ds = mini ? mini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  if (!ds->has(source)) {
    // "variable ... not found" is the wording eval::coarseRc maps to Stata's
    // r(111); rephrasing it would silently downgrade the rc to the generic 1.
    return gtErr("gquantiles: variable " + source + " not found");
  }
  if (ds->dtype(source) != "f64") {
    return gtErr("gquantiles: " + source +
                 " is a string variable; gquantiles needs a numeric variable");
  }
  if (wantVariable) {
    if (!strutil::isValidStataIdentifier(target)) {
      return gtErr("gquantiles: invalid variable name `" + target + "' -- " +
                       strutil::stataIdentifierRuleText(),
                   "syntax_error");
    }
    if (ds->has(target)) {
      // "already defined" is the wording coarseRc maps to r(110) (confirm new
      // variable). Same reason as above: the phrase is load-bearing.
      return gtErr("gquantiles: variable " + target + " already defined");
    }
  }

  const cecon::Column& column = ds->col(source);
  const double* data = column.f64();
  // n_values(), NOT nbytes/sizeof(double). `nbytes` is the payload width, and a
  // narrow (Narrow::F32) column carries FOUR bytes per observation while f64()
  // widens it into a cache of n_values() doubles -- so the byte division reads
  // half the row count and the guard below then rejects a perfectly good column
  // as "not full-length". io/columnar.h pairs f64() with n_values() for exactly
  // this reason; every other reader in the tree (winsor2's clampBoundaries) uses
  // the row count rather than deriving one from bytes.
  const int64_t count = column.n_values();
  if (data == nullptr || count != ds->nRows()) {
    return gtErr("gquantiles: could not read " + source + " as a full-length numeric column");
  }

  std::vector<double> sorted;
  sorted.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    if (!std::isnan(data[i])) sorted.push_back(data[i]);
  }
  if (sorted.empty()) {
    return gtErr("gquantiles: " + source + " has no non-missing observations");
  }
  std::sort(sorted.begin(), sorted.end());

  std::vector<double> values;
  values.reserve(pcts.size());
  for (double p : pcts) values.push_back(stataPctile(sorted, p));

  // Both forms are r-class, so both must REPLACE the store rather than add to
  // it. PIET never clears r() between commands (only `return clear` does), so
  // clearing on one arm only meant `gquantiles y, _pctile nq(5)` followed by
  // `gquantiles q = y, pctile nq(3)` left r(r4) readable and stale -- published
  // by the FIRST command, read as if it belonged to the second. Stata replaces
  // r() on every r-class command, and pctile is one.
  clearPositionalRValues(session);

  if (!wantVariable) {
    for (size_t i = 0; i < values.size(); ++i) {
      session.eval.rValues.set("r" + std::to_string(i + 1), values[i]);
    }
    // r(N) is the count the percentiles were computed FROM, i.e. the
    // non-missing observations, not the row count and not the number of
    // percentiles. gtools reports the same quantity.
    session.eval.rValues.set("N", static_cast<double>(sorted.size()));
    // Silent success. An EMPTY array, never a one-element one: the dispatcher
    // arm pushes whatever non-empty value comes back as a single block, and a
    // consumer reading "type" off an array aborts with nlohmann type_error.306.
    return ojson::array();
  }

  const int64_t nRows = ds->nRows();
  if (static_cast<int64_t>(values.size()) > nRows) {
    return gtErr("gquantiles: " + std::to_string(values.size()) +
                 " percentiles cannot be stored in a dataset of " + std::to_string(nRows) +
                 " observations");
  }
  std::vector<double> outCol(static_cast<size_t>(nRows),
                             std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < values.size(); ++i) outCol[i] = values[i];

  const std::string csvPath = mintCsvPath(session.workdir(), "gquantiles_pctile_vals");
  if (!writeValuesCsv(csvPath, outCol)) {
    return gtErr("gquantiles: could not write the percentile values");
  }

  // The new column is patched into the REAL head, not into the projection
  // above: the projection carries only the columns this handler fetched, so
  // writing to it would silently drop every other variable in the dataset.
  const std::string outPath = mintScratchPath(session.workdir(), "gquantiles_pctile");
  const CobraRunResult r =
      runCobra({"setcol", "--data", *session.currentHbin(), "--var", target, "--values", csvPath,
                "--dtype", "f64", "--new", "--out", outPath});
  if (!r.ok) return gtErr("gquantiles: " + cobraFailure(r));
  // BUG-124's torn-write guard, carried from the other stateless setcol sites:
  // the engine's writer creates `--out` before it can detect a write failure,
  // and a full scratch volume has twice reached PIET as a ZERO exit code.
  // session.load on a path that is not there does not fail -- it records the
  // head and hands the next Datastore open a missing or truncated mapping,
  // which is a crash rather than a diagnostic. This handler never opens a
  // ResidentEligible scope, so `--out` is always a real file here.
  if (!cecon::native_exists(outPath)) {
    return gtErr("gquantiles: failed writing output dataset (disk full or I/O error)");
  }

  session.load(outPath);
  // "gen", not "gquantiles": the mutation IS a pure add of one new column,
  // which is the branch seedNMissForPureAdd must take for the untouched columns
  // to carry their missing-counts forward.
  return mutationResult(session, "gen", {target});
}

// ---------------------------------------------------------------------------
// (4) gstats multiplex.
// ---------------------------------------------------------------------------

namespace {

ojson gstatsRefusal(const std::string& sub, eval::UnsupportedCommandKind kind) {
  return ojson(eval::unsupportedCommand("gstats " + sub, std::nullopt, std::nullopt,
                                         "refused:gstats_" + sub, kind));
}

// Winsorizing at the 1st and 99th percentile is winsor2's documented default
// and gstats winsor's. It is passed to runWinsor2 EXPLICITLY on every call --
// this handler owns the default, because "whatever the callee does when the
// argument is absent" is how an engine default silently replaces a Stata one
// (the xtile nq 2-vs-4 class, BUG-370).
constexpr double kGstatsWinsorCutLo = 1.0;
constexpr double kGstatsWinsorCutHi = 99.0;
const char* const kGstatsWinsorSuffix = "_w";

}  // namespace

ojson runGstats(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) {
    return gtErr("gstats: a sub-command is required (`winsor` or `sum`)", "syntax_error");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);
  const std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());

  if (sub == "winsor") {
    // Per-sub reject nets, not one union net over both sub-commands: `gstats
    // sum x, cuts(1 99)` has to be an error, and a union net would accept it
    // and then never read it.
    if (auto err = eval::rejectUnknownOptions(parsed.opts,
                                              {"cuts", "suffix", "trim", "replace", "gen"},
                                              {"by"})) {
      return ojson(*err);
    }
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      // runWinsor2 has no `if` channel at all, so forwarding the call would
      // winsorize every row at cutpoints drawn from every row while the user
      // asked for a subsample. Refused rather than answered.
      return ojson(eval::unsupportedCommand(
          "gstats winsor with `if` (the cutpoints would come from the full sample)",
          std::nullopt, std::nullopt, "refused:gstats_winsor_if",
          eval::UnsupportedCommandKind::kNotImplemented));
    }
    if (vars.empty()) return gtErr("gstats winsor: no variables specified", "syntax_error");

    double cutLo = kGstatsWinsorCutLo;
    double cutHi = kGstatsWinsorCutHi;
    std::string suffix = kGstatsWinsorSuffix;
    bool trim = false;
    bool replace = false;
    std::vector<std::string> genNames;
    for (const auto& kv : parsed.opts) {
      const std::string& k = kv.first;
      const eval::OptValue& v = kv.second;
      if (k == "cuts") {
        if (v.isBool) return gtErr("gstats winsor: cuts() requires two numbers", "syntax_error");
        const std::vector<std::string> parts = splitWords(v.strValue);
        if (parts.size() != 2) {
          return gtErr("gstats winsor: cuts() requires exactly two values, got '" + v.strValue +
                       "'");
        }
        // Strict consume both, mirroring BUG-112(4) on the winsor2 arm:
        // `cuts(5x 95)` must reject, not read "5x" as 5.
        if (!parseDoubleStrict(parts[0], &cutLo) || !parseDoubleStrict(parts[1], &cutHi)) {
          return gtErr("gstats winsor: cuts(" + v.strValue + ") -- expected two numbers", "syntax_error");
        }
      } else if (k == "suffix") {
        if (v.isBool || v.strValue.empty()) {
          return gtErr("gstats winsor: suffix() requires a suffix string", "syntax_error");
        }
        suffix = v.strValue;
      } else if (k == "trim") {
        trim = true;
      } else if (k == "replace") {
        replace = true;
      } else if (k == "gen") {
        if (v.isBool) {
          return gtErr("gstats winsor: gen() requires one new variable name per winsorized "
                       "variable",
                       "syntax_error");
        }
        genNames = splitWords(v.strValue);
      }
    }
    if (!genNames.empty() && replace) {
      // replace writes back over the source; gen() names a new column. Doing
      // both is two different answers to "where does the result go".
      return gtErr("gstats winsor: gen() and replace are alternatives -- replace writes the "
                   "winsorized values back over the source variables, gen() puts them in new "
                   "ones.");
    }
    if (!genNames.empty() && genNames.size() != vars.size()) {
      return gtErr("gstats winsor: gen() names " + std::to_string(genNames.size()) +
                       " variable(s) for " + std::to_string(vars.size()) +
                       " winsorized variable(s); the two lists must line up one for one.");
    }

    ojson result = runWinsor2(session, vars, cutLo, cutHi, trim, /*by=*/std::string(), suffix,
                              replace);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    if (genNames.empty()) return result;

    // gen() is spelled as a rename of the suffixed results because runWinsor2
    // names its outputs `src + suffix` and has no output-name channel. Doing it
    // here rather than teaching winsor2 a new parameter keeps the shim's reach
    // inside this file.
    for (size_t i = 0; i < genNames.size(); ++i) {
      ojson renamed = runRename(vars[i] + suffix, genNames[i], session);
      if (renamed.is_object() && renamed.value("type", std::string()) == "error") {
        // Reported, not swallowed: the earlier renames already landed, so the
        // dataset is half-renamed and the user has to be told which one failed.
        return renamed;
      }
      // The LAST block is what the caller gets: each mutation block carries
      // varlist_after, and only the last one describes the dataset that exists
      // when this handler returns.
      result = std::move(renamed);
    }
    return result;
  }

  if (sub == "sum" || sub == "summarize") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"detail"})) {
      return ojson(*err);
    }
    bool detail = false;
    for (const auto& kv : parsed.opts) {
      if (kv.first == "detail") detail = true;
    }
    std::optional<std::vector<std::string>> varsOpt;
    if (!vars.empty()) varsOpt = vars;
    // ifExpr is forwarded, not dropped: `gstats sum x if y > 0` is a legal line
    // (the registry row allows `if`), and summarizing every row instead would
    // be a wrong number reported as a right one. runSummarize has the channel.
    return runSummarize(varsOpt, detail, session, parsed.ifExpr);
  }

  // Known gtools sub-commands with no native twin here. Named individually so
  // the reason field distinguishes them and so a later wave can find them.
  static const std::unordered_set<std::string> kKnownSubs = {"tab", "transform", "hdfe", "range",
                                                             "moving"};
  if (kKnownSubs.count(sub) != 0) {
    return gstatsRefusal(sub, eval::UnsupportedCommandKind::kNotImplemented);
  }
  // Not a gstats sub-command at all. kUnknownSubcommand says "the command is
  // implemented, this word is not one of its sub-commands", which is a
  // different sentence from "PIET has not built this yet" and the only one that
  // points at a typo.
  return gstatsRefusal(sub, eval::UnsupportedCommandKind::kUnknownSubcommand);
}

}  // namespace piet::handlers
