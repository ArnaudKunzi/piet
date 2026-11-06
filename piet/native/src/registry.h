// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Phase A0 (BETA campaign) — the command registry: a single hand-transcribed
// table describing TODAY's dispatcher.cpp routing behavior, one row per
// canonical Stata command PIET actually routes. NOT a self-registration
// mechanism (dispatcher.cpp's ~75 `if (cmd == "...")` arms are anonymous
// if-bodies, nothing to attach a REGISTER macro to) and NOT a refactor of
// the dispatcher — see dispatcher.cpp:809-813's own "TODO(M3) EXTENSION
// POINT" comment. This table is a read-only DESCRIPTION of that if-chain,
// built by hand against dispatcher.cpp:809-2203 (the routing table),
// eval/tokenizer.cpp:507-519/529-539 (the weight/`in` allow-lists), and
// dispatcher.cpp:596-678 (the by/bysort gate) — see
// internal note A0-anchor-map.md for the full
// scouting trail this transcription is built from.
//
// Consumers:
//   - `piet_native --dump-registry` (this file's runDumpRegistry(), wired in
//     main.cpp's flag loop) dumps every row as JSON to stdout.
//   - tools/freeze_matrix.py reads that JSON to enumerate one frozen
//     invocation per registered command against internal script debug_all.do.
//   - selftest.cpp's testRegistryRoutes() is the drift guard: for every
//     canonical/abbrev spelling here, dispatch a minimal probe and assert
//     dispatcher.cpp never silently answers with the generic
//     unsupported_command("piet_gap") fallthrough (dispatcher.cpp:2201-2203)
//     — i.e. this table and the real if-chain must never drift apart.
//   - A3 (BETA campaign, BS-096): this table is no longer JUST a read-only
//     description — eval/tokenizer.cpp's weight/`in` qualifier gates and
//     dispatcher.cpp's by:/bysort: reject-message branch now CONSULT
//     findByName() at runtime instead of maintaining their own second copy
//     of the allow-lists this table was originally transcribed from. The
//     drift guard above still matters (arguably more): a row drifting from
//     the real dispatcher behavior now changes enforcement, not just
//     documentation.
//
// Deliberately NOT here (out of scope for A0 — see the anchor map + the
// task brief that spawned this file):
//   - `bysort`/`by` themselves are NOT rows: they are a PREFIX mechanism
//     (dispatcher.cpp:596-678), not a standalone command with one behavior.
//     Their gating is instead exposed per-row via allowByPrefix (true for
//     generate/gen/g, replace, egen, and — BS-092, BETA Phase G — summarize;
//     the only sub-commands the by-gate structurally supports; every other
//     sub-command is explicitly rejected, never silently flattened).
//   - Fixed-arity / trailing-token validation (dispatcher.cpp's
//     kFixedArityCommands, ~787-806) has no field here — it is a real
//     dispatcher behavior this table does not attempt to model.
#ifndef PIET_NATIVE_REGISTRY_H
#define PIET_NATIVE_REGISTRY_H

#include <string>
#include <vector>

