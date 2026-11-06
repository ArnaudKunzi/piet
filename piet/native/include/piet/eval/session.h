// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// EvalSession — the M1 subset of the Python SessionState (session.py),
// carrying exactly the state the macro layer reads/writes: local/global
// macros, scalars, r()/e() stores, matrices, variable labels, cwd, and the
// last-model result JSON.
//
// TODO(M2): this struct is absorbed into the full C++ SessionState when the
// session/datastore module is ported (dispatcher glue milestone). The macro
// engine only depends on the accessors here, so M2 can wrap or replace the
// storage without touching macros.cpp logic.
//
// Ordering: Python dicts preserve insertion order, and the macro engine's
// behavior depends on it (the scalar-substitution loop in _eval_arith walks
// session._scalars in insertion order). OrderedDict mirrors that.
#ifndef PIET_NATIVE_EVAL_SESSION_H
#define PIET_NATIVE_EVAL_SESSION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace cecon { class Mt64; }  // core/rng_mt64.h: Stata's session stream

namespace piet::eval {

// Store generations. A process-wide sequence; every OrderedDict / OrderedFloatDict
// carries the generation of its last write (a fresh one at construction and on
// every set, clear and erase of a present key), and every ENTRY carries the
// generation of its own last write. EvalMatrix carries one too. A copy carries
// its source's generations, so putting a saved copy back (runAssert's restore of
// r()) returns the store to exactly the state it had; a store assigned from a
// separately built dict (`rValues = newR`, `rValues = OrderedFloatDict()`) gets a
// generation no earlier state had.
//
// That is what lets ONE site replace r() for every r-class command
// (handlers_aer_estim.cpp replaceStaleRClass, called from dispatcher.cpp's
// dispatch()): Stata replaces all of r() at every r-class command, but PIET's
// r-class handlers MERGE their keys into whatever the previous command left
// (correlate, levelsof, count, describe, ... ). "Did this command write r()?" is
// a comparison of store generations with the sequence value at entry, and "which
// entries are left over from before it?" is the per-entry generation -- never a
// comparison of r() VALUES, which misses a command that writes the same values
// again (`summarize x1` twice).
//
// Keys with the reserved \x01 lead byte (test's accumulate set, sreturn's s()
// store) are bookkeeping, not r() contents: writing them leaves the store
// generation alone, and the r() replacement never erases them.
inline std::atomic<std::uint64_t>& storeGenerationSequence() {
  static std::atomic<std::uint64_t> seq{0};
  return seq;
}
inline std::uint64_t nextStoreGeneration() {
  return storeGenerationSequence().fetch_add(1, std::memory_order_relaxed) + 1;
}
// The last generation handed out: every write from now on is newer than this.
inline std::uint64_t currentStoreGeneration() {
  return storeGenerationSequence().load(std::memory_order_relaxed);
}
inline bool isReservedStoreKey(const std::string& k) { return !k.empty() && k[0] == '\x01'; }

// Insertion-ordered string map with Python-dict update semantics
// (assignment to an existing key keeps its position; new keys append).
class OrderedDict {
 public:
  bool contains(const std::string& k) const { return find(k) != nullptr; }

  const std::string* find(const std::string& k) const {
    for (const auto& [key, val] : items_) {
      if (key == k) return &val;
    }
    return nullptr;
  }

  // Python dict.get(k, def)
  std::string get(const std::string& k, const std::string& def = "") const {
    const std::string* v = find(k);
    return v ? *v : def;
  }

  void set(const std::string& k, std::string v) {
    const std::uint64_t g = nextStoreGeneration();
    if (!isReservedStoreKey(k)) generation_ = g;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].first == k) {
        items_[i].second = std::move(v);
        gens_[i] = g;
        return;
      }
    }
    items_.emplace_back(k, std::move(v));
    gens_.push_back(g);
  }

  void erase(const std::string& k) {
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].first == k) {
        if (!isReservedStoreKey(k)) generation_ = nextStoreGeneration();
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void clear() {
    generation_ = nextStoreGeneration();
    items_.clear();
    gens_.clear();
  }
  const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
  // See nextStoreGeneration.
  std::uint64_t generation() const { return generation_; }
  // Erase every non-reserved entry last written at or before `gen`. Bookkeeping
  // that follows a write which already moved the store: the store generation is
  // left as it is. True if anything was erased.
  bool eraseWrittenAtOrBefore(std::uint64_t gen) {
    bool any = false;
    for (size_t i = items_.size(); i-- > 0;) {
      if (gens_[i] <= gen && !isReservedStoreKey(items_[i].first)) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        any = true;
      }
    }
    return any;
  }

 private:
  std::vector<std::pair<std::string, std::string>> items_;
  std::vector<std::uint64_t> gens_;  // parallel to items_: each entry's last write
  std::uint64_t generation_ = nextStoreGeneration();
};