namespace piet::registry {

// Broad grouping used for the freeze/consistency-spine reporting surface.
// Six buckets only (per the A0 anchor map's recommended shape) — several
// assignments here are judgment calls on genuinely cross-cutting commands
// (e.g. tabstat/corr/pwcorr are classed Estimation, following
// dispatcher.cpp's OWN "M4a item 2 — estimation surface" section comment,
// even though they are descriptive rather than model-fitting); see the A0
// task's return summary for the full list of calls flagged for operator
// review.
enum class Family {
  DataMgmt,    // shapes/transforms/describes the loaded dataset's contents
  Estimation,  // model fitting + post-estimation (regress..estat, eststo..esttab)
  Graph,       // visual output (scatter..twoway, graph, coefplot, marginsplot)
  IO,          // filesystem/OS/session-external I/O (use, log, postfile, ...)
  Macro,       // named computational objects outside the dataset (matrix, scalar)
  Meta,        // session bookkeeping / metadata / no-ops (describe, label, version)
};

// Estimation-weight qualifier policy, mirroring eval/tokenizer.cpp's weight
// gate EXACTLY: each row names the ONE allow-set the engine actually accepts
// for that command; the gate rejects any spelling outside that row's set,
// naming the command AND (for a non-None row) the allowed set in its reject
// message. This is a per-command field now (P3 weights wave, an
// engine work item) — the pre-P3 law ("fw/fweight only, a narrow allow-list;
// aw/pw/iw rejected everywhere, no exceptions") is retired; None is still a
// real, common case (most commands), just no longer the ONLY non-FweightOnly
// case.
enum class WeightPolicy {
  None,           // any [aw=]/[fw=]/[pw=]/[iw=] is rejected upstream in the tokenizer
  FweightOnly,    // [fw=]/[fweight=] only (tabulate/tab1/tab2, collapse)
  AweightFweight, // [aw=]/[aweight=] or [fw=]/[fweight=] only (summarize)
  PweightOnly,    // [pw=]/[pweight=] only (mean, proportion)
  AweightPweight, // [aw=]/[aweight=] or [pw=]/[pweight=] only (ivreg2)
  AllWeights,     // [aw=]/[fw=]/[pw=]/[iw=] all forwarded (regress/reg/r/feols/reghdfe)
};

std::string familyToString(Family f);
std::string weightPolicyToString(WeightPolicy w);

// One row = one canonical Stata command name PIET's dispatcher recognizes.
// Genuine Stata abbreviations of the SAME command (e.g. `reg`/`r` for
// `regress`, `sum` for `summarize`) live in `abbrev`. Where ONE dispatcher.cpp
// if-arm structurally serves several DISTINCT command names (e.g. `logit`/
// `probit`, `csdid`/`drdid`, `eststo`/`estimates`) each distinct name gets
// its OWN row (same handlerTag) rather than being folded into `abbrev` —
// see the A0 return summary for the full list of these arm-sharing splits.
struct CommandSpec {
  std::string canonical;
  std::vector<std::string> abbrev;
  Family family;
  bool allowIf;          // true unless the arm structurally never reads/forwards parsed.ifExpr
  bool allowInRange;     // true iff PIET has a real `in a/b` row-filter implementation for this
                          // command (BS-090, BETA Phase G: this table IS the tokenizer's `in`
                          // allow-list — eval/tokenizer.cpp:560-570 reads it directly, no separate
                          // hardcoded list exists — see registry.cpp's field-derivation comment)
  WeightPolicy weight;    // per-command weight allow-set (P3 weights wave); see WeightPolicy's own
                          // doc comment above for what each value means and which rows carry it
  bool allowByPrefix;     // true iff by:/bysort: has a real per-group implementation for this command
  std::vector<std::string> knownOptions;  // sparse/aspirational — populated ONLY where the arm
                                           // already has an explicit unknown-option REJECTION net
                                           // (today: summarize/sum's detail/d/meanonly net). Empty
                                           // elsewhere is a growth field, not "no options exist".
  std::string handlerTag;  // the handlers::/eval:: function (or dispatcher multiplex) backing this row