// Same, for the float-valued r()/e() scalar stores.
class OrderedFloatDict {
 public:
  bool contains(const std::string& k) const { return find(k) != nullptr; }

  const double* find(const std::string& k) const {
    for (const auto& [key, val] : items_) {
      if (key == k) return &val;
    }
    return nullptr;
  }

  void set(const std::string& k, double v) {
    const std::uint64_t g = nextStoreGeneration();
    generation_ = g;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].first == k) {
        items_[i].second = v;
        gens_[i] = g;
        return;
      }
    }
    items_.emplace_back(k, v);
    gens_.push_back(g);
  }

  void erase(const std::string& k) {
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].first == k) {
        generation_ = nextStoreGeneration();
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void clear() {
    generation_ = nextStoreGeneration();
    items_.clear();
    gens_.clear();
  }
  const std::vector<std::pair<std::string, double>>& items() const { return items_; }
  std::uint64_t generation() const { return generation_; }  // see nextStoreGeneration
  // See OrderedDict::eraseWrittenAtOrBefore.
  bool eraseWrittenAtOrBefore(std::uint64_t gen) {
    bool any = false;
    for (size_t i = items_.size(); i-- > 0;) {
      if (gens_[i] <= gen) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        any = true;
      }
    }
    return any;
  }

 private:
  std::vector<std::pair<std::string, double>> items_;
  std::vector<std::uint64_t> gens_;  // parallel to items_: each entry's last write
  std::uint64_t generation_ = nextStoreGeneration();
};

// A named matrix as the Python side stores it: {data, rownames, colnames}.
// `gen` is the generation it was built at (see nextStoreGeneration); a copy keeps
// it. It is how the r() replacement tells an r() matrix this command posted from
// one a previous command left. Code that posts a COPY of an existing matrix into
// rMatrices must give it a fresh one (`m.gen = nextStoreGeneration()`), or the
// copy reads as left over and the next replacement erases it.
struct EvalMatrix {
  std::vector<std::vector<double>> data;  // row-major; NaN = Stata missing
  std::vector<std::string> rownames;
  std::vector<std::string> colnames;
  std::uint64_t gen = nextStoreGeneration();
};

// A point in the life of r(): the sequence value then, and how many r() matrices
// existed (an r() matrix erased without a replacement is a write the generations
// alone cannot show). Taken by dispatch() at entry and after every command.
struct RStoreMark {
  std::uint64_t seq = 0;
  std::size_t rMatrixCount = 0;
};

struct EvalSession {
  // Pending `ml model lf` definition and starting values. Kept separately
  // from live e(): a failed definition must never replay a previous model.
  std::optional<nlohmann::ordered_json> mlState;
  OrderedDict locals;     // _locals
  OrderedDict globals;    // _globals
  OrderedDict scalars;    // _scalars (value strings)
  // BUG-710: the scalars whose stored value is a STRING (`scalar st = s[2]`,
  // `scalar t = "abc"`), each with the exact text it marked. The store above
  // keeps no type, so `local x = st` substituted `ab2` as a bare NAME and the
  // line stored its own source text `st` (w2i_varsub d_02). A mark counts only
  // while the store still holds the text it was made for (isStringScalar), so
  // a writer that bypasses the `scalar =` branch (parallel's copy-back, the
  // evalcase harness, `scalar drop` in a program's cleanup) cannot leave a stale
  // string type on a later numeric value. Written by tryCaptureDefinition's
  // `scalar` branch only.
  std::map<std::string, std::string> stringScalars;
  bool isStringScalar(const std::string& name) const {
    const auto it = stringScalars.find(name);
    if (it == stringScalars.end()) return false;
    const std::string* v = scalars.find(name);
    return v != nullptr && *v == it->second;
  }
  OrderedFloatDict rValues;   // _r_values
  OrderedDict rStrings;       // _r_strings
  OrderedFloatDict eScalars;  // _e_scalars
  // e-class macros. Until AER bundle H the only e() macros PIET could print
  // were e(cmd)/e(depvar)/e(vce), read straight off LastModel, and
  // runEreturnList's header says the rest are "silently omitted rather than
  // invented". pca has to publish eight (e(cmd), e(Ctype), e(title),
  // e(properties), e(predict), e(rotate_cmd), e(estat_cmd), e(marginsnotok)),
  // so the omission becomes a store instead of a disclaimer.
  OrderedDict eStrings;       // _e_strings
  std::vector<std::pair<std::string, EvalMatrix>> matrices;    // _matrices
  // Mata's own namespace (mata_subset.h, M0). SEPARATE from `matrices` because
  // Stata keeps them apart and paper 030 relies on it: `matrix rchange = e(b)`
  // and `mata: rchange = (rchange \ st_matrix("rchange"))` name two different
  // objects, so one shared store would join e(b) onto itself -- a silent wrong
  // answer. Only `st_matrix("X")` crosses over (it reads `matrices`); bare Mata
  // names and `getmata` read and write this store only. Mata matrices carry no
  // row/column names: `colnames` is sized to the column count and left empty so
  // a void 0 x c matrix keeps its c (mata::colsOf).
  std::vector<std::pair<std::string, EvalMatrix>> mataMatrices;
  std::vector<std::pair<std::string, EvalMatrix>> eMatrices;   // _e_matrices
  // r-class matrices (Stata's `r(table)` and friends). Added for AER bundle H's
  // sktest, which returns a matrix and not just scalars. Kept as a THIRD store
  // rather than folded into rValues/matrices because r() matrices have the same
  // lifetime as the rest of r() -- they are replaced wholesale by the next
  // r-class command -- while `matrices` is the user's own namespace that must
  // survive it.
  std::vector<std::pair<std::string, EvalMatrix>> rMatrices;   // _r_matrices

  // r(table) bookkeeping (handlers_aer_estim.cpp), kept OUTSIDE every r() store
  // on purpose: an r-class command that wipes r() wholesale (`label language`,
  // getmata, boottest) used to delete a stamp kept under a reserved rStrings key,
  // and the table then outlived every later r-class command (BUG-629, review
  // 2026-10-11). rTableGen is the gen of the r(table) matrix the estimation
  // machinery posted (0 = none of ours; a table whose gen differs was posted by
  // someone else -- sktest, nlcom); rTableEid / rTableModel are the e() identity
  // and model print it belongs to. rTableRefusal is the reason r(table) is NOT
  // posted after the last estimation (interaction columns, margins, a
  // replicating prefix) -- a later read is then a named r(111) -- with the e()
  // identity and the generation it was left at; it dies with r() like a table.
  std::uint64_t rTableGen = 0;
  std::uintptr_t rTableEid = 0;
  std::string rTableModel;
  std::string rTableRefusal;
  std::uintptr_t rTableRefusalEid = 0;
  std::uint64_t rTableRefusalGen = 0;
  // Where r() stood when the last command (leaf or container) finished: a
  // container's own writes after its last inner command (an rclass program's
  // `return` set, committed when the program ends) are judged against it.
  RStoreMark rMarkAfterLastCommand;
  // Which r() names the live r-class command leaves absent although Stata posts
  // them there (internal capture
  // RULING.md C.1/C.3). Kept outside every r() store, like the r(table)
  // bookkeeping; set by handlers::rTableAfterCommand only when a command
  // REPLACED r(). rClassCmd names that command (messages only). rUnposted are
  // the names Stata posts for that invocation that PIET did not post: a read of
  // one is a named r(111), never `.`. rClassSetUnknown = the command's Stata set
  // is not declared, so an absent bare/delimited read is a named r(111) too.
  // rDeclaredCmd / rDeclaredNames are the declaration an enumerated dispatcher
  // arm makes for the CURRENT leaf (declareRPosted); the dispatch hook consumes
  // and clears it.
  std::string rClassCmd;
  std::vector<std::string> rUnposted;
  bool rClassSetUnknown = false;
  std::string rDeclaredCmd;
  std::optional<std::vector<std::string>> rDeclaredNames;

  OrderedDict varLabels;  // _var_meta[name]["label"] (the only key M1 reads)
  std::string cwd;        // session.cwd ("" = unset)