  // A3 (BETA campaign, BS-096): true iff Stata ITSELF structurally forbids a
  // by:/bysort: prefix on this command — CONFIRMED live against StataBE for
  // exactly 4 rows (codebook, describe, levelsof, table: each answers "may
  // not be combined with by" / "prefix by: not allowed", r(190)). This is
  // DISTINCT from allowByPrefix (which means "PIET has a real per-group
  // implementation" — true for generate/gen/g, replace, egen, and — BS-092,
  // BETA Phase G — summarize): a command can have allowByPrefix=false for
  // two structurally different reasons — either it's one of these 4
  // Stata-illegal stems, or it's one of the ~7 Stata-LEGAL-but-PIET-
  // not-yet-implemented stems (correlate/pwcorr, ttest, tabstat,
  // tabulate/tab1/tab2, count, list — audit G3, queued at BS-107;
  // summarize graduated out of this list this phase, BS-092). Drives
  // dispatcher.cpp's by-gate
  // reject-message wording (BS-096/A3): the 4 illegal stems get an honest
  // "Stata disallows this, not a PIET limitation" message instead of the
  // misleading "not supported yet" framing (which wrongly implies a future
  // fix — audit G3 finding #10). Defaults false via the member initializer
  // below so the ~75 pre-A3 rows above needed NO edit — only the 4
  // confirmed-illegal rows set this explicitly to true.
  bool byIllegalInStata = false;


  // WS-B (extcompat, 2026-08-06): the spellings ON THIS ROW that were
  // introduced by an independent, community-authored Stata package rather
  // than by StataCorp or by us — `reghdfe` on the `feols` row, `csdid` on
  // its own row (a row whose CANONICAL is itself a community name lists
  // that name here too). Empty on every other row, which is why the ~75
  // untouched rows above needed no edit.
  //
  // Two consumers, both single-point:
  //   · dispatcher.cpp's ONE extcompat gate, right after name resolution —
  //     with the `extcompat` setting off, a line whose command token is one
  //     of these spellings is rejected with r(199) instead of running.
  //   · selftest's drift guard, which asserts this set is exactly the set of
  //     names in communityNameTable() below (so neither can go stale alone).
  //
  // NOT a notice surface. The non-affiliation notice belongs to the consent
  // dialog / Settings / NOTICE.txt / docs and NEVER to command output —
  // LEGAL-DISTRIBUTION §2.7(c), because the display surface is what the
  // answerbook gates byte-compare.
  std::vector<std::string> communityNames;

  // BUG-450 (AER scope 2, 2026-09-07): true iff this row's varlist is an
  // ORDINARY Stata varlist, so a token carrying `*`, `?`, a range hyphen or
  // `_all` is expanded against the dataset before the arm ever sees it.
  //
  // Why a flag and not "always". Several rows carry something varlist-SHAPED
  // that must not be expanded, and expanding it would answer a different
  // question at rc 0:
  //   · `rename t* T*` is Stata's rename-GROUPS syntax — measured, it renames
  //     trunk->Trunk and turn->Turn. Pre-expanding the pattern destroys the
  //     pairing the feature is made of.
  //   · estimation rows carry factor-variable syntax (`i.x`, `c.x##c.z`) and
  //     time-series operators (`L.x`) in the same field.
  //   · `egen y = f(a b)` puts its arguments inside a function call.
  // The default is therefore false, and a row opts IN only once its varlist has
  // been confirmed to be a plain one. Set by name in commandRegistry(), so the
  // ~298 row literals need no edit and the opt-in list reads as a list.
  bool varlistIsPlain = false;