  // BUG-512: `_N` in a MACRO-context expression (`local n = _N`, `scalar s =
  // _N`, `` `=_N' ``, `return scalar N = _N`, `display _N*2`). This layer had no
  // way to know the observation count, so the name reached the evaluator as an
  // undefined identifier and `local n = _N` stored the two characters `_N`
  // (tests/eval/golden/aer_w2/w2_smalls.log section A: Stata stores 12).
  //
  // A PULL hook, not a projected value like varLabels. The count moves with
  // every data verb, and its consumers are every evalArith/expandMacros call
  // site in the tree (a dozen today); a projection would have to be refreshed
  // at each of them, and the one that forgot would go back to storing `_N` as
  // text -- silently. A hook is read at the moment of need and cannot be stale.
  // It keeps the layering BUG-208 drew: this header still knows nothing about
  // SessionState, which installs the hook in its constructor.
  //
  // Unset (the evalcase harness, session-less selftest callers) = this layer
  // does not know, and `_N`/`_n` stay exactly as unresolved as they were.
  std::function<long long()> obsCount;

  // runiform()/runiformint() in a MACRO-context expression (`local u =
  // runiform()`, `` `=runiform()' ``) follow `set seed` (w2c_functions.log
  // runiform_macro/runiformint_macro). A PULL hook for the same reason as
  // obsCount above: `set seed` lives on SessionState, which installs this in
  // its constructor. `epoch` advances on EVERY `set seed`, so re-issuing the
  // same seed restarts the stream exactly as Stata does; the draw counter
  // below restarts whenever the epoch it last saw moves. Unset (session-less
  // harnesses) = the default seed 123456789 (translate.cpp's, for the
  // engine's own draws) at epoch 0.
  struct RngSeedReading {
    std::optional<long long> seed;
    unsigned long long epoch = 0;
  };
  std::function<RngSeedReading()> rngSeed;
  // Stata's session RNG stream itself (SessionState::seed.stream(), mt64): the
  // macro layer's runiform()/runiformint() draw from it, in the order the
  // expressions run, sharing it with generate's draws. Unset (session-less
  // harnesses) = the Philox fallback below keyed on rngSeed.
  std::function<cecon::Mt64*()> rngMt64;

  // e(sample) T2 (internal note esample/SPEC.md §5.4): `e(sample)`
  // in a MACRO-context expression (`display e(sample)`, `local s = e(sample)`,
  // `scalar s = e(sample)`, `if e(sample) {`) is the mask's value at
  // observation 1 -- Stata's rule for a variable in a scalar expression
  // (w2_esample.log B16-B19, B29-B29c; 0 with no estimation results, A2-A4).
  // The mask lives on the working frame, which this layer cannot read, so it is
  // a PULL hook like obsCount, installed by SessionState's constructor. Returns
  // 0/1, or nullopt with the r(198) refusal text in `why` when the model has no
  // mask PIET can vouch for. Unset (session-less harnesses) = refused by name.
  std::function<std::optional<int>(std::string* why)> esampleObs1;

  // BUG-718: a dataset-variable subscript `VAR[i]` in a MACRO-context expression
  // (`` `=__d_itrim[1]' ``, w2c_functions.log itrim_data and twelve sibling
  // records). The data lives on the working frame, so it is a PULL hook like
  // obsCount, installed by SessionState's constructor; it reads through the same
  // reader `display VAR[i]` uses (display_hardening readVarObservation).
  // nullopt = `name` is not a dataset variable. `inRange` is false for an index
  // outside 1.._N (the text is then display's `.`; isString is the column's kind).
  // Unset (session-less harnesses) = the subscript stays unresolved, as before.
  struct VarObsReading {
    std::string text;
    bool isString = false;
    bool inRange = true;
  };
  std::function<std::optional<VarObsReading>(const std::string& name, long long idx)> varObs;
  // BUG-719: refresh the variable plane (varLabels' reserved keys: exists/type/
  // format) from the working frame -- SessionState::syncEvalVarLabels. The
  // dispatcher refreshes it only before a `local`/`global`/`label` line, so an
  // inline `` `: type VAR' `` on a `display` line read the plane of an earlier
  // line and answered r(111) for a variable generated since. The inline arm
  // calls this first. Unset (session-less harnesses) = the hand-written plane.
  std::function<void()> syncVarPlane;
  // `: label lblname #` and `: label (varname) #` (w2ah_data_verbs.log
  // ah_rename_lab, ah_encode_gen, ah_recode_g_lab, ah_clone_byte): the value-label
  // sets live on SessionState (labelDefs, code text -> label text), so a PULL
  // hook like obsCount. Returns the named set, or nullptr when no set of that
  // name is defined. Unset (session-less harnesses) = no set is defined.
  std::function<const std::map<std::string, std::string>*(const std::string& lblname)>
      valueLabelSet;
  // BUG-1006: the delimited `` `c(name)' `` reference (`display "`c(k)'"`,
  // w2aq_histogram_family.log every hk_*_g record). The values live on
  // SessionState (piet::resolveCValue, cclass.h), so a PULL hook like obsCount,
  // installed by SessionState's constructor. Returns the raw text (no quotes),
  // or nullopt for a name outside resolveCValue's spine, which expandPass then
  // expands to "" when Stata has no such c() name either and refuses by name,
  // r(133), when it does (BUG-1330). Unset (session-less harnesses) = no name
  // resolves.
  std::function<std::optional<std::string>(const std::string& name)> cValue;
  // EM13 / LBL-CHAR: `: char NAME[CHAR]` reads a characteristic (SessionState's
  // `char` table: `_dta[note0]`, `x[note1]`, `_dta[_TSpanel]`), a PULL hook like
  // valueLabelSet. nullopt = not set (the function's answer is then ""). Unset
  // (session-less harnesses) = nothing is set.
  std::function<std::optional<std::string>(const std::string& target, const std::string& name)>
      characteristic;
  // LBL-DATALABEL: `: data label` reads SessionState::dataLabel (`label data`), a
  // PULL hook like the one above. Unset = no label ("").
  std::function<std::string()> dataLabel;
  // `: sortedby` (and the inline `` `: sortedby' ``) reads the head frame's
  // sort marker -- the engine file's `sorted_by` claim (Datastore::sortedBy),
  // minted where Stata's `sort` sets its marker. A PULL hook like dataLabel.
  // Unset = no marker ("").
  std::function<std::string()> sortedBy;
  unsigned long long rngEpochSeen = 0;  // the epoch rngDraws counts from
  unsigned long long rngDraws = 0;      // Philox counter of the next draw

  // session.last_model.result — raw result dict (coeftable + scalars) when
  // an estimation is "in memory". TODO(M2): becomes the full LastModel.
  // Not a coefficient source while estBlock is installed (coefSource below):
  // it is the engine's raw reply for most fits, and estpost, erepost and
  // `estimates use` leave it describing an earlier e() (eb-order SPEC B.4).
  // Its readers are the fitted e-scalars (resolveEScalar) and the store
  // snapshot/restore.
  std::optional<nlohmann::ordered_json> lastModelResult;

  // eb-order T-P3 (internal note eb-order/SPEC.md B.4,
  // T-P3 parts 0-1): ONE coefficient source for e(b)/e(V)/_b[]/_se[] in every
  // layer -- the live e()'s estimation block (SessionState::lastEstBlock), read
  // through a PULL hook for the reason obsCount is one: a hook is read at the
  // moment of need and cannot be stale, and this header keeps knowing nothing
  // about SessionState (BUG-208). SessionState's constructor installs both.
  //   estBlock  -- the block, or null when the live e() has none (no e() at
  //                all, or pca/rdrobust/rddensity: a fresh fit clears the block
  //                and only the fits that have an e(b) write theirs).
  //   modelLive -- whether an e() is live at all; tells "no e()" from "an e()
  //                without an e(b)", which a null block alone cannot.
  // Unset (the evalcase harness, session-less selftests): lastModelResult is
  // the source and its presence the liveness, exactly as before.
  std::function<const nlohmann::ordered_json*()> estBlock;
  std::function<bool()> modelLive;
  // Ordered e() name reflection is complete for an explicit posting. Ordinary
  // fits still have lazily resolved results beyond these stores; listing only
  // their stored subset would claim an incomplete namespace as complete.
  std::function<bool(const std::string&)> eNameListsComplete;
  // A fit that publishes Stata's whole ordered name list for one kind
  // ("scalars", "macros", "matrices") answers it here: the list in Stata's
  // order, or nullopt when that fit published no list for the kind, or the live
  // e() no longer matches it (another fit, a posting, a name added or lost).
  // SessionState::eNameOrderFor; unset in session-less harnesses.
  std::function<std::optional<std::vector<std::string>>(const std::string&)> eNameOrder;
  // A scoped coefficient source, set only by esttab's transform() guard
  // (handlers_aer_tables.cpp TrScopedModel) while it evaluates one stored model.
  const nlohmann::ordered_json* coefOverride = nullptr;