  // BUG-1422: true iff Stata's own syntax for this command takes neither an `in`
  // range nor weights, and the arm itself refuses them as Stata does, in
  // Stata's order (varlist, if, in, weights): r(101) "in range not allowed" /
  // "weights not allowed". The tokenizer's two feature-gap gates (r(198) "does
  // not support the `in` row-range qualifier ...") stand aside for these rows,
  // which would otherwise answer a Stata refusal as a PIET gap. Set by name in
  // commandRegistry(), like varlistIsPlain.
  bool qualifiersRefusedByArm = false;
};

// WS-B (extcompat, 2026-08-06): the attribution record behind the gate
// message. `author` is transcribed VERBATIM from NOTICE.txt section 2 — that
// file is the single source of truth for who introduced each name, and no
// attribution may be invented here. `nativeEquivalent` is PIET's own command
// for the same job, and is filled ONLY where it is structurally provable
// (same registry row, or the same dispatcher handler): "" means this name has
// no native equivalent and the gate message drops its middle sentence rather
// than pointing at a command that does something else.
struct CommunityName {
  std::string name;
  std::string author;
  std::string nativeEquivalent;
};

const std::vector<CommunityName>& communityNameTable();

// Lookup by exact (already-lowercased) spelling; nullptr when the name is not
// a community-package compatibility name.
const CommunityName* findCommunityName(const std::string& lowerName);

// The pinned r(199) text dispatcher.cpp's extcompat gate emits. Lives here,
// next to the table it reads, so the gate itself stays a three-line branch.
std::string communityGateMessage(const CommunityName& cn);

// Single static transcription table — see registry.cpp. Population order
// mirrors dispatcher.cpp's own arm order (809 -> 2203) so the table can be
// diffed against the source by eye.
const std::vector<CommandSpec>& commandRegistry();

// A3 (BETA campaign, BS-096): case-sensitive lookup by canonical name OR any
// abbreviation spelling. Callers must lowercase `lowerName` first — every
// canonical/abbrev spelling in commandRegistry() is already lowercase,
// matching eval/tokenizer.cpp's own toLower(cmd) convention at every call
// site this backs. Returns nullptr when `lowerName` isn't a registered
// command — notably `bysort`/`by` themselves (deliberately NOT rows here,
// see the "Deliberately NOT here" comment above), so a lookup for the OUTER
// token of a `bysort V: CMD` line correctly misses regardless of how
// well-known CMD is. Backs the registry-driven qualifier gates in
// eval/tokenizer.cpp (weight/`in`, replacing the old hardcoded allow-lists)
// and the by:/bysort: reject-message branch in dispatcher.cpp.
const CommandSpec* findByName(const std::string& lowerName);

// BUG-105 (ONESHOT-PREP9 WS-K) — THE ONE NAME-RESOLUTION POINT.
//
// Maps any registered spelling to its row's canonical spelling; returns
// `lowerName` unchanged when the token is not a registered command (so a
// caller can keep comparing it against its own non-registry literals, e.g.
// dispatcher.cpp's by/bysort prefix words, which are deliberately NOT rows).
//
// Why this exists: the tokenizer's qualifier gates already resolved through
// findByName() while dispatcher.cpp's ~75 `cmd == "..."` arms compared
// hardcoded literals, so a spelling could be routable in one and unknown in
// the other — which is what forced the 2026-07-31 BUG-105 stopgap to edit BOTH
// files for each of its five spellings. Normalising ONCE at dispatcher.cpp's
// single `cmd` formation site (and its by/bysort sibling) makes every future
// abbreviation a one-line registry edit, and makes the out-of-registry
// spelling copies (kNoDataCommands, kFixedArityCommands, the by-prefix
// sub-command literals) see the canonical name automatically.
//
// The POLICY is unchanged and deliberately NOT a computed prefix rule: real
// Stata's accepted-spelling union is non-contiguous (`hist` runs, `histo` does
// not; `tab` runs, `tabl` does not; `d`=describe but `dr`=nothing), measured
// live against StataBE — see internal note BUG105-ABBREV-SOURCING-20260731.md §2. Only
// declared spellings resolve.
//
// ONE deliberate exemption: a spelling this row lists in `communityNames` is
// returned UNCHANGED. A community-package compatibility name is not an
// abbreviation of our canonical command — `reghdfe` sits in the `feols` row's
// abbrev list for routing, but it is behaviourally distinct downstream
// (handlers.cpp's runRegress reads `stataCmd == "reghdfe"` for the
// keepsingletons net and the estimator label), and dispatcher.cpp's extcompat
// gate must still see the typed community name. Folding it to `feols` here
// would silently kill both. `reghdfe` is the only spelling this exemption
// currently reaches (every other community name is its own canonical).
std::string resolveCanonical(const std::string& lowerName);

// ---------------------------------------------------------------------------
// PREFIX registry (FULL FUNCTIONAL CHECK, 2026-08-05).
//
// The command table above deliberately has no rows for PREFIXES — `by`/`bysort`
// were excluded from the start (see "Deliberately NOT here"), and the P7 close
// tried a `svy` row and removed it again because the route-drift guard
// correctly refuses one: a bare `svy` reaches no dispatcher arm, so it is not a
// command in this table's sense.
//
// The consequence was structural rather than cosmetic. The registry-coverage
// gate proves every ROW is exercised by the pangram; prefixes, being rowless,
// were invisible to it — an entire execution shape (the whole resampling family
// arrived that way in P6, `svy:` in P7) could ship without the gate noticing,
// with only whichever pangram section happened to pin it standing in for
// coverage.
//
// This parallel table closes that hole without pretending a prefix is a
// command. `probe` is a minimal line that must actually REACH the prefix's
// handler; selftest's prefix drift guard runs each one and asserts it does not
// fall through to unsupported_command, which is what keeps this list honest
// rather than a hand-kept registry that quietly goes stale — the same
// compensating-control shape the command table's own drift guard uses.
struct PrefixSpec {
  std::string name;        // the prefix keyword as typed
  std::string probe;       // minimal line exercising the PREFIX form
  std::string handlerTag;  // where the prefix is recognised
};

const std::vector<PrefixSpec>& prefixRegistry();

// `piet_native --dump-prefix-registry`: prefixRegistry() as a JSON array.
// A SEPARATE flag, not extra keys on --dump-registry, because that dump's
// array-of-command-rows shape is already parsed by tools/freeze_matrix.py and
// internal script coverage_check.py.
int runDumpPrefixRegistry();

// `piet_native --dump-registry`: prints commandRegistry() as a JSON array
// (one object per row, snake_case keys) to stdout and returns 0. Wired as a
// RETURNING branch inside main.cpp's flag loop (main.cpp ~1790-1825) —
// MUST return before that loop's post-loop fallthrough
// (`runRealFrontend()`), which hangs headless.
int runDumpRegistry();

// ---------------------------------------------------------------------------
// AER scope-2 A1 — the LEXICAL instrumentation dumps.
//
// Two read-only censuses of surfaces no existing dump reaches, both wired as
// RETURNING arms in main.cpp's flag loop for exactly the reason
// runDumpRegistry() above documents: the post-loop fallthrough calls
// runRealFrontend(), which hangs headless. Both flags stand alone and must come
// before any `--do`, which consumes every remaining argv.
//
//   --dump-optvocab                       every command carrying a registered
//                                         eval::OptVocab, as
//                                         {command, known[], unimplemented[],
//                                          aliases{}}.
//   --dump-optvocab --resolve <cmd> <opt> what dispatcher.cpp's resolution
//                                         point does to that one option, via
//                                         piet::resolveOptionKeyForCommand.
//   --dump-functions                      every expression-function name the
//                                         engine knows, tagged with the LAYER
//                                         that implements it.
//
// EVERY LIST THESE EMIT IS SORTED. OptVocab holds its sets in unordered_set /
// unordered_map, whose iteration order is unspecified and moves with both the
// libc++ version and the insertion history (registerOptVocab UNIONs across call
// sites), so an unsorted dump would print different bytes from identical source
// and could never be pinned.

// The `--dump-optvocab` array, as the exact string the flag prints (without its
// trailing newline). Exposed so selftest asserts on the serializer's own output
// rather than on a paraphrase of it.
std::string optVocabDumpJson();
int runDumpOptVocab();

// `--dump-optvocab --resolve <command> <option>`: one JSON object, always
// rc 0 — an unregistered command is an honest `vocab_registered: false` row,
// not an error.
int runDumpOptVocabResolve(const std::string& commandIn, const std::string& optionIn);

// The `--dump-functions` array, same string contract as optVocabDumpJson().
std::string functionDumpJson();
int runDumpFunctions();

}  // namespace piet::registry

#endif  // PIET_NATIVE_REGISTRY_H