  // The block _b[]/_se[] and the canonical e(b)/e(V) read: the override, else
  // the hook's value (null included -- falling back to lastModelResult would
  // bring a stale copy back), else lastModelResult.
  const nlohmann::ordered_json* coefSource() const {
    if (coefOverride != nullptr) return coefOverride;
    if (estBlock) return estBlock();
    return lastModelResult.has_value() ? &*lastModelResult : nullptr;
  }
  // Whether an e() is live, in coefSource()'s order. coefSource() != null
  // implies eLive() whenever a session is attached.
  bool eLive() const {
    if (coefOverride != nullptr) return true;
    if (modelLive) return modelLive();
    return lastModelResult.has_value();
  }

  // A live e() that has no e(b)/e(V) at all (pca, rddensity, an estpost
  // posting): Stata's `matrix X = e(b)` and `matlist e(b)` are rc 0 there and X
  // is a 0x0 matrix (w2v_estpost_ematrix.log ep13_b_shape, ep15c_b_shape
  // `0|0|0||`, ep15c_V_shape; w2g_ebmat.log:2087, :2096), while `matrix list
  // e(b)`, `: colnames e(b)`, el() and colsof() refuse. Only b and V: no other
  // absent e() matrix is captured.
  // rdrobust is not one of them: Stata posts a 1x3 e(b) there (w2g_ebmat.log:2305
  // `e(b)[1,3]`), which runRdrobust posts; an rdrobust e() without it refuses
  // by name (resolveEMatrix's closed stripe table).
  bool eMatrixAbsentFromLiveE(const std::string& name) const {
    if (!((name == "b" || name == "V") && findClassMatrix('e', name) == nullptr && eLive() &&
          coefSource() == nullptr))
      return false;
    const std::string* cmd = coefOverride == nullptr ? eStrings.find("cmd") : nullptr;
    return cmd == nullptr || *cmd != "rdrobust";
  }

  // eb-order T-P1: the one resolver of e(NAME) as a matrix. A matrix POSTED
  // for the live e() wins (estat event/margins post, ereturn post, estadd
  // matrix, pca/oaxaca/teffects); otherwise e(b)/e(V) are canonicalEMatrix of
  // coefSource(). nullopt with `rc`/`why` set when there is nothing to answer:
  // 301 with no e() at all, 111 for a live e() without that matrix, and the
  // named refusal of an e(V) whose fit sent no covariance (never a diagonal).
  // Defined in eval/macros.cpp.
  //
  // eb-order ROUND 3 (internal capture
  // DIAGNOSIS.md section 3): a matrix is BUILT only for an e(cmd) the closed
  // stripe table names; any other e(cmd) is a named r(198). Three readers ask:
  //   kCopy    -- `matrix X = e(b)`, `matrix list e(b)`, `matlist e(b)`;
  //   kNames   -- `: colnames/colfullnames/rownames/rowfullnames e(b)`;
  //   kOperand -- el()/colsof()/rowsof() and the e(b)[i,j] subscript in an
  //               expression.
  // They differ in two places only, both open completion gaps with an owner:
  // after a factor-variable fit (the engine sends no base-level `1b.` columns,
  // task T-E2) kCopy answers the short matrix bin/main answered and the other
  // two refuse; and with no fitted covariance only kOperand answers, from the
  // diagonal (`diagonalOnly`: el(e(V),i,i) = se_i^2, an off-diagonal cell is
  // refused by the caller).
  enum class EReader { kCopy, kNames, kOperand };
  struct EMatrixAnswer {
    std::optional<EvalMatrix> matrix;
    int rc = 0;
    std::string why;
    // Nothing to answer and nothing refused: no e() at all (rc 301) or a live
    // e() without this matrix (rc 111). Expression operands read it as Stata's
    // r(109) "type mismatch" (an empty e() macro where a matrix is required).
    bool absent = false;
    bool diagonalOnly = false;
  };
  EMatrixAnswer resolveEMatrixFor(const std::string& name, EReader reader) const;
  // The fit-matrix half of resolveEMatrixFor (e(b)/e(V) of `source` under the
  // live e(cmd)'s stripe row) for an explicit source block. resolveEMatrixFor
  // passes coefSource(); the derived e(beta)/e(V_modelbased) pass a local copy
  // of it with the coefficients or covariance replaced, so the live block (and
  // its identity, handlers_aer_estim.cpp liveEstimationIdentity) is never
  // swapped or touched.
  EMatrixAnswer resolveFitEMatrixFrom(const std::string& name, EReader reader,
                                      const nlohmann::ordered_json* source) const;
  std::optional<EvalMatrix> resolveEMatrix(const std::string& name, int* rc = nullptr,
                                           std::string* why = nullptr,
                                           EReader reader = EReader::kCopy) const {
    EMatrixAnswer a = resolveEMatrixFor(name, reader);
    if (!a.matrix.has_value()) {
      if (rc != nullptr) *rc = a.rc;
      if (why != nullptr) *why = std::move(a.why);
    }
    return std::move(a.matrix);
  }

  const EvalMatrix* findMatrix(const std::string& name) const {
    for (const auto& [k, m] : matrices) {
      if (k == name) return &m;
    }
    for (const auto& [k, m] : eMatrices) {
      if (k == name) return &m;
    }
    return nullptr;
  }

  // e()/r() matrices are NOT reachable through findMatrix: that one resolves the
  // BARE-name namespace (`matlist A`), and folding the class stores into it
  // would let `matlist table` silently answer with r(table). Callers that have
  // already parsed the `e(...)`/`r(...)` wrapper come here instead.
  const EvalMatrix* findClassMatrix(char cls, const std::string& name) const {
    const auto& store = (cls == 'r') ? rMatrices : eMatrices;
    for (const auto& [k, m] : store) {
      if (k == name) return &m;
    }
    return nullptr;
  }

  // BUG-456 (2026-09-08): the e() namespace is ONE namespace in Stata --
  // `ereturn local x` after `ereturn scalar x` is refused (rc 110/152,
  // refute3_capture.log Y6pre). PIET keeps the macro and scalar halves in two
  // stores, so a stale entry in one could shadow the other after the bare-form
  // resolver learned to read both. These two setters are the cheap rule that
  // keeps a lookup from ever finding both: the last write wins and erases its
  // sibling. An EMPTY e-macro is ABSENT in Stata (`ereturn local em ""` ->
  // `assert e(em)==.` rc 0, `==""` r(109); T13-T15), so setting "" erases
  // rather than storing a present-but-empty string. Every writer of the two
  // stores goes through here; `.set` on the stores directly is the old,
  // shadowable shape.
  void setEString(const std::string& k, std::string v) {
    eScalars.erase(k);
    if (v.empty()) {
      eStrings.erase(k);
      return;
    }
    eStrings.set(k, std::move(v));
  }
  void setEScalar(const std::string& k, double v) {
    eStrings.erase(k);
    eScalars.set(k, v);
  }

  // The current r() mark (see RStoreMark).
  RStoreMark rMark() const { return RStoreMark{currentStoreGeneration(), rMatrices.size()}; }
};

// eb-order T-P1 (SPEC B.4 "One builder"): Stata's e(b) (`which` 'b') or e(V)
// ('V') from an estimation block's coeftable, in Stata's column order:
// `(intercept)` is spelled `_cons` and moved last within its equation, the
// other columns keep the engine's order, and e(V) is the engine's full `e_V`
// ({names, matrix}) permuted to that order. nullopt with `why` set when the
// block has no coeftable or, for e(V), no `e_V` (a coeftable carries standard
// errors, not covariances, so a diagonal would be a silent wrong answer).
// Defined in eval/macros.cpp.
std::optional<EvalMatrix> canonicalEMatrix(const nlohmann::ordered_json& block, char which,
                                           std::string* why = nullptr);

// eb-order T-E2 (ME08/C7/EB-BASE): Stata's e(b) stripe as the FIT recorded it
// in its block (`eb_stripe`, written once at the fit by handlers_aer_estim.cpp
// annotateEbStripe, from the same factor-level rules r(table) uses): Stata's
// columns in order, each the canonical (canonicalEMatrix) column it reads, or
// an empty `src` for a column Stata posts and the engine does not send -- a
// factor's base level (`0b.g`, b 0, a zero row/column of e(V)). An omitted
// column the engine sent under its bare name carries its Stata `o.` name
// (`o.dz0` reads `dz0`). nullopt when the block has no stripe or the stripe
// does not cover `canonNames` exactly once (then the canonical names stand).
struct EbStripeEntry {
  std::string name;  // Stata's column name (no equation)
  std::string src;   // the canonical column it reads; "" = a base column (0)
};
std::optional<std::vector<EbStripeEntry>> ebStripeOf(const nlohmann::ordered_json& block,
                                                     const std::vector<std::string>& canonNames);
// Re-lay a canonical e(b)/e(V) (`which` 'b'/'V') on the block's stripe; false
// (m untouched) when the block has none. Base columns hold 0 (b, and V's row
// and column).
bool applyEbStripe(const nlohmann::ordered_json& block, char which, EvalMatrix& m);
// The block's Stata coefficient columns in e(b) order with their coeftable
// rows (nullptr = a base column, b 0 and se 0): canonicalEMatrix's order, or
// the stripe's when the block has one. `(intercept)` is spelled `_cons`. For
// the readers that walk coefficient rows (parmest, regsave, `_b[]`).
struct EbColumnRow {
  std::string name;
  const nlohmann::ordered_json* row = nullptr;
};
std::vector<EbColumnRow> stataEbColumns(const nlohmann::ordered_json& block);
// `_b[name]`/`_se[name]` (`kind` "b"/"se") for a column only the stripe names:
// a base level (`_b[0b.c]` and `_b[0.c]` are 0, w2f_residuals.log e_b_0bc /
// e_b_0c) or an `o.` spelling of an omitted column. nullopt = not such a name.
std::optional<double> ebStripeCoef(const nlohmann::ordered_json& block, const std::string& name,
                                   const std::string& kind);
// The posted name `_b[name]` means when it is not spelled exactly: an
// abbreviated variable (`_b[1.treatment#1.husb]`, StataBE uni-oracle/hz.do
// Y4/Z2) or an interaction in another atom order -- the engine's
// postest::resolve_coef over the coeftable and stripe names, so `_b[]` and
// test/lincom agree. "" when no other column matches.
std::string coefFullName(const nlohmann::ordered_json& block, const std::string& name);
// The equation Stata names every e(b) column of the live fit with, when its
// e(cmd) posts one named after the depvar (logit/probit `yb:`, poisson with `.`
// spelled `_`): `_b[yb:x1]` and `[yb]_b[x1]` read the bare row `x1` (BUG-1328).
// "" for every other fit.
std::string depvarEquationOf(const EvalSession& session);
// The omitted spelling a plain `_b[name]` / `_b[eq:name]` also answers to: an
// omitted column posts as `o.name` (`yb:o.d` after ivprobit, BUG-1169) and Stata
// reads it under the unmarked name, b 0. "" when `name` is no plain variable.
std::string omittedColumnSpelling(const std::string& name);
// Whether Stata's e(b) for the live fit has columns the engine's table does not
// carry (tobit's var(e.y), nbreg's lnalpha, ologit's cuts, the other ancillary
// fits whose e(b) readers refuse): erepost's conformability test (BUG-1203).
bool ebHasUnsentColumns(const EvalSession& session);

// An r-class command REPLACES all of r() (Stata): the r() scalars, the user r()
// macros and every r() matrix, r(table) included, together with the r(table)
// bookkeeping. The reserved  keys of rStrings (sreturn's s() store, test's
// accumulate set) are left to their owners -- `return clear`'s long-standing
// convention; that s() survives `label language` follows this convention, it is
// not an oracle claim. The one copy of the shape: `return clear`, `label
// language`, getmata, and an estimation (which then posts its own r(table)) call
// it; the dispatch hook's replaceStaleRClass covers every handler that merges.
inline void replaceRClass(EvalSession& ev) {
  ev.rValues = OrderedFloatDict();
  std::vector<std::string> userKeys;
  for (const auto& kv : ev.rStrings.items())
    if (!isReservedStoreKey(kv.first)) userKeys.push_back(kv.first);
  for (const auto& k : userKeys) ev.rStrings.erase(k);
  ev.rMatrices.clear();
  ev.rTableGen = 0;
  ev.rTableEid = 0;
  ev.rTableModel.clear();
  ev.rTableRefusal.clear();
  ev.rTableRefusalEid = 0;
  ev.rTableRefusalGen = 0;
  // r() is now known-empty. The declaration pair is left alone: an arm may
  // declare before or after it calls this (RULING.md D.1).
  ev.rClassCmd.clear();
  ev.rUnposted.clear();
  ev.rClassSetUnknown = false;
}

// The one setter for an r-class arm's declaration (RULING.md C.3/D.3): the
// names Stata posts for THIS invocation of `cmd`, made on the success path
// after the arm's r() writes. handlers::rTableAfterCommand turns it into
// rUnposted (declared minus present) when the command replaced r().
inline void declareRPosted(EvalSession& ev, std::string cmd, std::vector<std::string> stataNames) {
  ev.rDeclaredCmd = std::move(cmd);
  ev.rDeclaredNames = std::move(stataNames);
}

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_SESSION_H
