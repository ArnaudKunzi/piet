// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M3b: MI-066 data verbs (merge/reshape/append/duplicates/gsort/order/
// recode) via cobra 0.18.0's native CLI verbs. C++ ports of the
// corresponding piet/backend/piet/{merge_op,reshape,append,duplicates,
// gsort,order,recode}.py oracle modules — bug-for-bug where cobra 0.18.0
// and the Python backend agree; where they differ, the FROZEN 1.16.0
// corpus wins (this file pre-validates the oracle's own error conditions
// in C++ BEFORE shelling out to cobra, so error text matches byte-for-byte
// even where cobra's native error text differs — see each handler's notes).
#include "piet/handlers.h"
#include "piet/translate.h"

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "core/ident.h"  // gsort's varspec: bytes >= 0x80 are identifier bytes
#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/column_fetch.h"  // headColumnValues: one column of a resident head
#include "piet/dispatcher.h"  // clonevar/recode dispatch generate/replace lines
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/loops.h"         // expandVarlist (collapse's output metadata)
#include "piet/storage_type.h"  // resolveCreateType (collapse's c(type) outputs)
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
#include "handlers_data_verbs_internal.h"
#include "estimation_m4_internal.h"  // e(sample) T2: the copy verbs' mask helpers
namespace {
const piet::eval::OptVocab& kVocabOrder = piet::eval::registerOptVocab(
    {"order"}, {{"first", "last", "before",
                                                                   "after", "alphabetic"},
     {"sequential"}});
const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j"},
     {}});
// 2026-09-10: `generate`, `update` and `replace` REMOVED from this row's unimplemented set.
// They are implemented — `handlers_data_verbs2.cpp`'s kVocabMerge (the one `runMerge` actually
// reads) carries all three in `known`, and this build answers Stata's cells exactly:
// 74 non-missing after update, `_merge==4` on the 5 filled rows, 69 matched, `mflag` created and
// `_merge` absent (tests/eval/golden/aer_b4/b5_riders_pins.log).
//
// This registration is DEAD CODE — there is no runMerge in this translation unit — but
// `registerOptVocab` UNIONS same-named registrations across translation units, so its
// unimplemented set repolluted the merged vocabulary even though `known` wins at runtime. The
// live surface was therefore correct while the CENSUS, which reads the vocabulary and not the
// code, counted all 83 corpus uses of `merge, update`/`merge, gen()` as gaps. A vocabulary row is
// not documentation: something reads it, and here that something was the instrument measuring us.
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate",
                                                                   "keep", "assert"},
     {"nolabel", "nonotes", "noreport", "force", "sorted"}});
// BUG-897: tsfill's vocabulary is registered once, beside its one reader
// (handlers_data_verbs3.cpp runTsfill); the copy here was never read.
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// R2: the shared helpers that lived here moved VERBATIM to
// handlers_data_verbs_internal.h (dv_detail) — three TUs consume them now.
using namespace dv_detail;

// ---------------------------------------------------------------------------
// save FILE [, replace] — C++ port of dispatcher.py's save_data handler.
// Persists the current dataset by copying the session hbin to FILE
// (in-memory dataset unchanged, Stata-faithful).
ojson runSaveData(const std::string& filepathArg, bool replace, SessionState& session,
                  int dtaRelease) {
  // C4-L2A (D1 card plane, brief §1 save row): this entry check needs only
  // "is a dataset loaded", which is hasData() — the old
  // `session.datastore() == nullptr` half was logically REDUNDANT (datastore()
  // returns nullptr exactly when currentHbin_ is unset, session_state.h) yet
  // forced a full Datastore open (mmap + whole-file FNV) and, on a dirty
  // virtual head, the EMH frame snapshot, before EVERY save — on the
  // minted-tempfile virtual-save path below that construct is 100% pure waste
  // (residentSaveVirtual snapshots worker RAM, never reads the mmap). The
  // real-file copy path gets its on-disk source via the explicit
  // ensureMaterializedHead below instead. Escape arm (PIET_NO_RESIDENT_READ=1)
  // keeps the old entry verbatim — construct, EMH and the BUG-124 throw
  // semantics included — so the RED/A-B arm reproduces pre-D1 behavior.
  if (piet::residentReadDisabled()) {
    if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
      return engineErrorBlock("save: no dataset in memory");
    }
  } else if (!session.hasData()) {
    return engineErrorBlock("save: no dataset in memory");
  }
  std::string rawFp = trim(filepathArg);
  if (rawFp.size() >= 2 && ((rawFp.front() == '"' && rawFp.back() == '"') ||
                            (rawFp.front() == '\'' && rawFp.back() == '\''))) {
    rawFp = rawFp.substr(1, rawFp.size() - 2);
  }

  // Public save uses Stata format 118, including extensionless names and
  // tempfile paths. A tempfile's suffix does not change its file format:
  // w2ar_data_io_family dx_sa_004 / dx_save_079_emptyok read the actual
  // <release>118</release> bytes back. Explicit .hbin remains the native plane.
  std::string rawExt = extOf(rawFp);
  std::string effectiveFp = rawFp;
  if (rawExt.empty()) {
    effectiveFp = rawFp + ".dta";
    rawExt = ".dta";
  }
  const bool isDta = rawExt == ".dta" || rawExt == ".tmp" || dtaRelease > 0;
  if (!isDta && rawExt != ".hbin") {
    std::string suggested = rawFp.substr(0, rawFp.size() - rawExt.size()) + ".dta";
    return engineErrorBlock(
        "save: `" + rawExt + "` is not a save format this version of PIET can write. Use `save " +
        suggested + (replace ? ", replace" : "") + "` for Stata format or an explicit `.hbin`.");
  }

  // Resolve against session.cwd, NOT session.workdir() (parity FIX, native-
  // paths stabilization item 2): `save x.hbin` now lands next to the user's
  // own files (cwd), matching real Stata — session.workdir() is the
  // internal hbin-chain scratch dir and was never a user-meaningful
  // location for a relative `save`.
  std::string dest = resolveUserPath(session, effectiveFp);
  // Both refusals name the file as typed, in Stata's words (BUG-410): StataBE
  // `file s1.dta already exists` r(602), and for a directory that is not there
  // `file nodir/s.dta could not be opened` r(603).
  auto refuse = [&](const std::string& why, const char* rc) {
    return ojson(eval::errorBlock("engine_error", "file " + effectiveFp + " " + why, std::nullopt,
                                  std::nullopt, std::nullopt, std::string(rc)));
  };
  if (fileExists(dest) && !replace) return refuse("already exists", "r(602)");
  {
    const size_t slash = dest.find_last_of('/');
    std::error_code dirEc;
    if (slash != std::string::npos && slash > 0 &&
        !std::filesystem::is_directory(cecon::to_native_path(dest.substr(0, slash)), dirEc))
      return refuse("could not be opened", "r(603)");
  }
  // Explicit native checkpoints under the session tempdir retain the RAM
  // snapshot optimization. Public tempfile saves above produce actual DTA bytes.
  const bool isMintedTempfile = !isDta && rawExt == ".hbin" && session.runTempdir.has_value() &&
                                dest.rfind(*session.runTempdir + "/", 0) == 0;
  if (isMintedTempfile &&
      std::none_of(session.mintedTempfiles.begin(), session.mintedTempfiles.end(),
                   [&dest](const auto& item) { return item.second == dest; })) {
    session.mintedTempfiles.emplace_back(session.preserveLevel(), dest);
  }
  // A real file is about to land at `dest`: an earlier RAM-only save under the
  // same name must not answer a later `use` instead of it.
  if (isDta && session.isVirtualSave(dest)) session.residentVirtualDrop(dest);
  // e(sample) T2: the estimation-sample mask is a hidden column of the working
  // frame (piet/datastore.h kEsampleColumn), not the user's data, and Stata's
  // `save` does not write its marker: `save` + `use` reads e(sample) 0, and an
  // `append using` of a saved copy adds rows outside the sample
  // (w2_esample.log:1272 E14 |0|0|0|28|, :1104 E7 |0|0|28|122|). The `.dta`
  // writer (export-dta) already leaves `__piet_*` columns out; the two hbin
  // planes copy the head's bytes, so when the head carries the mask the file
  // is written by an engine `eval --drop` instead (below). Checked only in a
  // session where a fit installed one.
  bool stripMask = false;
  if (!isDta && session.esampleColumnInstalls > 0) {
    try {
      Datastore* mds = session.datastore();
      stripMask = mds != nullptr && mds->has(kEsampleColumn);
    } catch (const std::exception&) {
      stripMask = false;  // an unreadable head fails the copy below, honestly
    }
  }
  if (isMintedTempfile && !stripMask && session.residentSaveVirtual(dest)) {
    // BUG-198: the virtual arm writes no file, so there is no header to state
    // the session's format/label overrides in. Park them beside the entry so
    // the `use` that restores it re-applies exactly what the disk arm would
    // have round-tripped through the header (both sides call hbinMetaPatch, so
    // the two planes agree by construction rather than by inspection).
    // Costs one card read, and ONLY when the session has an opinion at all —
    // hasHbinMetaOverrides answers "no" from an empty varMeta without a walk,
    // which is the whole tempfile-checkpoint hot path.
    if (!session.varMeta.empty()) {
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      if (card != nullptr) session.noteVirtualSaveMeta(dest, session.hbinMetaPatch(*card));
    }
    // BUG-659: nor a char table to state the ts declaration in (see the disk
    // arm's twin below).
    session.noteCheckpointTsDecl(dest);
    session.datasetChanged = false;  // BS-091: a successful save resets c(changed).
    ojson out;
    out["type"] = "echo";
    out["text"] = "file " + effectiveFp + " saved";
    return out;
  }
  // BUG-138 (CRITICAL) + L5-IOSAVE, one change: NEVER open `dest` for write.
  //
  // This site used to be an open-coded `std::ofstream dst(dest, trunc);
  // dst << src.rdbuf();` with no check that `dest` IS `*session.currentHbin()`
  // — and after a plain `use FILE` (no *mutating* verb since) it still is. The
  // trunc-open destroyed the user's own file while `src` was still pointed at
  // it, the copy then moved 0 bytes, libc++ set failbit on the empty insert,
  // and the BUG-124 torn-write handler `remove()`d what it correctly believed
  // was a partial. Net: `use X` → `save X, replace` — the commonest Stata idiom
  // there is — DELETED X and blamed a disk that was not full.
  //
  // Adopt the shape the engine's own writer has carried for exactly this hazard
  // (op-cobra `src/data/frame.cpp:2103-2148`, the `path == src_path_` branch)
  // and that IDIOMS.md §Filesystem already prescribes ("save-in-place: write to
  // a same-dir temp + atomic rename, writer state CHECKED before the rename
  // commits"): produce the bytes in a sibling temp in the SAME directory (same
  // filesystem, so the commit cannot EXDEV), check them, then `rename(2)` the
  // temp over `dest` in one atomic step. `dest` is never opened for write at
  // all, so save-over-source is non-destructive BY CONSTRUCTION, and a failure
  // anywhere in the copy leaves the original file exactly as it was rather than
  // removed.
  //
  // The same shape carries the L5-IOSAVE perf half for free (§L5 DIAG in
  // internal note GATE23-stage0-RUN.md): `dst << src.rdbuf()` is a
  // byte-at-a-time streambuf loop, ~5.5 ns/byte and *user*-CPU bound (0.91 user
  // / 0.22 sys on the 164 MB reference corpus = ~1.10 s, ~95% of the probe
  // wall). A same-volume `clonefile(2)` into the temp is O(1) (1.10 s →
  // 0.0003 s, measured); the cross-volume fallback is a 1 MB buffered loop
  // (0.51 s, 3.5× the streambuf loop). Every arm produced byte-identical output
  // (sha256, 4 arms) ⇒ value-identical, no golden re-pin.
  //
  // NOTE (inode/symlink edge): rename() retargets the directory entry to the
  // temp's NEW inode, so another fd or hardlink holding the OLD inode keeps
  // seeing the pre-save bytes, and a `dest` that is a symlink is REPLACED
  // rather than written through. Same tradeoff `mv`, most editors, and the
  // engine's own writer already make (frame.cpp carries the identical note).
  {
    // Sibling-temp target: a hidden temp in dest's OWN directory (same FS, so
    // the commit rename cannot EXDEV). Computed before the write path is chosen
    // because BOTH the C4-L2C direct-snapshot and the copy fallback land here.
    // Split with std::filesystem, NOT find_last_of('/'): on Windows a user path
    // is normally `C:\dir\file.hbin`, which contains no '/' at all. The manual
    // scan then made destDir "." and destBase the WHOLE path, so the sibling
    // temp became `./.C:\dir\file.hbin.savetmp…` — an invalid Windows filename
    // (a ':' and separators in a leaf), which failed the save with EINVAL, and
    // would have put the temp on the wrong volume even where the name was legal.
    // parent_path()/filename() handle '\', '/', and the drive prefix, and are
    // byte-identical to the old scan on POSIX.
    const std::filesystem::path destPath = cecon::to_native_path(dest);
    const std::filesystem::path destParent = destPath.parent_path();
    std::string destDir =
        destParent.empty() ? std::string(".") : cecon::from_native_path(destParent);
    std::string destBase = cecon::from_native_path(destPath.filename());
    // Per-CALL unique (IDIOMS §Filesystem — never name-keyed, never epoch-keyed:
    // BUG-111b): pid distinguishes concurrent processes, the counter
    // distinguishes two saves from the same process to the same directory.
    static std::atomic<unsigned long long> saveTmpSeq{0};
    std::string tmp = destDir + "/." + destBase + ".savetmp" +
                      std::to_string(static_cast<long long>(cecon::native_process_id())) + "_" +
                      std::to_string(saveTmpSeq++);
    std::error_code tmpEc;
    // Clear any stale temp from a killed run BEFORE the direct snapshot too — a
    // leftover file at `tmp` would make frameSnapshot/clonefile fail on EEXIST.
    std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);

    // Mode fidelity: the old trunc-open wrote THROUGH an existing `dest`, so
    // that file kept its own permissions. rename() installs the temp's inode
    // instead — and `clonefile(2)`/frameSnapshot create the temp with their own
    // mode — so an existing destination's mode must be carried across explicitly
    // or a `save over_an_0600_file, replace` would silently re-permission it.
    // POSIX-only, and DELIBERATELY so — this was considered, not overlooked.
    // The Windows equivalent of "carry the destination's permissions" is not a
    // mode word at all, so there is nothing for a `_wchmod` to do here: the
    // CRT's `_wchmod` maps the whole of `st_mode` onto ONE bit, FILE_ATTRIBUTE_
    // _READONLY, and the real access control lives in the file's security
    // descriptor (DACL), which no POSIX call reaches.
    //
    // What the commit below actually does to that DACL (see the MoveFileExW
    // block): a same-volume MoveFileExW is a directory-entry rename, so the
    // file that ends up at `dest` keeps the TEMP's security descriptor and the
    // old destination's is destroyed with the old file. The temp is a sibling
    // in `dest`'s OWN directory (BUG-138 requires that for the EXDEV-free
    // commit), so it was created with exactly the ACEs that directory hands
    // down — which is also what a brand-new `save newfile.hbin` in that
    // directory would get. For the overwhelmingly common case (a destination
    // whose ACL is purely inherited from its directory) the before and after
    // descriptors are therefore equivalent and there is nothing to carry.
    //
    // The residual, stated honestly: if the destination carried EXPLICIT
    // (non-inherited) ACEs — someone ran `icacls dest /grant`, or the file was
    // protected with inheritance disabled — those ACEs are NOT reproduced on
    // the replacement. `save over_that_file, replace` re-permissions it to the
    // directory default. That is the same thing `mv`, and every other
    // temp-then-rename writer on Windows, does.
    //
    // Not fixed here, on purpose. The two candidate fixes both cost more than
    // the gap:
    //   * GetNamedSecurityInfoW/SetNamedSecurityInfoW (advapi32, already
    //     linked) round-trip. Correct only with ACL surgery, not a straight
    //     copy: writing back a descriptor that still contains the INHERITED_ACE
    //     entries re-materializes them as EXPLICIT ACEs on the new file, so the
    //     file stops tracking later changes to the directory's ACL — a
    //     different, quieter corruption of the thing we were trying to
    //     preserve. Doing it properly means reading SE_DACL_PROTECTED, sieving
    //     the inherited ACEs out, and rebuilding an ACL by hand, plus WRITE_DAC
    //     on the target and a decision about owner/SACL. Too much machinery,
    //     and untestable here, for the payload.
    //   * cecon::native_replace_file() (ReplaceFileW) DOES preserve the
    //     destination's DACL by contract — that is the function's reason to
    //     exist — but it requires `dest` to already exist and it has failure
    //     modes (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) that can leave the
    //     destination already deleted. That would trade an ACL nicety for the
    //     BUG-138/BUG-124 guarantee this whole block is built around, "the
    //     existing file, if any, is unchanged". Not a good trade.
#ifndef _WIN32
    struct ::stat destSt;
    bool destHadMode = (::stat(cecon::to_native_path(dest).c_str(), &destSt) == 0);
#endif

    bool ok = false;
    int failErrno = 0;
    std::string failWhy;  // Win32 replace failure text (GetLastError, not errno)

    // C4-L2C (D3, C4-RESIDENT-READ-SCOPE-brief §3 close/§4): a dirty virtual head
    // is serialized STRAIGHT into `tmp` with one frameSnapshot — no scratch
    // materialization, no copy. `tmp` is already a BUG-138 sibling temp, so the
    // rename below still makes save-over-source non-destructive. The head is left
    // virtual (a later disk consumer materializes lazily). The escape arm
    // PIET_NO_RESIDENT_READ=1 skips this so the A/B parity + seeded-RED arm
    // reproduces the pre-C4 materialize-then-copy path (and its snapshot) exactly.
    bool wroteDirect = false;
    if (!isDta && !stripMask && !piet::residentReadDisabled() &&
        session.trySaveHeadDirect(tmp) == SessionState::SaveHeadPath::WroteDirect) {
      wroteDirect = true;
      ok = true;
    }

    // P1-A3: the `.dta` plane. Neither hbin plane applies — the bytes are
    // PRODUCED by cobra's format-118 writer, not copied or snapshotted — but
    // everything around the producer is deliberately the SAME machinery: the
    // BUG-138 sibling temp, the mode carry, the checked `rename(2)` commit and
    // the "the existing file, if any, is unchanged" failure contract all hold
    // for a `.dta` exactly as they do for an `.hbin`.
    //
    // Semantics ruling (this stint, DoD ②): routing the session's own scratch
    // hbin through the writer is legitimate HERE, where the datasignature
    // surface's mirror-image argument (42nd NO-GO ④) said it was not. That
    // argument is that a scratch round-trip makes `datasignature` fingerprint
    // what the scratch WRITE produced rather than the user's session. `save` is
    // the opposite case by definition: serializing the session IS the request,
    // the scratch hbin is the session's own current bytes, and the `--in-meta`
    // sidecar carries the label/format layer the hbin cannot — so the artifact
    // is the session, losslessly, not an artifact of the plumbing.
    std::string dtaErr;
    if (isDta) {
      // cobra's writer reads `--data` from DISK (contract_A §25), so a dirty
      // virtual head must be materialized first — the same precondition, for
      // the same reason, that the copy path states below.
      session.ensureMaterializedHead("save-dta");
      const std::string srcPath = *session.currentHbin();

      // Live card for the sidecar's presence filter (name + dtype only) —
      // resident when current, else the datastore. Taken AFTER materialization
      // so the card and the file the writer reads describe the same head.
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      std::string metaPath;
      if (card != nullptr) metaPath = writeDtaMetaSidecar(session, *card, dtaErr);

      if (dtaErr.empty()) {
        // `--data`/`--out` in FLAG form, never the positional fallback: `tmp` is
        // a hidden sibling (`.name.savetmp<pid>_<n>`) and the positional parse
        // keys on "argument does not start with --", so the flags keep an
        // unusual filename from ever being read as the wrong slot.
        std::vector<std::string> args = {"export-dta", "--data", srcPath, "--out", tmp};
        if (!metaPath.empty()) {
          args.push_back("--in-meta");
          args.push_back(metaPath);
        }
        if (dtaRelease > 0 && dtaRelease != 118) {
          args.push_back("--release");
          args.push_back(std::to_string(dtaRelease));
        }
        CobraRunResult cr = runCobra(args);
        ok = cr.ok;
        if (!ok) {
          // ENGINE-CAPABILITY case, and the reason this branch exists at all:
          // the writer is NEWER than the pinned engine this build resolves by
          // default (Art 7, `releases/engine/current/cobra` — measured
          // 2026-07-31: 0.34.3 answers `export-dta` with `unknown command`).
          // Leaking that raw would tell a user their SAVE failed for reasons
          // naming a command they never typed, so it is mapped back to the
          // shape the pre-writer honest-reject had: name the cause, name the
          // format this engine CAN write, and never leave a half-file behind.
          if (cr.stderr_text.find("unknown command: export-dta") != std::string::npos) {
            const std::string suggested = rawFp.substr(0, rawFp.size() - rawExt.size()) + ".hbin";
            std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);
            if (!metaPath.empty()) {
              std::error_code metaEc;
              std::filesystem::remove(cecon::to_native_path(metaPath), metaEc);
            }
            return engineErrorBlock(
                "save: writing Stata `.dta` files needs a cobra engine with the `export-dta` "
                "writer, and the engine this build resolves to does not have it (" +
                resolveCobraBin() +
                "). Save in the native format instead: `save " + suggested +
                (replace ? ", replace" : "") + "` — it loads back with a plain `use " + suggested +
                "`. (Set COBRA_BIN to an engine that ships `export-dta` to write .dta here.)");
          }
          // Everything else is the writer's own refusal, and those are the ones
          // this surface must NOT swallow: a string over the str# cap (strL is
          // out of the writer's scope, Q4), a malformed sidecar, an unwritable
          // target. Same stderr→message mapping every other verb here uses.
          dtaErr = cobraErrorStr(errorFromRaw(cr));
          if (dtaErr.empty()) dtaErr = "export-dta failed";
        }
      }
      if (!metaPath.empty()) {
        std::error_code metaEc;
        std::filesystem::remove(cecon::to_native_path(metaPath), metaEc);
      }
    } else if (stripMask) {
      // e(sample) T2 (see stripMask above): the head minus the hidden mask,
      // written by the engine straight into the sibling temp; the commit below
      // is the same. The engine call reads the head as a file (stateless), so a
      // dirty virtual head is materialized first by the runner itself.
      const CobraRunResult cr =
          runCobra({"eval", "--data", *session.currentHbin(), "--drop", kEsampleColumn, "--out",
                    tmp});
      ok = cr.ok && cecon::native_exists(tmp);
      if (!ok) failErrno = EIO;
    } else if (!wroteDirect) {
      // Copy path: clean head, a divergent intermediate temp (currentHbin_ is
      // then already real), the escape arm, or a direct-write failure that fell
      // back. The copy reads *currentHbin() as a FILE, so a dirty virtual head
      // must be on disk first. Idempotent no-op on a clean head and on the escape
      // arm (whose entry datastore() already materialized). A FAILED
      // materialization (ENOSPC/dead worker) leaves no file at srcPath and the
      // copy reports it honestly below — "file not saved … unchanged".
      session.ensureMaterializedHead("save-entry");
      const std::string srcPath = *session.currentHbin();
#if defined(__APPLE__)
      {
        // Keep the paths alive across the call — path::c_str() is const char* here.
        std::filesystem::path nsrc = cecon::to_native_path(srcPath);
        std::filesystem::path ntmp = cecon::to_native_path(tmp);
        ok = (::clonefile(nsrc.c_str(), ntmp.c_str(), 0) == 0);
        // clonefile fails on a non-APFS FS, a cross-device pair, or an EEXIST
        // race — all handled by the buffered fallback below, which is also the
        // only path compiled on non-Apple builds.
      }
#endif
      if (!ok) {
        std::ifstream in(cecon::to_native_path(srcPath), std::ios::binary);
        std::ofstream out(cecon::to_native_path(tmp), std::ios::binary | std::ios::trunc);
        if (!in || !out) {
          failErrno = errno;
        } else {
          std::vector<char> buf(1 << 20);
          while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got > 0) out.write(buf.data(), got);
            if (!out) break;
          }
          // BUG-124: check the writer's state before committing — an ENOSPC
          // mid-copy used to leave a silently short file at the USER's
          // destination while `save` still echoed "file ... saved". Now the torn
          // bytes are confined to the temp, which never becomes `dest`.
          out.flush();
          ok = static_cast<bool>(out) && !in.bad();
          if (!ok) failErrno = errno;
        }
      }
    }
    // BUG-198: state the session's format/label overrides INTO the produced
    // bytes. This sits AFTER both hbin arms and before the commit on purpose —
    // the resident-frame snapshot and the clonefile/copy of the scratch head
    // both drop the layer (measured on both by WS-D3), and neither can be
    // taught to carry it without reaching into the worker's Frame. Patching the
    // FILE is arm-independent: whichever arm made `tmp`, one site makes it
    // honest. `.dta` is excluded because its own `--in-meta` sidecar already
    // carried this layer through cobra's writer (P1-A3).
    //
    // Cost discipline: patch_hbin_meta writes NOTHING when the header already
    // says what the session says, so the ordinary save (no overrides, or
    // overrides that came out of this very file's header on `use`) is
    // byte-identical to before this change — which is what keeps the freeze
    // corpus still.
    //
    // A failure here is a save failure, not a warning: the alternative is
    // committing a file that silently lost the metadata the user set, which is
    // the bug this whole path exists to close. `tmp` is removed and `dest` is
    // untouched, exactly like every other failure on this path.
    // The `varMeta.empty()` gate is not a micro-optimization: reading the card
    // can cost a Datastore construct (mmap + whole-file FNV) and, on a virtual
    // head, an EMH snapshot — exactly the waste C4-L2A removed from this
    // function's entry. A session that has never set a format or a label has
    // nothing to state, and answers that from an empty map.
    if (ok && !isDta && !session.varMeta.empty()) {
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      if (card != nullptr) {
        std::vector<cecon::ColMetaPatch> metaPatch = session.hbinMetaPatch(*card);
        if (!metaPatch.empty()) {
          try {
            cecon::patch_hbin_meta(tmp, metaPatch);
          } catch (const std::exception& e) {
            std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);
            return engineErrorBlock(
                std::string("save: failed writing the display-format/label layer into ") + effectiveFp +
                " (" + e.what() + ") — file not saved (the existing file, if any, is unchanged)");
          }
        }
      }
    }
    // Apply half of the mode carry captured above. POSIX-only for the reasons
    // stated there; the Windows arm intentionally has no counterpart, because
    // the sibling temp already holds the destination directory's inherited ACL.
#ifndef _WIN32
    if (ok && destHadMode) {
      ::chmod(cecon::to_native_path(tmp).c_str(), destSt.st_mode & 07777);
    }
#endif
    if (ok) {
#ifdef _WIN32
      // POSIX rename() over an existing file is not available on Windows —
      // the CRT's rename fails with EEXIST. MoveFileExW is the atomic
      // replace, and it is the only wide form, so a Korean `dest` survives.
      // WRITE_THROUGH makes the metadata commit before we report success,
      // which is the whole point of the temp-then-rename dance.
      // NOT cecon::native_replace_file(): ReplaceFileW requires the
      // destination to already exist, and `save newfile.hbin` is the common
      // case here. MOVEFILE_REPLACE_EXISTING covers both.
      //
      // Security-descriptor consequence, since the two calls differ on it and
      // the choice above is what settles it: this is a same-volume move, i.e.
      // a directory-entry rename, so the file left at `dest` carries the TEMP's
      // security descriptor — the old destination's descriptor goes away with
      // the old file. (ReplaceFileW is the call that preserves the replaced
      // file's DACL; MoveFileExW makes no such promise.) The temp is a sibling
      // in `dest`'s own directory, so its ACL is that directory's inherited
      // ACL, identical to what a newly created `dest` would carry — the visible
      // difference is confined to a destination that had explicit,
      // non-inherited ACEs. Full rationale, and why it is not compensated for,
      // at the mode-fidelity block above.
      std::filesystem::path nativeTmp = cecon::to_native_path(tmp);
      std::filesystem::path nativeDest = cecon::to_native_path(dest);
      ok = MoveFileExW(nativeTmp.c_str(), nativeDest.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
      if (!ok) {
        // GetLastError() is not an errno — carry its own message across.
        failWhy = std::error_code(static_cast<int>(GetLastError()),
                                  std::system_category())
                      .message();
      }
#else
      ok = (::rename(cecon::to_native_path(tmp).c_str(),
                     cecon::to_native_path(dest).c_str()) == 0);
      if (!ok) failErrno = errno;
#endif
    }
    if (!ok) {
      std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);  // never leave the temp behind
      // BUG-124 honesty, corrected: report the cause the OS actually gave
      // instead of asserting "disk full" (BUG-138's second half — the old text
      // named a full disk on a volume with 770 GiB free). `dest` is untouched,
      // so say that too: the user's file is still there.
      std::string why = !dtaErr.empty()   ? dtaErr
                        : !failWhy.empty() ? failWhy
                        : failErrno != 0  ? std::string(std::strerror(failErrno))
                                          : std::string("copy failed");
      return engineErrorBlock("save: failed writing " + effectiveFp + " (" + why +
                              ") — file not saved (the existing file, if any, is unchanged)");
    }
  }
  // BS-091 (BETA A4): a successful `save` resets c(changed) to false,
  // matching Stata's own "changed since load/save" semantics — see
  // session_state.h's load()/setCurrentFile() for the other two edges of
  // this dirty bit.
  session.datasetChanged = false;
  // Report-only bookkeeping for the `datastore_reloads` counter (counters.h):
  // remember that THIS session wrote this path, so a later `use` of it can be
  // told apart from a first load of a foreign file. Never gates anything.
  //
  // History, because the absence is the interesting part: B5-③ registered a
  // resident-stash memo here so the next `use` of the saved file would be a
  // 0-reload restore. It worked — datastore_reloads 12 → 0 on a save→use loop
  // — and it bought no wall at all (paired median −0.2%), because a native
  // .hbin open is an mmap + header parse with no whole-file hash, so the work
  // removed was already cheap and the added stash/unstash trips ate it.
  // Reverted on maintainer adjudication (2026-08-06, precedent A4): against zero
  // measured gain it was carrying a memo, an invalidation contract, and a
  // silent-wrong-answer hazard. The counter stayed, because knowing how often
  // a session re-reads its own output is worth knowing either way.
  session.recordSaveWrite(dest);
  // BUG-659: a native `.hbin`/`.tmp` has no char table, so the xtset/tsset
  // declaration live now is recorded beside the path for runUse to restate
  // (a `.dta` carries it in its own _dta chars via writeDtaMetaSidecar).
  if (!isDta) session.noteCheckpointTsDecl(dest);

  ojson out;
  out["type"] = "echo";
  // Report the EFFECTIVE filename (with `.dta` appended when the user gave
  // none), not the bare name they typed — the honesty requirement behind
  // this whole fix is exactly that the reported name must match the real
  // bytes on disk.
  out["text"] = "file " + effectiveFp + " saved";
  return out;
}

// ---------------------------------------------------------------------------
// rename OLDVAR NEWVAR — C++ port of mutate.py's run_rename (two-arg form
// only — wildcard/multi-var syntax is OUT of scope, matching the oracle's
// own commands.py guard).
//
// rename is pure column-metadata relabeling — it must never touch a row's
// VALUE. L1/M3 (CSV-shim retirement): this calls cobra's native `rename`
// verb (`--pairs old=new`), which renames the header cell in place and
// leaves every other column — and the renamed column's own values and
// factor dictionary — byte-identical. Numeric and string/factor columns take
// the IDENTICAL path, no dtype branch.
//
// History (superseded, kept for the honesty rationale): this was first a
// `cobra eval --gen NEW=OLD --drop OLD` value-copy that only worked for f64
// columns and leaked a raw "[internal] expr: ..." engine exception when OLD
// was string/factor (BUG B, W3 stabilization); the fix routed it through a
// whole-frame CSV/`cobra convert` round-trip (writeColumnCsvRoundTrip, since
// deleted) because piet/native then had no way to rename a header without an
// hbin writer. The native verb supersedes both — no round-trip, no
// re-factorization.
namespace {
// rename's two refusals with Stata's text and rc (w2ah_data_verbs.log
// ah_rename_exist / ah_ren_exist: rc 110, the target untouched; ah_rename_absent
// / ah_ren_absent: rc 111). The target message used to read "already exists",
// which coarseRc maps to rc 1.
ojson renameAbsent(const std::string& oldvar) {
  return rcErrorBlock("variable " + oldvar + " not found", 111);
}
ojson renameTargetExists(const std::string& newvar) {
  return rcErrorBlock("variable " + newvar + " already defined", 110);
}
}  // namespace

ojson runRename(const std::string& oldvar, const std::string& newvar, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: rename's entry does nothing but ask whether one
  // name exists and another does not — the exact question runGsort/runOrder/
  // runDrop/runKeep already answer from the resident shadow card. It was
  // answering it with session.datastore(), i.e. a full cecon::Dataset::open of
  // the current hbin (mmap + 192-column header parse). Falls back to the exact
  // prior behavior whenever the card cannot be trusted — see
  // session.shadowVarlistIfCurrent()'s doc comment (session_state.h).
  //
  // SCOPE, STATED HONESTLY. This is NOT where rename's measured ~0.19 s/call
  // goes. The dominant term is engine-side: Frame::rename sets structure_dirty_
  // (src/data/frame.cpp), which disqualifies write_hbin's header-patch path and
  // forces write_hbin_full — a complete 164 MB serialize to change one string in
  // a JSON header. That is a cobra-owned fix in a file this stint does not
  // touch; removing the wasted open here does not substitute for it and is not
  // claimed to.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    if (!cardHasName(*card, oldvar)) return renameAbsent(oldvar);
    if (cardHasName(*card, newvar)) return renameTargetExists(newvar);
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    const auto& names = ds->names();
    if (std::find(names.begin(), names.end(), oldvar) == names.end()) return renameAbsent(oldvar);
    if (std::find(names.begin(), names.end(), newvar) != names.end())
      return renameTargetExists(newvar);
  }
  // BUG C: shared validator (piet::strutil::isValidStataIdentifier, see its
  // header comment) — a rename TARGET is a new column name exactly like
  // gen's LHS, so it goes through the identical rule at the identical
  // helper (handlers.cpp::runGen / strmutate.cpp::runStringGen are the
  // other two call sites).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "rename: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // BUG-1120: `rename x _cons' is "_cons invalid varname" r(198), and so are the
  // storage types and if/in/using/with (`rename x int'); a str# is "str5 invalid
  // variable name" (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return rcErrorBlock(r->message, r->rc);

  // L1/M3: native `cobra rename` — renames the header cell in place and
  // byte-preserves every other column plus the source's own values and factor
  // dictionary (no whole-frame CSV round-trip, no re-factorization). Output
  // --out lands on a fresh scratch path under the "rename" prefix (that prefix
  // is what scratch_path_monotonic.py's rename-chain leg counts). Mirrors
  // runCollapse's own single-verb invocation machinery (runCobra +
  // errorFromRaw). Numeric and string/factor columns take the identical path.
  std::string outPath = nextPath(session.workdir(), "rename");
  // *session.currentHbin() rather than ds->path() — equivalent whenever data is
  // loaded (a Datastore is always constructed from *currentHbin_), and it is
  // what lets the card branch above skip building one at all. Same substitution
  // runGsort's own fix already made for its --data arg.
  std::vector<std::string> args = {"rename",  "--data",  *session.currentHbin(), "--out",
                                   outPath,   "--pairs", oldvar + "=" + newvar};
  {
    // P2-PERF ARC-B/B2: rename's `--out` becomes the head and is never read
    // back as a file — mutationResult renders from the shadow card, and the
    // card knows how to fold a rename (session_state.cpp CardFoldPlan::Rename).
    // That is exactly the invariant ResidentEligible asserts, and it is the
    // same opt-in gen/replace/egen/drop/keep have had all along. Without it the
    // call is a stateless mutation, and Frame::rename sets structure_dirty_, so
    // the engine re-serialises the WHOLE frame to change one string in a JSON
    // header: 18.6 ms per call, measured, on a 48,000 x 31 frame.
    SessionState::ResidentEligible residentRename(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);

  // Carry metadata: var-label/format/stata_type + value-label attachment, and
  // the type claim through the one carry rule (FU-243: an untyped generate's
  // implicit float used to stay behind under the old name, so `replace` on the
  // new name stored a double; w2ah_data_verbs.log ah_fu243_rename/ah_fu243_ren).
  storage::carryTypeClaim(session, oldvar, newvar, /*move=*/true);
  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    session.varMeta[newvar] = metaIt->second;
    session.varMeta.erase(oldvar);
  }
  if (session.userFormattedVars.erase(oldvar) != 0) session.userFormattedVars.insert(newvar);
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) {
    session.varLblname[newvar] = lblIt->second;
    session.varLblname.erase(oldvar);
  }
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) {
    session.valueLabels[newvar] = vlIt->second;
    session.valueLabels.erase(oldvar);
  }

  return mutationResult(session, "rename", {newvar}, {oldvar});
}

// ---------------------------------------------------------------------------
// clonevar NEWVAR = OLDVAR — value copy + metadata clone (BUG-003, 1.6.0).
// C++ port of dispatcher.py's clonevar handler: gen NEWVAR=OLDVAR (action
// stays "gen", not "clonevar" — the oracle's own quirk) + clone
// label/format/stata_type/value-label attachment.
//
// Numeric clonevar uses the fast `cobra eval --gen NEW=OLD` path (also the
// one place `clonevar ... if` is supported, folded into a cond()); the
// string/factor path — which `cobra eval --gen`'s numeric-only RHS cannot
// serve (it once leaked "[internal] expr: string/factor variable used where
// a number is required" on `clonevar st2 = state`, BUG-037) — now calls
// cobra's native `clonevar` verb (`--pairs old=new`, L1/M3): the clone is
// appended at the end with the source's factor dictionary SHARED, OLDVAR
// survives untouched. (History: BUG-037 first routed the string/factor path
// through a whole-frame CSV round-trip, writeColumnCsvRoundTrip, since
// deleted — the native verb supersedes it.)
//
// BS-031(c) (BETA campaign C2, silent-if-drop sweep): `ifExpr` was
// previously accepted by the tokenizer but never threaded to this handler
// at all — `clonevar inc2 = income if year==1982` silently cloned ALL 336
// rows instead of only the 48 in-sample ones (oracle-verified live this
// stint: 48 non-missing, 288 missing — clonevar is gen-class, so an
// if-excluded row's target is MISSING, same as plain `gen`). The numeric
// fast path folds `if` into cobra's own `cond()` the exact same way
// handlers.cpp::runGen already does for plain `gen ... if` — no filtering/
// merge-back needed since this is a per-row passthrough. The string/factor
// path (`cobra clonevar` is a whole-column copy with no per-row conditional)
// serves `if` through a dispatched string `generate`, clonevar.ado's own body.
//
// C4-L4B (FIX-C4L4B-RESIDUALS ①): the encode recipe, applied to the residual
// wrap class the C4-L4 capture named. clonevar's entry reads a NAME and a
// DTYPE and nothing else — no column data — so the resident card answers both
// (the D1 rule) and the datastore opens LAZILY; the engine call takes the head
// TOKEN (`*session.currentHbin()`) instead of `ds->path()` and is wrapped in
// ResidentEligible, which routes it through the frame plane. `cobra clonevar`
// rides the SAME §26 acquire_data/commit_frame bridge as `cmd_encode`
// (apps/cmds/cmds_data.cpp:365/:368 vs :693/:697 — read cross-lane, zero cobra
// change), and the numeric arm is plain `eval --gen`, the hot resident path
// itself. Escape (`PIET_NO_RESIDENT_READ` for the C4 family,
// `PIET_NO_RESIDENT_SETCOL` for the C2-specific arm) restores the pre-C4 entry
// and `ds->path()` VERBATIM, so the floor RED arm reproduces the old path with
// one env, exactly as at the destring/encode sites.
namespace {
// clonevar.ado after its `gen': the variable label, the value label and the
// format follow the source, and so does the type claim -- through the one carry
// rule (storage::carryTypeClaim, FU-243): `gen `type'' declares the source's
// type, so the clone of an untyped generate is a float that `replace` keeps
// float (w2ah_data_verbs.log ah_fu243_clone, ah_fu243_bigint).
void copyCloneMeta(SessionState& session, const std::string& oldvar, const std::string& newvar) {
  storage::carryTypeClaim(session, oldvar, newvar, /*move=*/false);
  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    const VarMeta oldMeta = metaIt->second;
    VarMeta& newMeta = session.varMeta[newvar];
    if (oldMeta.label.has_value()) newMeta.label = oldMeta.label;
    if (oldMeta.format.has_value()) newMeta.format = oldMeta.format;
  }
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) session.varLblname[newvar] = lblIt->second;
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) session.valueLabels[newvar] = vlIt->second;
}
}  // namespace

ojson runClonevar(const std::string& newvar, const std::string& oldvar, SessionState& session,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange) {
  // runSaveData's entry idiom: `datastore() == nullptr` is exactly
  // `!currentHbin().has_value()` (session_state.h), so hasData() is the same
  // question without the mmap + 192-column header parse (and, on a dirty
  // virtual head, without the EMH snapshot that open would force).
  if (piet::residentReadDisabled()) {
    if (session.datastore() == nullptr) return eval::noDataLoaded();
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  // BUG-1096: clonevar.ado reads `[if] [in]` before anything else, so a range
  // outside the data is "observation numbers out of range" r(198) first; rows
  // outside the range are missing, as for `generate ... in` (StataBE 19.5:
  // `clonevar y = x in 2/3` keeps x's label, value label and format, and
  // "(3 missing values generated)"). It was refused by name.
  std::string rangeArg;  // "a/b" in literal row numbers, "" = no `in`
  if (inRange.has_value() && !inRange->empty()) {
    InRangeBounds rr = parseInRangeBounds(*inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "clonevar");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  // S4a's rule (runEncode): an unrecognized dtype spelling is REFUSED rather
  // than guessed — the card's vocabulary is exactly Datastore::dtype()'s
  // ("factor" | "f64"), and anything else falls through to the datastore for
  // the whole answer rather than being mapped by hand.
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      const VarInfo* vi = cardFind(*card, name);
      if (vi == nullptr) return std::nullopt;  // a current card is authoritative for absence
      if (vi->dtype == "factor" || vi->dtype == "f64") return vi->dtype;
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  // BUG-1120: a reserved name is refused before anything is looked up, by
  // generate's rule -- `clonevar _cons = x' "_cons invalid varname" r(198),
  // `clonevar _skip = x' "_skip not allowed" r(101), `clonevar in = x' "'='
  // invalid observation number" r(198) (StataBE 19.5).
  if (auto r = strutil::generateNameRefusal(newvar)) return rcErrorBlock(r->message, r->rc);
  // clonevar.ado's order: `confirm new var newvar' (r(110)) before `confirm var
  // varname' (r(111)). w2ah_data_verbs.log ah_clone_exist: `clonevar cb = ci'
  // with cb present is rc 110 and cb keeps its byte values -- main replaced cb
  // with ci's values at rc 0.
  if (lookupDtype(newvar).has_value()) {
    return rcErrorBlock("variable " + newvar + " already defined", 110);
  }
  std::optional<std::string> srcDtype = lookupDtype(oldvar);
  if (!srcDtype.has_value()) {
    return rcErrorBlock("variable " + oldvar + " not found", 111);
  }
  // BUG C parity: clonevar's target is a new column name exactly like
  // gen's LHS / rename's target — this path never had the shared validator
  // wired at all before; mirror it here too (piet::strutil::
  // isValidStataIdentifier, see its header comment).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "clonevar: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // C4-L4B: the residency switch, hoisted above both arms (the destring site's
  // shape). Resident path: `--data` rides the current head token and the
  // frame-run channel strips it. Escape/stateless path: pre-C4 verbatim —
  // materialize via ensureDs() and pass the real file.
  const bool residentClone = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                             !residentReadDisabled();
  std::string dataPath;
  if (residentClone) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }

  if (*srcDtype == "f64") {
    // Numeric fast path — UNCHANGED from before this fix when no `if` is
    // given; `if` folds into a cond() exactly like plain `gen ... if`.
    std::string outPath = nextPath(session.workdir(), "gen");
    std::string genExpr = haveIf ? (newvar + "=cond((" + *ifExpr + ")," + oldvar + ",.)")
                                  : (newvar + "=" + oldvar);
    std::vector<std::string> args = {"eval", "--data", dataPath, "--gen", genExpr, "--out",
                                      outPath};
    if (!rangeArg.empty()) args.insert(args.end(), {"--gen-in", rangeArg});
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  } else {
    // String/factor path — the BUG-037 fix. BS-031(c): `cobra clonevar` is a
    // whole-column copy with no per-row conditional, so an `if` or `in` takes
    // the string generate path below instead of the verb.
    if (haveIf || !rangeArg.empty()) {
      // clonevar.ado's own body: `gen `type' newvar = varname `if'' -- a string
      // clone keeps the source's str# and is "" where the condition fails
      // (w2ah_data_verbs.log ah_clone_str_if: str12 %12s, [y1] then []). The
      // string generate path serves it; the metadata copy below runs as for
      // every clone.
      std::string typeTok;
      auto tIt = session.varMeta.find(oldvar);
      if (tIt != session.varMeta.end() && tIt->second.stataType.has_value() &&
          tIt->second.stataType->rfind("str", 0) == 0)
        typeTok = *tIt->second.stataType + " ";
      ojson genBlocks =
          dispatch("quietly generate " + typeTok + newvar + " = " + oldvar +
                       (haveIf ? " if " + *ifExpr : std::string()) +
                       (rangeArg.empty() ? std::string() : " in " + rangeArg),
                   session);
      if (genBlocks.is_array()) {
        for (const auto& b : genBlocks)
          if (b.is_object() && b.value("type", std::string()) == "error") return b;
      }
      ojson result = mutationResult(session, "gen", {newvar});
      copyCloneMeta(session, oldvar, newvar);
      return result;
    }
    // L1/M3: native `cobra clonevar` — appends the clone at the END of the
    // column order with the factor dictionary SHARED from the source (no
    // whole-frame CSV round-trip, no re-factorization); OLDVAR survives
    // untouched (a clone is a COPY, not a move). Output keeps the "gen"
    // scratch prefix (scratch_path_monotonic.py's clonevar-string leg counts
    // gen_*.hbin; the numeric fast path above shares that prefix).
    std::string outPath = nextPath(session.workdir(), "gen");
    std::vector<std::string> args = {"clonevar", "--data", dataPath, "--out",
                                     outPath,     "--pairs", oldvar + "=" + newvar};
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  }

  // dispatcher.py's clonevar handler ORDERING BUG (verified against the
  // frozen 1.16.0 oracle directly, MI-XXX not filed but reproduced
  // bug-for-bug per Doctrine): `gen_result = run_gen(...)` computes its
  // `varlist_after` (via _mutation_result -> varlist_with_meta) BEFORE the
  // metadata-clone lines run below, and the handler returns that ALREADY-
  // COMPUTED gen_result unless it was None — so the returned block's
  // varlist_after reflects the label/format/etc BEFORE cloning (label=""
  // for the new var), even though session._var_meta[newvar] itself DOES
  // end up correctly populated (a later command's varlist_after — e.g. the
  // next `describe` — WILL show the cloned label; only THIS command's own
  // return block is stale). Compute the result block first, THEN clone
  // metadata, to reproduce this exact ordering.
  ojson result = mutationResult(session, "gen", {newvar});
  copyCloneMeta(session, oldvar, newvar);
  return result;
}

// ---------------------------------------------------------------------------
// collapse (stat) varlist [(stat) varlist...] [, by(g1 g2)] [if COND] — C++
// port of dispatcher.py's _run_collapse. L1/M3 (CSV-shim retirement):
// cobra's native `collapse --by` now accepts factor by-vars directly — the
// output by-col stays factor with a fresh, self-consistent dictionary in
// first-seen output order (the same convention `convert --factor` produces,
// and what the old oracle's restore-levels dance rebuilt from scratch each
// collapse). Older cobra required all --by columns to be f64, so this
// handler used to pre-convert factor by-vars to f64 CODES via a CSV
// round-trip + egen-group() proxy and restore level text afterward; that
// whole pre/post dance is gone, by-vars pass straight through to `--by`.
namespace {
// format-display-default-lifecycle: what collapse gives each result
// variable. Its format is the source variable's; its type follows the statistic: sum, rawsum
// and percent are double; count is long (double under a weight); max, min,
// first, last, firstnm and lastnm keep the source's type; every other statistic
// is double for a double or long source and c(type) otherwise.
// w2af_format_lifecycle.log af_collapse: `(mean) dt f92 v_b (sum) sm=v_b
// (count) cn=v_b (max) mx=v_l` -> double %td, double %9.2f, float %8.0g,
// double %8.0g, long %8.0g, long %12.0g. Before, a result kept whatever its
// name held (v_b stayed byte) and a new name took no type at all.
struct CollapseResultMeta {
  std::string target;
  std::string format;
  std::optional<std::string> type;  // nullopt = no claim (unknown source)
  std::string label;                // the result's variable label (see collapseResultLabel)
};

// FU-1013 / BUG-960: collapse's clist, parsed in this ONE place. Its grammar is
// collapse.ado's own note, `name[ ]=[ ]<varname>`: `s2 = src2`, `s2= src2`,
// `s2 =src2` and `s2=src2` are the same target (w2e_float_default.log
// collapse_sum_* / collapse_count_* / collapse_max_*: `(sum) s2 = src2 bsum = bb`,
// `(count) c = src`, `(max) mx = src bmx = bb` at rc 0). The handler used to
// re-join the stat-varlist token by token, so `s2 = src2` reached the engine's
// --stat as three names ("no column: s2", rc 1). Several targets per block and
// targets beside plain variables follow from the same walk. A plain token with
// a wildcard or a range (`modal*`, `a-b`) is expanded here, because the engine
// takes names only ("no column: x*", rc 1 before).
// Refusals (collapse): a duplicate result name is "name conflict" r(198)
// (:203-212), a by() variable as a result is r(198) (bynottar), no variable at
// all is r(198) (:115). `= old` with no target and `new =` with no source are
// UNPINNED by a capture: r(198) by the task's rule (collapse_17's own path would
// reach `syntax`, whose rc for these shapes is not captured).
struct CollapseItem {
  std::string stat;    // lowercased, without the parentheses ("mean" by default)
  std::string target;  // the result variable
  std::string source;  // the source variable as written
};

struct CollapseClist {
  std::string statCobra;            // the engine's --stat: `(stat) new=old name ...`
  std::vector<CollapseItem> items;  // one per result variable, in order
  std::optional<ojson> refusal;
};

// A statistic as each flavor's parser takes it, to the engine's name.
// collapse: the listed words, SEMean /
// SEBinomial / SEPoisson by abbreviation, p1..p99; else `(avg) invalid
// statistic` r(198) (cz_c_err_stat, cz_c_err_p100, cz_c_err_p0). gcollapse
// (ParseList and the quantile check): its own words, the
// short spellings sem/seb/sep/var/skew/kurt, p# below 100 but not p0; else
// `Invalid stat` r(110) (cz_g_err_stat, cz_g_err_p100). fcollapse (ftools)
// serves no SE statistic: r(110) (cz_f_semean_g, cz_f_err_stat).
// BUG-1098: every flavor reads the statistic as typed, so (MEAN), (Mean),
// (P50) and (SEM) are r(198) in collapse and r(110) in gcollapse and
// fcollapse (StataBE 19.5; this build lower-cased them and served them). And
// fcollapse checks a quantile only against `^p[0-9]+$` (fcollapse):
// (p0) is the minimum, (p100) and past it the maximum, (p050) the median, all
// at rc 0, where collapse refuses p0/p100 r(198) and gcollapse r(110).
std::optional<std::string> canonicalCollapseStat(const std::string& raw, const std::string& flavor,
                                                 std::string* rcOut) {
  auto prefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return raw.size() >= minLen && raw.size() <= f.size() && f.compare(0, raw.size(), raw) == 0;
  };
  static const std::set<std::string> kCollapseWords = {
      "mean", "median", "sd", "sum", "rawsum", "count", "max", "min",
      "iqr", "first", "firstnm", "last", "lastnm", "percent"};
  if (flavor == "gcollapse") {
    *rcOut = "110";
    static const std::set<std::string> kGtools = {
        "sum", "nansum", "mean", "geomean", "sd", "variance", "cv", "max", "min", "range",
        "count", "median", "iqr", "percent", "first", "last", "firstnm", "lastnm", "semean",
        "sebinomial", "sepoisson", "nunique", "nmissing", "skewness", "kurtosis", "gini",
        "rawsum", "rawnansum"};
    std::string st = raw;
    if (st == "var") st = "variance";
    if (st == "sem") st = "semean";
    if (st == "seb") st = "sebinomial";
    if (st == "sep") st = "sepoisson";
    if (st == "skew") st = "skewness";
    if (st == "kurt") st = "kurtosis";
    if (kGtools.count(st)) return st;
    static const std::regex reQ(R"(^p([0-9][0-9]?(\.[0-9]+)?)$)");
    if (std::regex_match(st, reQ) && st != "p0") return st;
    return std::nullopt;
  }
  *rcOut = flavor == "fcollapse" ? "110" : "198";
  if (kCollapseWords.count(raw)) return raw;
  if (flavor == "fcollapse") {
    static const std::regex reQ(R"(^p[0-9]+$)");
    return std::regex_match(raw, reQ) ? std::optional<std::string>(raw) : std::nullopt;
  }
  if (prefixOf("semean", 3)) return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("semean");
  if (prefixOf("sebinomial", 3))
    return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("sebinomial");
  if (prefixOf("sepoisson", 3))
    return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("sepoisson");
  if (raw.size() >= 2 && raw[0] == 'p' &&
      std::all_of(raw.begin() + 1, raw.end(), [](unsigned char c) { return std::isdigit(c); })) {
    const int v = std::atoi(raw.c_str() + 1);
    if (v >= 1 && v <= 99) return raw;
  }
  return std::nullopt;
}

CollapseClist parseCollapseClist(const std::string& statRaw, const std::vector<std::string>& byVars,
                                 SessionState& session, const std::string& flavor) {
  CollapseClist out;
  auto refuse = [&](const std::string& msg) {
    out.refusal = engineErrorBlock("collapse: " + msg, "syntax_error");
    return out;
  };
  auto refuseRc = [&](const std::string& msg, const std::string& rc) {
    out.refusal = ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                         std::nullopt, "r(" + rc + ")"));
    return out;
  };
  // Tokens: a parenthesised statistic, a lone `=`, or a word.
  std::vector<std::string> toks;
  for (size_t i = 0; i < statRaw.size();) {
    const char c = statRaw[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (c == '(') {
      const size_t close = statRaw.find(')', i);
      if (close == std::string::npos)
        return refuse("`" + statRaw.substr(i) + "' -- unmatched parenthesis in the statistic");
      toks.push_back(statRaw.substr(i, close - i + 1));
      i = close + 1;
    } else if (c == '=') {
      toks.emplace_back("=");
      ++i;
    } else {
      size_t j = i;
      while (j < statRaw.size() && !std::isspace(static_cast<unsigned char>(statRaw[j])) &&
             statRaw[j] != '=' && statRaw[j] != '(')
        ++j;
      toks.push_back(statRaw.substr(i, j - i));
      i = j;
    }
  }
  auto isStat = [](const std::string& t) { return !t.empty() && t.front() == '('; };
  std::string stat = "mean";
  std::vector<std::string> parts;
  // A statistic with no variable after it reaches `syntax varlist` with an
  // empty list: "varlist required" r(100) (cz_c_err_novar `(mean), by(g)`).
  bool statOpen = false;
  size_t itemsAtStat = 0;
  for (size_t k = 0; k < toks.size(); ++k) {
    const std::string& t = toks[k];
    if (isStat(t)) {
      if (statOpen && out.items.size() == itemsAtStat) return refuseRc("varlist required", "100");
      const std::string rawStat = trim(t.substr(1, t.size() - 2));
      std::string rc;
      const std::optional<std::string> canon = canonicalCollapseStat(rawStat, flavor, &rc);
      if (!canon.has_value()) {
        // collapse names a one-word `p...` statistic by what
        // follows the p: `(100) invalid statistic`, `(xx) invalid statistic`
        // for (pxx). gtools adds a hint for (p0).
        std::string shown = rawStat;
        if (flavor == "collapse" && !shown.empty() && shown[0] == 'p' &&
            shown.find_first_of(" \t") == std::string::npos)
          shown.erase(0, 1);
        if (flavor == "gcollapse" && rawStat == "p0") shown += "; maybe you meant 'min'?";
        return refuseRc(flavor == "collapse" ? "(" + shown + ") invalid statistic"
                                             : "Invalid stat: (" + shown + ")",
                        rc);
      }
      stat = *canon;
      // gtools' sepoisson does not saturate k at 2^31 the way collapse_17's
      // builtin does (cz_g_sepoisson_g_v6c): the engine's uncapped spelling.
      parts.push_back("(" + std::string(flavor == "gcollapse" && stat == "sepoisson"
                                            ? "sepoisson_nocap" : stat) + ")");
      statOpen = true;
      itemsAtStat = out.items.size();
      continue;
    }
    if (t == "=") {
      const std::string next = k + 1 < toks.size() ? " " + toks[k + 1] : std::string();
      return refuse("`=" + next + "' has no target name; the target form is "
                    "`(stat) newvar = oldvar'");
    }
    if (k + 1 < toks.size() && toks[k + 1] == "=") {
      if (k + 2 >= toks.size() || toks[k + 2] == "=" || isStat(toks[k + 2]))
        return refuse("`" + t + " =' has no source variable; the target form is "
                      "`(stat) newvar = oldvar'");
      if (!strutil::isValidStataIdentifier(t))
        return refuse("`" + t + "' invalid name -- " + strutil::stataIdentifierRuleText());
      out.items.push_back({stat, t, toks[k + 2]});
      parts.push_back(t + "=" + toks[k + 2]);
      k += 2;
      continue;
    }
    if (t.find_first_of("*?~-") != std::string::npos) {
      auto vr = loops::expandVarlist(t, session);
      if (vr.isError) {
        out.refusal = vr.error;
        return out;
      }
      for (const std::string& nm : vr.tokens) {
        out.items.push_back({stat, nm, nm});
        parts.push_back(nm);
      }
      continue;
    }
    out.items.push_back({stat, t, t});
    parts.push_back(t);
  }
  if (statOpen && out.items.size() == itemsAtStat) return refuseRc("varlist required", "100");
  if (out.items.empty()) return refuse("no variables specified");
  for (size_t i = 0; i < out.items.size(); ++i) {
    const CollapseItem& a = out.items[i];
    for (size_t j = i + 1; j < out.items.size(); ++j) {
      const CollapseItem& b = out.items[j];
      if (a.target == b.target)
        return refuse("error: " + a.target + " = (" + a.stat + ") " + a.source + " and " +
                      b.target + " = (" + b.stat + ") " + b.source + " -- name conflict");
    }
    if (std::find(byVars.begin(), byVars.end(), a.target) != byVars.end())
      return refuse(a.target + " may not be both target and by()");
  }
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.statCobra += " ";
    out.statCobra += parts[i];
  }
  return out;
}

// The statistic as each flavor spells it in the result's variable label.
// collapse hands back `p 50` for (median) and
// `p #` for (p#), the full word for an SE abbreviation; the label is
// `(fcn) lab`: w2au_collapse_family.log cz_c_median_g_v2
// `[(p 50) xb]`, cz_c_p10_g_v2 `[(p 10) xb]`. gtools labels with the stat as
// its parser normalised it (gcollapse.ado ParseList, labelformat
// `(#stat#) #sourcelabel#` :1024): cz_g_median `[(median) byte source]`,
// cz_g_p50 `[(p50) byte source]`. ftools: sprintf("(%s) %s", stat, var)
// (fcollapse_main), the stat as typed once ParseList has taken a
// leading `raw` off it (fcollapse): (rawsum) labels `(sum) x`.
std::string collapseLabelStat(const std::string& stat, const std::string& flavor) {
  if (flavor == "collapse") {
    if (stat == "median") return "p 50";
    if (stat.size() >= 2 && stat[0] == 'p' &&
        std::all_of(stat.begin() + 1, stat.end(), [](unsigned char c) { return std::isdigit(c); }))
      return "p " + stat.substr(1);
  }
  if (flavor == "fcollapse" && stat == "rawsum") return "sum";
  return stat;
}

std::vector<CollapseResultMeta> collapseResultMeta(const std::vector<CollapseItem>& items,
                                                   const std::vector<std::string>& byVars,
                                                   const std::string& flavor, const std::string& wkind,
                                                   bool clabel, SessionState& session) {
  const bool weighted = !wkind.empty();
  std::vector<CollapseResultMeta> out;
  std::vector<VarInfo> fallbackCard;
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card == nullptr) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return out;
    fallbackCard = ds->varlist();
    card = &fallbackCard;
  }
  auto findVar = [&](const std::string& nm) -> const VarInfo* {
    for (const VarInfo& v : *card)
      if (v.name == nm) return &v;
    return nullptr;
  };
  // The source's variable label as every reader resolves it: a session
  // label over the card's own (SessionState::varlistWithMetaFrom).
  auto sourceLabel = [&](const VarInfo& v) -> std::string {
    auto it = session.varMeta.find(v.name);
    if (it != session.varMeta.end() && it->second.label.has_value()) return *it->second.label;
    return v.label;
  };
  const bool gtools = flavor == "gcollapse";
  // gcollapse's smart-target pass (gcollapse.ado parse_keep_drop):
  // a target that exists but is no source is renamed away first; then, in
  // target order, the FIRST target of each source that is not itself a target,
  // not in memory and not a by() key takes over the source's column by
  // `rename`. Such a target, like a target named as its own source, "exists",
  // and parse_ok_astarget (:1524-1613) keeps its storage when it can hold the
  // statistic. That is how (count) of a double source stays double
  // (cz_g_count_g_v6 `double|%8.0g`, cz_g_noby_v3 `n|double`) while a new
  // count target is long (or double when the source also feeds a non-count
  // statistic, the `upgrade` rule :1352-1362).
  std::vector<bool> gReuses(items.size(), false);
  std::set<std::string> gUpgrade;
  if (gtools) {
    std::set<std::string> sources, targets, mem;
    for (const CollapseItem& it : items) {
      sources.insert(it.source);
      targets.insert(it.target);
    }
    for (const VarInfo& v : *card)
      if (!(targets.count(v.name) && !sources.count(v.name))) mem.insert(v.name);
    std::set<std::string> uniqLeft = sources;
    for (size_t k = 0; k < items.size(); ++k) {
      const CollapseItem& it = items[k];
      if (it.target == it.source && mem.count(it.target)) gReuses[k] = true;
      if (!uniqLeft.count(it.source)) continue;
      uniqLeft.erase(it.source);
      if (!targets.count(it.source) && !mem.count(it.target) &&
          std::find(byVars.begin(), byVars.end(), it.source) == byVars.end()) {
        mem.erase(it.source);
        mem.insert(it.target);
        gReuses[k] = true;
      }
    }
    for (const CollapseItem& it : items)
      if (it.stat != "count" && it.stat != "freq" && it.stat != "nunique" && it.stat != "nmissing")
        gUpgrade.insert(it.source);
  }
  auto sourceType = [&](const std::string& nm) -> std::optional<std::string> {
    auto it = session.varMeta.find(nm);
    if (it != session.varMeta.end() && it->second.stataType.has_value() &&
        !it->second.stataType->empty())
      return *it->second.stataType;
    if (session.implicitFloatVars.count(nm) != 0) return std::string("float");
    return std::nullopt;
  };
  const std::string cType = storage::resolveCreateType(session, "");
  for (size_t k = 0; k < items.size(); ++k) {
    const CollapseItem& item = items[k];
    const std::string& stat = item.stat;
    std::string src = item.source;
    const size_t dot = src.rfind('.');
    if (dot != std::string::npos) src = src.substr(dot + 1);  // L.x: x's format (Setnf)
    const VarInfo* v = findVar(src);
    if (v == nullptr) continue;
    CollapseResultMeta m;
    m.target = item.target;
    m.format = session.displayFormat(*v);
    // The result's variable label. collapse: `(fcn) lab`, lab the source as
    // written (the graphbar `_percent`/`_freq`
    // sources read "percent"/"frequency"). gcollapse: `(stat) <source label,
    // else source name>` (gcollapse; cz_g_tgt_064_v4 `nobs`
    // `[(count) xi]`). By-keys keep their own label.
    {
      const std::string ls = collapseLabelStat(stat, flavor);
      if (gtools) {
        const std::string sl = sourceLabel(*v);
        m.label = "(" + ls + ") " + (sl.empty() ? item.source : sl);
      } else if (flavor == "collapse" && stat == "percent" && item.source == "_percent") {
        m.label = "percent";
      } else if (flavor == "collapse" && stat == "count" && item.source == "_freq") {
        m.label = "frequency";
      } else if (clabel) {
        // collapse's `clabel` (:407-411): `fcn of lab` (BUG-1100: StataBE 19.5
        // `mean of xf`, `p 99 of xf`, `sebinomial of xf`).
        m.label = ls + " of " + item.source;
      } else {
        m.label = "(" + ls + ") " + item.source;
      }
    }
    // gtools and ftools display a count as %8.0g (gcollapse,
    // fcollapse_main); collapse keeps the source's format.
    if (flavor != "collapse" && (stat == "count" || stat == "nunique" || stat == "nmissing"))
      m.format = "%8.0g";
    const std::optional<std::string> st = sourceType(src);
    if (v->dtype == "factor") {
      m.type = st;  // first/last of a string keep its str#
    } else if (flavor == "fcollapse") {
      // ftools' infer_type (fcollapse_main): max, min, first, last,
      // firstnm and lastnm keep the source's type, count is long, every other
      // statistic double. runCollapse then compresses each one (compress_type)
      // unless `nocompress` (BUG-1100: StataBE 19.5's fcollapse (mean) of a
      // float is a double, where this build stored the collapse rule's float).
      // A pw/iw count is long too, so a non-integer one is truncated when it is
      // stored (StataBE 19.5, ftools 2.49.0: counts 3.5 and 1.5 are byte 3 and
      // 1, long with nocompress).
      if (stat == "max" || stat == "min" || stat == "first" || stat == "last" ||
          stat == "firstnm" || stat == "lastnm")
        m.type = st;
      else
        m.type = std::string(stat == "count" ? "long" : "double");
    } else if (stat == "sum" || stat == "rawsum" || (stat == "percent" && !gtools)) {
      // gcollapse types percent by its generic rule (cz_g_percent_g float).
      m.type = std::string("double");
    } else if (stat == "count" && gtools && !weighted) {
      const bool upgrade = gUpgrade.count(item.source) != 0;
      const bool keeps = gReuses[k] && (st == std::string("long") || st == std::string("double"));
      m.type = keeps ? *st : std::string(upgrade ? "double" : "long");
    } else if (stat == "count") {
      // collapse: fw/iw/pw count is double, an aweighted one long.
      m.type = std::string(weighted && !(wkind == "aw" && !gtools) ? "double" : "long");
    } else if (stat == "max" || stat == "min" || stat == "first" || stat == "last" ||
               stat == "firstnm" || stat == "lastnm") {
      m.type = st;
    } else if (st == std::string("double") || st == std::string("long")) {
      m.type = std::string("double");
    } else {
      m.type = cType;
    }
    // FU-1033 / BUG-961: a float result is claimed float, and runCollapse
    // re-stores its values as float (one `eval --store-type float`) before the
    // claim is written, so the frame holds what the label says.
    if (m.type == std::string()) m.type.reset();
    out.push_back(std::move(m));
  }
  return out;
}

// fcollapse_main.mata compress_type, applied unless `nocompress`: the
// smallest storage type that holds a result. A byte stays; a float or double
// holding a non-integer stays; otherwise -127..100 is byte, -32767..32740 int,
// -2147483647..2147483620 long over the nonmissing values, and anything else
// (all missing included) keeps `type`. BUG-1100, StataBE 19.5: (sum) of
// integers summing to 3/7/5 is byte, to 40300 long, (count) byte, (max) of a
// float holding integers long; with nocompress double, double, long, float.
std::string fcollapseCompressedType(const double* x, int64_t n, const std::string& type) {
  if (type == "byte") return type;
  double lo = 0.0, hi = 0.0;
  bool any = false, fractional = false;
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(x[i])) continue;
    if (std::trunc(x[i]) != x[i]) fractional = true;
    lo = any ? std::min(lo, x[i]) : x[i];
    hi = any ? std::max(hi, x[i]) : x[i];
    any = true;
  }
  if (!any || ((type == "float" || type == "double") && fractional)) return type;
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return type;
}

// A result's claim after compress_type. A max/min/first/last of a source whose
// type nothing recorded has no claim; its values are compressed as a double's
// would be, and an integer type is claimed only when they all fit one.
void compressFcollapseResult(CollapseResultMeta& m, const double* x, int64_t n) {
  const std::string base = m.type.value_or("double");
  const std::string t = fcollapseCompressedType(x, n, base);
  if (m.type.has_value() || t != base) m.type = t;
}

// Whether every value of x is kept by a store as `type`: what `recast` checks
// before it changes anything (it changes nothing when one value would change).
bool valuesFitType(const double* x, int64_t n, const std::string& type) {
  for (int64_t i = 0; i < n; ++i) {
    const double v = x[i];
    if (std::isnan(v) || type == "double") continue;
    if (type == "float") {
      if (static_cast<double>(static_cast<float>(v)) != v) return false;
      continue;
    }
    const double lo = type == "byte" ? -127 : type == "int" ? -32767 : -2147483647.0;
    const double hi = type == "byte" ? 100 : type == "int" ? 32740 : 2147483620.0;
    if (std::trunc(v) != v || v < lo || v > hi) return false;
  }
  return true;
}

// fcollapse's `smart` test (fcollapse): the data's sort claim starts
// with the first by() variable's name, as text (BUG-1100, StataBE 19.5: data
// sorted by gg pass for by(g), and one observation sorted by g passes), or
// there are two observations or more and none has a first by() value below the
// one before it (a string by its bytes, a numeric missing above every number).
// Without by() fcollapse tests a constant of its own, which no sort claim
// names and which is sorted once there are two observations.
bool fcollapseSortedOnFirstBy(SessionState& session, const std::vector<std::string>& byVars) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return false;
  if (!byVars.empty()) {
    if (!ds->has(byVars[0])) return false;
    if (ds->sortedBy().rfind(byVars[0], 0) == 0) return true;
  }
  if (ds->nRows() < 2) return false;
  if (byVars.empty()) return true;
  const cecon::Column& c = ds->col(byVars[0]);
  const int64_t n = ds->nRows();
  if (ds->dtype(byVars[0]) == "factor") {
    auto text = [&](int64_t r) -> std::string {
      const int32_t code = c.codes()[r];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    };
    for (int64_t i = 1; i < n; ++i)
      if (text(i) < text(i - 1)) return false;
    return true;
  }
  if (ds->dtype(byVars[0]) != "f64") return false;
  const double* x = c.f64();
  for (int64_t i = 1; i < n; ++i)
    if (!std::isnan(x[i]) && (std::isnan(x[i - 1]) || x[i] < x[i - 1])) return false;
  return true;
}
}  // namespace

// The comma that starts collapse's options: the first one outside parentheses and
// quotes (eval::findUnquotedComma, the tokenizer's own). `raw.find(',')` cut
// `collapse ... if strpos(s, "o") > 0, by(g)` inside strpos(), and the stat list
// became `... nx=x, "o") > 0` (rc 1 `no column: x,`; w2aj_qualifiers
// qf_if_collapse_strpos).
static size_t collapseOptionComma(const std::string& raw) {
  const long long c = eval::findUnquotedComma(raw);
  return c < 0 ? std::string::npos : static_cast<size_t>(c);
}

ojson runCollapse(const std::string& rawLine, const std::optional<std::string>& ifExpr,
                  SessionState& session,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  const std::optional<std::string>& weightVar,
                  const std::optional<std::string>& inRange, const std::string& flavor,
                  const std::optional<std::string>& weightTypeIn) {
  // ONESHOT-LASTFLUSH: this handler used to open session.datastore() on entry and
  // then use nothing from it but `ds->path()`. On a virtual head that open is a
  // FULL materialization of the pre-collapse frame — 14 of the flushes left after
  // the §38 wave, and the most expensive ones in the run (the collapsed result is
  // ~5% of its input). *currentHbin() answers the same question without opening
  // anything; the same substitution runRename and runGsort already made.
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string raw = trim(rawLine);

  // BUG-042 (collapse if-misparse): `rawLine` is `parsed.raw` — the FULL,
  // untouched original command text, which still contains any `if COND`
  // clause verbatim (the tokenizer only strips `if` out of the internal
  // varlist/opts split it builds for `parsed.varlist`/`parsed.opts`; the
  // `raw` field it also carries is deliberately left whole). This handler
  // re-derives its own stat-varlist directly from `raw` via `reCollapse`
  // below, so an unstripped `if id<=4` used to leak through as bogus
  // "variable name" tokens ("if", the condition's own identifiers) reaching
  // cobra's `--stat` argument ("engine error: no column: if"). Strip the
  // SAME `if` clause back out here (mirroring tokenizer.cpp's own if-split:
  // only the portion before the first top-level comma is examined, so a
  // `by(...)` option tail is never touched) before parsing anything else.
  // `ifExpr` is already correctly threaded through to cobra's own
  // `--keepif` below — this fixes the PARSE, not the filtering itself.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitIfCollapse(R"(\s+if\s+)", std::regex::icase);
    std::smatch ifm;
    if (std::regex_search(head, ifm, reSplitIfCollapse)) {
      head = head.substr(0, static_cast<size_t>(ifm.position(0)));
    }
    raw = trim(head) + tail;
  }

  // BS-090 (BETA Phase G): `in a/b` sits in the same pre-comma head as `if`
  // (and, same as the if-strip above, would otherwise leak "in"/the range
  // token through as bogus stat-varlist entries). The `if`-strip above
  // already removes any `in` clause that happens to come AFTER `if` in the
  // raw text (it truncates the whole head at the `if` keyword), so this is
  // a no-op in that ordering — it only actually fires when `in` precedes
  // `if` (or `if` is absent). `inRange` is already threaded to
  // applyIfAndInFilter below — this fixes the PARSE, not the filtering.
  if (inRange.has_value() && !inRange->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitInCollapse(R"(\s+in\s+\S+)", std::regex::icase);
    head = std::regex_replace(head, reSplitInCollapse, "");
    raw = trim(head) + tail;
  }

  // Step-1 D3: the `[fw=w]` weight clause also sits in the pre-comma head
  // and (like the `if` above) would otherwise leak into cobra's --stat argument
  // ("engine error: no column: w]"). The weight is already threaded to cobra via
  // --fweight below, so strip the clause from the stat head here. Gated on
  // weightVar so the no-weight collapse path stays byte-identical.
  if (weightVar.has_value() && !weightVar->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    // The tokenizer's own weight grammar (eval::removeWeightClause): `[w = x]`,
    // `[cell=x]` and every other spelling tokenize() read as this weight.
    head = eval::removeWeightClause(head);
    raw = trim(head) + tail;
  }

  std::string byVal;
  {
    static const std::regex reBy(R"(\bby\s*\(\s*([^)]+)\))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(raw, m, reBy)) byVal = trim(m.str(1));
  }
  std::vector<std::string> byVars;
  {
    std::istringstream iss(byVal);
    std::string tok;
    while (iss >> tok) byVars.push_back(tok);
  }

  // stat-varlist: everything between `collapse` and the option comma (or end).
  // BUG-962: this used to stop only at `, by(`, so `, cw`, `, fast by(g)` and a
  // by()-less `, cw` left `x,` and `cw` in the stat-varlist ("no column: x,",
  // rc 1). No statistic, target or variable name holds a comma, and the `if`,
  // `in` and weight clauses were stripped above.
  static const std::regex reCollapse(R"(\bcollapse\b(.+?)(?:,|$))", std::regex::icase);
  std::smatch cm;
  if (!std::regex_search(raw, cm, reCollapse)) {
    return engineErrorBlock(
        "collapse: cannot parse stat varlist from command — expected "
        "`collapse (stat) varlist [(stat) varlist ...] [, by(varlist)]`");
  }
  std::string statRaw = trim(cm.str(1));
  if (statRaw.empty()) return engineErrorBlock("collapse: empty stat-varlist");

  // L1/M3 (CSV-shim retirement): cobra's native `collapse --by` now accepts
  // factor by-vars directly — the output by-col stays factor with a fresh,
  // self-consistent dictionary in first-seen output order (the same
  // convention `convert --factor` produces). The former workaround — dump the
  // whole frame to CSV -> convert -> materialize an egen-group() f64 proxy per
  // factor by-var -> collapse on the proxy -> restore the level text via a
  // second CSV round-trip — is gone; by-vars pass straight through to `--by`.
  //
  // The option words, read off the keys the dispatcher resolved: an accepted
  // key is the option's full name (collapse and fcollapse take no
  // abbreviation, dispatcher.cpp kOptMinCompleteLater), so an option the net
  // let through is never left unread here. The net already confined each word
  // to its flavor: clabel to collapse; nocompress, append and smart to
  // fcollapse.
  std::set<std::string> optWords;
  for (const auto& kv : opts) optWords.insert(lower(kv.first));
  const bool cw = optWords.count("cw") != 0;
  const bool fcAppend = flavor == "fcollapse" && optWords.count("append") != 0;
  // fcollapse's `smart` (fcollapse, ftools 2.49.0): on data already
  // sorted on the first by() variable it runs `collapse ..., by() fast cw`
  // instead, with collapse's checks and rules, and then ends while its own
  // `preserve` is still live -- so without `fast` the data come back as they
  // were (BUG-1100: StataBE 19.5 leaves N and k unchanged at rc 0), and with it
  // they are collapse's result. Unsorted data take the fcollapse path.
  std::string flavorRun = flavor;
  bool smartNoop = false;
  if (flavor == "fcollapse" && optWords.count("smart") != 0 && !fcAppend &&
      fcollapseSortedOnFirstBy(session, byVars)) {
    // fcollapse.ado hands collapse the weight as `[aweightw]`: its `loc eqsign
    // =` leaves eqsign empty. So any weight on this path is collapse's syntax
    // error (StataBE 19.5, ftools 2.49.0: aw, fw, iw and pw, with and without
    // fast, are `invalid syntax` r(198), and the data stay).
    if (weightVar.has_value() && !weightVar->empty())
      return ojson(eval::errorBlock("engine_error", "invalid syntax", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
    flavorRun = "collapse";
    smartNoop = optWords.count("fast") == 0;
  }
  // append reads the head as a file twice (the collapse, then the append).
  if (fcAppend) session.ensureMaterializedHead("fcollapse-append");
  std::string dataPath = *session.currentHbin();

  std::string byCsv;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) byCsv += ",";
    byCsv += byVars[i];
  }

  // The clist, parsed once (parseCollapseClist above): the engine's --stat
  // `(mean) y1 y2 (sum) z=w` -- vars stay SPACE-separated within a (stat) block
  // (cobra's own format), a target is one `new=old` token -- and the result list
  // the types, the formats, `cw` and the float re-store read.
  // The weight kind: collapse_17's `[aw fw iw pw]`, where [w=] and [weight=]
  // are aweights (the tokenizer already maps those to "aweight").
  std::string wkind;
  if (weightVar.has_value() && !weightVar->empty()) {
    const std::string wt = lower(weightTypeIn.value_or("fweight"));
    if (wt == "aw" || wt == "aweight" || wt == "weight" || wt == "w") wkind = "aw";
    else if (wt == "iw" || wt == "iweight") wkind = "iw";
    else if (wt == "pw" || wt == "pweight") wkind = "pw";
    else wkind = "fw";
  }
  auto rcError = [](const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };
  // The card: by() and every source must name a variable (`syntax ...
  // BY(varlist)` first, then each clist varlist): r(111) (cz_c_err_noby,
  // cz_c_err_nosrc, cz_c_err_nosrc_tgt).
  std::vector<VarInfo> cardCopy;
  {
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      cardCopy = *card;
    } else if (Datastore* ds = session.datastore()) {
      cardCopy = ds->varlist();
    }
  }
  auto cardVar = [&](const std::string& nm) -> const VarInfo* {
    for (const VarInfo& v : cardCopy)
      if (v.name == nm) return &v;
    return nullptr;
  };
  if (!cardCopy.empty()) {
    for (const auto& b : byVars)
      if (cardVar(b) == nullptr) return rcError("variable " + b + " not found", "111");
  }

  CollapseClist clist = parseCollapseClist(statRaw, byVars, session, flavorRun);
  if (clist.refusal.has_value()) return *clist.refusal;
  const std::string& statCobra = clist.statCobra;
  if (statCobra.empty()) return engineErrorBlock("collapse: empty stat-varlist");
  if (!cardCopy.empty()) {
    for (const CollapseItem& item : clist.items) {
      std::string src = item.source;
      const size_t dot = src.rfind('.');
      if (dot != std::string::npos) src = src.substr(dot + 1);
      const VarInfo* v = cardVar(src);
      if (v == nullptr) return rcError("variable " + src + " not found", "111");
      if (v->dtype != "factor") continue;
      // A string source: gtools refuses it outright (cz_g_str_g r(7)); collapse
      // serves first/last/firstnm/lastnm only (collapse: r(109)).
      if (flavor == "gcollapse")
        return rcError("'" + src + "' found where numeric variable expected", "7");
      if (item.stat != "first" && item.stat != "last" && item.stat != "firstnm" &&
          item.stat != "lastnm")
        return rcError("string variable " + src + " not allowed with statistic " + item.stat,
                       "109");
    }
  }
  // The weight table (collapse; gcollapse accepts the same combinations).
  if (!wkind.empty()) {
    for (const CollapseItem& item : clist.items) {
      const std::string& st = item.stat;
      const bool bad =
          (wkind == "pw" && (st == "sd" || st == "semean" || st == "sebinomial" || st == "sepoisson")) ||
          (wkind == "iw" && (st == "semean" || st == "sebinomial" || st == "sepoisson")) ||
          (wkind == "aw" && (st == "sebinomial" || st == "sepoisson"));
      if (bad) {
        const std::string wname = wkind == "pw" ? "pweights" : wkind == "iw" ? "iweights" : "aweights";
        return rcError(st + " not allowed with " + wname, "135");
      }
    }
  }

  // e(sample) T2: each result row carries its by-group's first observation's
  // marker (StataBE capture w2_esample_copies K1/K2/K4 first_diff=0, K8 = the
  // w2_esample.log:1193 E10 `0|0|3|11|`); without by() the result reads 0 (K5).
  // Read from the frame BEFORE the qualifiers pre-filter it; `restricted` = some
  // of a group's rows may not enter (if/in, a weight, `cw`).
  const bool collapseRestricted =
      (ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty()) ||
      (weightVar.has_value() && !weightVar->empty()) || cw;
  const EsampleGroupMarks esMarks = esampleGroupMarksBefore(session, byVars, collapseRestricted);

  // BUG-962: `cw` (casewise) keeps only the rows where every source variable is
  // non-missing (collapse); it was never read
  // and each statistic ran over its own variable's non-missing rows. It joins the
  // `if` qualifier of the pre-filter below.
  std::optional<std::string> filterIf = ifExpr;
  if (cw) {
    std::set<std::string> seen;
    std::string cwPred;
    for (const CollapseItem& item : clist.items) {
      if (!seen.insert(item.source).second) continue;
      if (!cwPred.empty()) cwPred += " & ";
      cwPred += "!missing(" + item.source + ")";
    }
    filterIf = (ifExpr.has_value() && !ifExpr->empty()) ? "(" + *ifExpr + ") & " + cwPred : cwPred;
  }

  // BS-090 (BETA Phase G): collapse's own cobra CLI has a --keepif of its
  // own (unlike tabstat/corr/ttest), but it runs through the SAME
  // non-bysort Frame::keep_if() as every other --keepif call, so an
  // `in`-derived `_n` predicate can't go through it either. Pre-filter
  // `dataPath` (the raw session hbin) via the shared helper and point the
  // collapse call at the result instead of forwarding a raw `--keepif`.
  if ((filterIf.has_value() && !filterIf->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, filterIf, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "collapse");
    dataPath = fr.dataPath;
    // collapse: no observation left after if/in/cw is "no
    // observations" r(2000), and the data stay as they were. The filtered copy
    // is a real file, so its row count costs no head materialization.
    // fcollapse's append keeps every observation and leaves the empty sample
    // to Mata's factor(), whose assertion is r(3498) (BUG-1100, StataBE 19.5,
    // ftools 2.49.0: `_assert_abort():  3498  no observations` for if, in and
    // cw alike, the data kept).
    int64_t rowsLeft = -1;
    try {
      rowsLeft = Datastore(dataPath).nRows();
    } catch (const std::exception&) {
      rowsLeft = -1;  // unreadable: the engine call below answers
    }
    if (rowsLeft == 0)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string(fcAppend ? "r(3498)" : "r(2000)")));
  }

  std::vector<CollapseResultMeta> resultMeta = collapseResultMeta(
      clist.items, byVars, flavorRun, wkind, optWords.count("clabel") != 0, session);
  // smart without fast: collapse's checks have run, and the data stay.
  if (smartNoop) return mutationResult(session, "collapse");

  std::string outPath = nextPath(session.workdir(), "collapse");
  std::vector<std::string> args = {"collapse", "--data", dataPath, "--stat", statCobra, "--out",
                                    outPath};
  if (!byCsv.empty()) {
    args.push_back("--by");
    args.push_back(byCsv);
  }
  // Step-1 D3: forward the frequency weight to cobra's collapse. cobra
  // owns the Σw math (and rejects what it doesn't support, e.g. weighted
  // median/p## — PIET does not pre-empt that here, it just passes through).
  if (wkind == "fw") {
    args.push_back("--fweight");
    args.push_back(*weightVar);
  } else if (!wkind.empty()) {
    args.push_back("--weight");
    args.push_back(*weightVar);
    args.push_back("--weight-type");
    args.push_back(wkind);
  }
  // Stata's `sort by`: a string by() variable's groups in text order
  // (cz_c_bystr_v1c `[]|[a]|[c]|[d]|[eee]|...`).
  args.push_back("--stata-by-order");
  {
    // ONESHOT-LASTFLUSH: collapse's `--out` becomes the head (session.load
    // immediately below) and is never read back as a file — which is exactly and
    // only what ResidentEligible asserts. The guard is scoped to THIS call, not
    // the whole handler, because applyIfAndInFilter above writes a temp that IS
    // read back as a file (collapse's own --data); eligibility is a session
    // counter, so an opt-in spanning both would silently drag the filter onto the
    // resident plane and hand cobra a --data that was never written.
    //
    // Unlike gen/replace, collapse reports card_stable=false (the row count and
    // column set both move), so mutation_result.cpp still rebuilds a Datastore
    // afterwards. That rebuild materializes the COLLAPSED head — measured at ~5%
    // of the frame this used to flush on the way in, and the engine no longer
    // reads the big one back at all.
    // fcollapse's append reads --out back as a file (the append's --using).
    std::optional<SessionState::ResidentEligible> residentCollapse;
    if (!fcAppend) residentCollapse.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      // `mark touse [wgt]`: a negative aw/fw/pw is r(402), a fractional fw r(401)
      // (cz_c_wr_fw_neg, cz_c_wr_aw_neg, cz_c_wr_fw_nonint).
      const std::string what = ce.what();
      if (what.find("has a negative value") != std::string::npos)
        return rcError("negative weights encountered", "402");
      if (what.find("has a non-integer value") != std::string::npos)
        return rcError("may not use noninteger frequency weights", "401");
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }

  // fcollapse's `append` (fcollapse_main): the results go below the
  // data instead of replacing them. The by() keys and the results fill the new
  // observations, every other variable is missing there, and nothing is sorted.
  // A result that names a variable already there is stored into it, and the
  // variable is recast to the result's (compressed) type when every value then
  // fits it (recast changes nothing otherwise); a new one is added with that
  // type, in its type's default format. Neither is relabelled or reformatted
  // (BUG-1100, StataBE 19.5: xi float %6.2f "the xi" became byte %6.2f "the
  // xi"; m double %10.0g, n byte %8.0g, mx float %9.0g, all unlabelled; an xi
  // holding .5 stays float).
  if (fcAppend) {
    if (optWords.count("nocompress") == 0) {
      Datastore res(outPath);
      for (CollapseResultMeta& m : resultMeta)
        if (res.has(m.target) && res.dtype(m.target) == "f64")
          compressFcollapseResult(m, res.col(m.target).f64(), res.nRows());
    }
    const std::string appended = nextPath(session.workdir(), "collapse");
    {
      SessionState::ResidentEligible residentAppend(session);  // the append's --out is the head
      CobraRunResult ar = runCobra({"append", "--data", *session.currentHbin(), "--using", outPath,
                                    "--out", appended, "--type-clash", "error"});
      if (!ar.ok) {
        CobraError ce = errorFromRaw(ar);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
    }
    session.load(appended);
    Datastore* ds = session.datastore();
    std::map<std::string, std::vector<std::string>> restore;  // type -> targets stored as it
    for (const CollapseResultMeta& m : resultMeta) {
      if (ds == nullptr || !ds->has(m.target) || ds->dtype(m.target) != "f64") continue;
      const double* x = ds->col(m.target).f64();
      std::optional<std::string> type = m.type;
      if (const VarInfo* before = cardVar(m.target)) {
        std::optional<std::string> was;
        auto vm = session.varMeta.find(m.target);
        if (vm != session.varMeta.end() && vm->second.stataType.has_value())
          was = *vm->second.stataType;
        else if (session.implicitFloatVars.count(m.target) != 0)
          was = std::string("float");
        if (!type.has_value()) {
          // A result whose type nothing recorded (a max/min/first/last of an
          // untyped source) has no type to recast to. The target keeps its claim
          // while its values fit it, and is widened as replace widens it when
          // they do not, never left claiming a type its values overflow.
          type = was;
          if (was.has_value() && !valuesFitType(x, ds->nRows(), *was)) {
            bool fraction = false, any = false;
            double lo = 0.0, hi = 0.0;
            for (int64_t i = 0; i < ds->nRows(); ++i) {
              if (std::isnan(x[i])) continue;
              fraction = fraction || std::trunc(x[i]) != x[i];
              lo = any ? std::min(lo, x[i]) : x[i];
              hi = any ? std::max(hi, x[i]) : x[i];
              any = true;
            }
            type = storage::promotedType(*was, fraction, lo, hi);
          }
        } else if (!valuesFitType(x, ds->nRows(), *type)) {
          type = was;
        }
        if (type != was) session.prepareStorageChange(*before);
      } else {
        VarMeta& fresh = session.varMeta[m.target];
        fresh.format.reset();
        fresh.label.reset();
      }
      if (type.has_value() && !valuesFitType(x, ds->nRows(), *type))
        restore[*type].push_back(m.target);
      session.varMeta[m.target].stataType = type;
      session.implicitFloatVars.erase(m.target);
    }
    // A value its variable cannot hold is stored as that type stores it. The
    // append's card is taken first, as before the float re-store below: left
    // pending, it was overwritten by the re-store's, and a new result that was
    // not re-stored dropped out of the varlist (`ds` and _all lost mx beside
    // re-stored counts c and c2, while c(k) still counted it).
    if (!restore.empty()) (void)mutationResult(session, "collapse");
    for (const auto& [type, targets] : restore) {
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : targets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, type);
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("fcollapse: the results were appended, but could not be stored "
                                "as " + type + ": " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
    return mutationResult(session, "collapse");
  }

  session.load(outPath);

  // FU-1033 / BUG-961: a float result (c(type) float for the mean of a non-double
  // source; a max/min/first/last of a float) holds float
  // values in Stata: w2e_float_default.log collapse_mean_float_src
  // +1.24924a0000000X+000, where the engine's double mean is 1.249249aaaaaab.
  // ONE `eval --store-type float --gen a=a ...` re-stores them through the store
  // boundary every creator uses (storage::appendStoreTypeArg -> the engine's
  // round_to_store), so there is no second rounding rule. Double and integer
  // results and the by-keys are not touched.
  {
    std::vector<std::string> floatTargets;
    for (const CollapseResultMeta& m : resultMeta)
      if (m.type == std::string("float")) floatTargets.push_back(m.target);
    if (!floatTargets.empty()) {
      // The session keeps ONE pending card delta, for the last resident commit.
      // Left pending, collapse's (a new column set) is overwritten by the
      // re-store's in-place one, which then folds onto the PRE-collapse card:
      // under `preserve` the card answered the old varlist and `: type sm` failed
      // (af_collapse, w2af_format_lifecycle.log). Take collapse's card first.
      (void)mutationResult(session, "collapse");
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : floatTargets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, "float");
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      // The same plane rule as the collapse call above: --out becomes the head.
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("collapse: the data were collapsed, but the float results could "
                                "not be stored as float: " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
  }

  // fcollapse stores each numeric result in the smallest type that holds it
  // unless `nocompress` (fcollapseCompressedType). The type is chosen from the
  // values as computed, and a value its type cannot hold (a pw/iw count of 3.5
  // in a long) is then stored as that type stores it, truncated (StataBE 19.5,
  // ftools 2.49.0: iw counts 100.75 and 1.5 are int 100 and 1).
  if (flavorRun == "fcollapse") {
    Datastore* ds = session.datastore();
    std::map<std::string, std::vector<std::string>> restore;  // type -> targets stored as it
    for (CollapseResultMeta& m : resultMeta) {
      if (ds == nullptr || !ds->has(m.target) || ds->dtype(m.target) != "f64") continue;
      const double* x = ds->col(m.target).f64();
      if (optWords.count("nocompress") == 0) compressFcollapseResult(m, x, ds->nRows());
      if (m.type.has_value() && !valuesFitType(x, ds->nRows(), *m.type))
        restore[*m.type].push_back(m.target);
    }
    if (!restore.empty()) (void)mutationResult(session, "collapse");  // as the float re-store
    for (const auto& [type, targets] : restore) {
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : targets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, type);
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("fcollapse: the data were collapsed, but the results could not "
                                "be stored as " + type + ": " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
  }

  for (const CollapseResultMeta& m : resultMeta) {
    VarMeta& vm = session.varMeta[m.target];
    vm.format = m.format;
    vm.stataType = m.type;
    vm.label = m.label;
    session.implicitFloatVars.erase(m.target);
  }
  if (!esampleGroupMarksAfter(session, esMarks, byVars, EsampleLostBy::kCollapse)) {
    return engineErrorBlock(
        "collapse: the data were collapsed, but the estimation-sample marker could not be "
        "written onto the result; e(sample) is not reliable until the next estimation.");
  }
  return mutationResult(session, "collapse");
}

// ---------------------------------------------------------------------------
// gsort [-]VAR [[-]VAR ...] — C++ port of gsort.py (parse_gsort_varlist +
// run_gsort, which delegates to editor.py's run_sort). Native cobra 0.18.0
// gsort's own error text diverges from the oracle's ("no column: X" vs
// "Variable 'X' not found.") — pre-validate here so error cells stay
// byte-identical; success path routes to cobra's native --vars "-a,b" verb.
ojson runGsort(const std::vector<std::string>& tokens, SessionState& session) {
  if (tokens.empty()) {
    return engineErrorBlock("gsort requires at least one variable");
  }
  std::vector<std::string> varnames;
  std::vector<bool> ascending;
  for (const auto& tok : tokens) {
    // [+-]?name, the name a (Unicode-permissive) identifier: bytes >= 0x80 are
    // identifier bytes (core/ident.h), an existing column is looked up byte-exactly.
    const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
    const std::string nm = signd ? tok.substr(1) : tok;
    bool ok = !nm.empty() && cecon::ident::isIdentStartByte(nm[0]);
    for (size_t i = 1; ok && i < nm.size(); ++i) ok = cecon::ident::isIdentByte(nm[i]);
    if (!ok) {
      return engineErrorBlock("gsort: invalid varspec '" + tok + "' — expected [-]varname");
    }
    varnames.push_back(nm);
    ascending.push_back(!(signd && tok[0] == '-'));
  }
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — this varlist check only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current for the loaded hbin, instead of unconditionally forcing
  // session.datastore() to reopen the file from disk. Falls back to the
  // exact prior behavior (session.datastore() + ds->has()) otherwise.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varnames) {
      if (!cardHasName(*card, v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varnames) {
      if (!ds->has(v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  }

  std::string joined;
  for (size_t i = 0; i < varnames.size(); ++i) {
    if (i) joined += ",";
    if (!ascending[i]) joined += "-";
    joined += varnames[i];
  }
  std::string outPath = nextPath(session.workdir(), "gsort");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded (Datastore is always constructed
  // from *currentHbin_, see SessionState::datastore()), and lets the fast
  // (card) branch above skip building a Datastore entirely, same choice
  // runSort's own fix already made for its --data arg.
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): single terminal row-reorder; --out never read back
  // as a file. Opt into the resident frame plane (no-op on the fallback plane).
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) return engineErrorBlock(cobraErrorStr(errorFromRaw(cr)), errorFromRaw(cr).code);
  session.load(outPath);
  return mutationResult(session, "sort");
}

// ---------------------------------------------------------------------------
// order VARLIST [, first|last|before(VAR)|after(VAR)] — C++ port of
// order.py's run_order. Pre-validates everything the oracle validates
// (anchor existence, anchor-not-in-varlist, mode exclusivity) so error text
// matches byte-for-byte, then calls cobra's native order verb for the
// actual column-permutation write.
ojson runOrder(const std::vector<std::string>& varlistIn,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session) {
  std::vector<std::string> varlist = varlistIn;
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — order's own entry validation (varlist +
  // before()/after() anchor existence, all below) only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current, instead of unconditionally forcing session.datastore() open.
  // Falls back to the exact prior behavior (session.datastore() +
  // ds->names()) whenever the card cannot be trusted.
  //
  // NOTE on scope: order's own Frame-level mutation (src/data/frame.cpp,
  // Frame::order()'s rvalue overload) unconditionally sets
  // structure_dirty_=true ("a column permutation can't be patched onto the
  // source"), so the engine reports card_stable=false on every single call
  // (apps/cecon_cli.cpp's card_stable computation) and mutationResult's own
  // post-mutation Datastore rebuild further down is NEVER skippable via the
  // resident fast path — unlike gsort/sort, whose take_rows-based Frame
  // mutation never sets that bit. This fix removes only the AVOIDABLE
  // entry-side reopen (immediately below); the unavoidable post-mutation
  // rebuild is untouched (and untouchable from this file — it lives in
  // mutation_result.cpp).
  const auto* card = session.shadowVarlistIfCurrent();
  Datastore* ds = nullptr;
  if (card == nullptr) {
    ds = session.datastore();
    if (ds == nullptr) return engineErrorBlock("No dataset loaded. Use `use` first.", "no_data_loaded");
  }
  if (varlist.empty()) return engineErrorBlock("order: variable list required");

  // BUG-105 (옵션망): order accepts only first/last/before()/after() — any
  // other residual option token (a Stata option this build doesn't
  // implement, or a typo) must be a named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOrderOpts = kVocabOrder.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnownOrderOpts)) {
    return *err;
  }

  auto hasName = [&](const std::string& n) {
    if (card != nullptr) return cardHasName(*card, n);
    const std::vector<std::string>& allNames = ds->names();
    return std::find(allNames.begin(), allNames.end(), n) != allNames.end();
  };
  // `alphabetic` (min `alpha`) sorts the named variables before moving them,
  // and `_all`/`*` name every variable (aer_w2 w2b_dataio order_alpha
  // `order *, alpha` -> big g id num s x year; order_alphabetic `order _all,
  // alphabetic`, w2b_dataio.log:809-845).
  bool alphabetic = false;
  for (const auto& [k, v] : opts)
    if (k == "alphabetic") alphabetic = true;
  {
    std::vector<std::string> all;
    if (card != nullptr) {
      for (const auto& info : *card) all.push_back(info.name);
    } else {
      all = ds->names();
    }
    std::vector<std::string> expanded;
    for (const auto& v : varlist) {
      if (v == "_all" || v == "*") {
        for (const auto& n : all)
          if (std::find(expanded.begin(), expanded.end(), n) == expanded.end()) expanded.push_back(n);
        continue;
      }
      if (std::find(expanded.begin(), expanded.end(), v) == expanded.end()) expanded.push_back(v);
    }
    varlist = std::move(expanded);
    if (alphabetic) std::sort(varlist.begin(), varlist.end());
  }
  for (const auto& v : varlist) {
    if (!hasName(v)) return engineErrorBlock("order: variable '" + v + "' not found in dataset");
  }

  bool first = false, last = false;
  bool beforeIsBool = false, afterIsBool = false;
  std::optional<std::string> beforeVar, afterVar;
  for (const auto& [k, v] : opts) {
    if (k == "first" && v.isBool && v.boolValue) first = true;
    if (k == "last" && v.isBool && v.boolValue) last = true;
    if (k == "before") {
      if (v.isBool) beforeIsBool = true;
      else beforeVar = v.strValue;
    }
    if (k == "after") {
      if (v.isBool) afterIsBool = true;
      else afterVar = v.strValue;
    }
  }
  if (beforeIsBool) {
    return engineErrorBlock("order, before(): requires a variable name, e.g. before(weight)");
  }
  if (afterIsBool) {
    return engineErrorBlock("order, after(): requires a variable name, e.g. after(make)");
  }
  if (beforeVar.has_value()) *beforeVar = trim(*beforeVar);
  if (afterVar.has_value()) *afterVar = trim(*afterVar);
  if (beforeVar.has_value() && beforeVar->empty()) beforeVar.reset();
  if (afterVar.has_value() && afterVar->empty()) afterVar.reset();

  int nModes = (first ? 1 : 0) + (last ? 1 : 0) + (beforeVar.has_value() ? 1 : 0) +
              (afterVar.has_value() ? 1 : 0);
  if (nModes > 1) {
    return engineErrorBlock("order: specify at most one of first, last, before(), after()");
  }
  if (nModes == 0) first = true;

  std::string mode = "first";
  std::string anchor;
  if (beforeVar.has_value()) {
    if (!hasName(*beforeVar)) {
      return engineErrorBlock("order, before(): variable '" + *beforeVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *beforeVar) != varlist.end()) {
      return engineErrorBlock("order, before(" + *beforeVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *beforeVar + "' would land)");
    }
    mode = "before";
    anchor = *beforeVar;
  } else if (afterVar.has_value()) {
    if (!hasName(*afterVar)) {
      return engineErrorBlock("order, after(): variable '" + *afterVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *afterVar) != varlist.end()) {
      return engineErrorBlock("order, after(" + *afterVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *afterVar + "' would land)");
    }
    mode = "after";
    anchor = *afterVar;
  } else if (last) {
    mode = "last";
  }

  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "order");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded, and lets the fast (card) branch
  // above skip building a Datastore entirely, same choice runSort's own fix
  // and this file's runGsort fix already made for their --data args.
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars", joined};
  if (mode == "first") {
    args.push_back("--first");
  } else if (mode == "last") {
    args.push_back("--last");
  } else if (mode == "before") {
    args.push_back("--before");
    args.push_back(anchor);
  } else if (mode == "after") {
    args.push_back("--after");
    args.push_back(anchor);
  }
  args.push_back("--out");
  args.push_back(outPath);
  {
    // MAXPERF Stage 2 (item 3): pure column reorder — single terminal mutation,
    // --out never read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  return mutationResult(session, "order");
}

// ---------------------------------------------------------------------------
// recode varlist (rule) [(rule) ...] [if] [in] [, generate(newvars) prefix(str)
//        label(name) copyrest test]
// recode varname rule [rule ...] [if] [in] [, ...]
// Behaviour per [D] recode (syntax, rules, options, the min/max keywords) and
// the StataBE 19.5 measurements.
// A recode is a nested conditional assignment ([D] recode, Remarks): each
// source is rewritten (in place) or copied into a new variable through one
// dispatched `replace`/`generate` whose right-hand side is a cond() chain built
// from the rules, so storage-type promotion and float storage stay those
// commands' own. The rule set is also evaluated here, row by row, wherever a
// number has to be known before or apart from the store: min/max, the change
// count r(N), `test`'s notes and the type a new variable keeps.
namespace {

// The values of one numeric column of the current head: the §38 column fetch
// when the head is resident (no whole-frame flush), else the datastore.
std::optional<std::vector<double>> headColumnValues(SessionState& session, const std::string& col) {
  std::string projPath;
  std::unique_ptr<Datastore> proj = fetchColumns(session, {col}, session.workdir(), &projPath);
  std::optional<std::vector<double>> out;
  Datastore* d = proj ? proj.get() : session.datastore();
  if (d != nullptr && d->has(col) && d->dtype(col) == "f64") {
    const double* p = d->col(col).f64();
    out = std::vector<double>(p, p + d->nRows());
  }
  if (proj) {
    proj.reset();
    cecon::native_unlink(projPath);
  }
  return out;
}

// The smallest integer type that holds every non-missing value (compress on a
// long), "" when a value is not an integer or leaves long's range.
std::string compressedIntegerType(const std::vector<double>& v) {
  double lo = 0.0, hi = 0.0;
  for (double x : v) {
    if (std::isnan(x)) continue;
    if (x != std::floor(x)) return std::string();
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo >= -127.0 && hi <= 100.0) return "byte";
  if (lo >= -32767.0 && hi <= 32740.0) return "int";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return std::string();
}

// ---- rule tokens -----------------------------------------------------------

struct RcTok {
  std::string text;
  bool quoted = false;
  bool is(const char* s) const { return !quoted && text == s; }
};

// Blank-separated words; `(`, `)`, `/` and `=` stand alone; "..." and the
// compound `"..."' are one token each, without their outer quotes.
std::optional<std::vector<RcTok>> recodeLex(const std::string& s) {
  std::vector<RcTok> out;
  std::string word;
  auto flush = [&]() {
    if (!word.empty()) out.push_back({word, false});
    word.clear();
  };
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == ' ' || c == '\t') {
      flush();
      ++i;
    } else if (c == '(' || c == ')' || c == '/' || c == '=') {
      flush();
      out.push_back({std::string(1, c), false});
      ++i;
    } else if (c == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      flush();
      // Compound quotes nest: find the matching "' .
      int depth = 1;
      size_t j = i + 2;
      while (j < s.size()) {
        if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
          ++depth;
          j += 2;
        } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
          if (--depth == 0) break;
          j += 2;
        } else {
          ++j;
        }
      }
      if (depth != 0) return std::nullopt;
      out.push_back({s.substr(i + 2, j - (i + 2)), true});
      i = j + 2;
    } else if (c == '"') {
      flush();
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) return std::nullopt;
      out.push_back({s.substr(i + 1, close - i - 1), true});
      i = close + 1;
    } else {
      word.push_back(c);
      ++i;
    }
  }
  flush();
  return out;
}

// ---- rules -----------------------------------------------------------------

// One value of a rule: a number, a missing code (kind 0 = `.`, 1..26 =
// `.a`..`.z`), or the source's min / max.
struct RcEl {
  enum Kind { Num, Miss, Min, Max } kind = Num;
  double num = 0;
  int miss = 0;
  std::string typed;
};

struct RcElement {
  RcEl lo;
  bool range = false;
  RcEl hi;
};

struct RcRule {
  std::vector<RcElement> elems;  // empty for a keyword rule
  RcEl to;
};

struct RcRuleSet {
  std::vector<RcRule> cond;
  std::optional<RcEl> elseTo, missTo, nonmissTo;
  // Rule labels in writing order: (code, text).
  std::vector<std::pair<std::string, std::string>> labels;
  bool usesMinMax = false;
};

enum class RcLabelPolicy { Allowed, InPlace, MultiNoLabel };

struct RcParseError {
  std::string msg;
};

std::optional<RcEl> recodeParseEl(const RcTok& t) {
  RcEl e;
  e.typed = t.text;
  if (t.quoted) return std::nullopt;
  const std::string& s = t.text;
  if (s == ".") {
    e.kind = RcEl::Miss;
    return e;
  }
  if (s.size() == 2 && s[0] == '.' && s[1] >= 'a' && s[1] <= 'z') {
    e.kind = RcEl::Miss;
    e.miss = s[1] - 'a' + 1;
    return e;
  }
  if (s == "min" || s == "max") {
    e.kind = s == "min" ? RcEl::Min : RcEl::Max;
    return e;
  }
  static const std::regex kNum(R"(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)");
  if (!std::regex_match(s, kNum)) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(v)) return std::nullopt;
  e.num = v;
  return e;
}

// The keyword a word spells, if any: else (`*` too), missing (at least `mis`),
// nonmissing (at least `nonm`).
std::optional<std::string> recodeKeyword(const RcTok& t) {
  if (t.quoted) return std::nullopt;
  const std::string& s = t.text;
  if (s == "else" || s == "*") return std::string("else");
  if (s.size() >= 3 && std::string("missing").compare(0, s.size(), s) == 0)
    return std::string("missing");
  if (s.size() >= 4 && std::string("nonmissing").compare(0, s.size(), s) == 0)
    return std::string("nonmissing");
  return std::nullopt;
}

class RcParser {
 public:
  RcParser(std::vector<RcTok> toks, RcLabelPolicy policy) : t_(std::move(toks)), policy_(policy) {}

  // Syntax 1: `(rule) (rule) ...`.
  void parseParenthesised(RcRuleSet& rs) {
    while (pos_ < t_.size()) {
      if (!t_[pos_].is("(")) throw RcParseError{"( expected, \"" + t_[pos_].text + "\" found"};
      ++pos_;
      parseOneRule(rs, /*paren=*/true);
    }
  }

  // Syntax 2: `elements = el elements = el ...` without parentheses.
  void parseBare(RcRuleSet& rs) {
    while (pos_ < t_.size()) parseOneRule(rs, /*paren=*/false);
  }

 private:
  const RcTok* peek() const { return pos_ < t_.size() ? &t_[pos_] : nullptr; }

  RcEl takeEl() {
    const RcTok* t = peek();
    if (t == nullptr) throw RcParseError{"unknown el  in rule"};
    std::optional<RcEl> e = recodeParseEl(*t);
    if (!e.has_value()) throw RcParseError{"unknown el " + t->text + " in rule"};
    ++pos_;
    return *e;
  }

  void noteMinMax(RcRuleSet& rs, const RcEl& e) {
    if (e.kind == RcEl::Min || e.kind == RcEl::Max) rs.usesMinMax = true;
  }

  void parseOneRule(RcRuleSet& rs, bool paren) {
    const RcTok* first = peek();
    if (first == nullptr) throw RcParseError{"unmatched open parenthesis"};
    RcRule rule;
    std::optional<std::string> keyword = recodeKeyword(*first);
    if (keyword.has_value()) {
      ++pos_;
      const RcTok* eq = peek();
      if (eq == nullptr || !eq->is("=")) throw RcParseError{"= expected"};
      ++pos_;
      if (*keyword == "else" ? rs.elseTo.has_value()
                             : (*keyword == "missing" ? rs.missTo.has_value()
                                                      : rs.nonmissTo.has_value()))
        throw RcParseError{"multiple specification of keyword " + *keyword};
      const bool isElse = *keyword == "else";
      if ((isElse && (rs.missTo.has_value() || rs.nonmissTo.has_value())) ||
          (!isElse && rs.elseTo.has_value()))
        throw RcParseError{"keywords else/* and missing/nonmissing may not be combined"};
      sawKeyword_ = true;
      rule.to = takeEl();
    } else {
      if (sawKeyword_) throw RcParseError{"unknown keyword " + first->text};
      for (;;) {
        const RcTok* t = peek();
        if (t == nullptr) {
          if (paren) throw RcParseError{"unmatched open parenthesis"};
          throw RcParseError{"invalid syntax"};
        }
        if (t->is("=")) {
          if (rule.elems.empty())
            throw RcParseError{"transformation rule without \"condition\" part"};
          ++pos_;
          break;
        }
        RcElement el;
        el.lo = takeEl();
        const RcTok* slash = peek();
        if (slash != nullptr && slash->is("/")) {
          ++pos_;
          el.range = true;
          el.hi = takeEl();
        }
        rule.elems.push_back(el);
      }
      rule.to = takeEl();
    }
    for (const RcElement& el : rule.elems) {
      noteMinMax(rs, el.lo);
      if (el.range) noteMinMax(rs, el.hi);
    }
    noteMinMax(rs, rule.to);

    if (paren) {
      const RcTok* t = peek();
      if (t == nullptr) throw RcParseError{"unmatched open parenthesis"};
      if (!t->is(")")) {
        // A label: one token, then the closing parenthesis.
        const RcTok label = *t;
        const RcTok* after = pos_ + 1 < t_.size() ? &t_[pos_ + 1] : nullptr;
        if (label.text.empty() || after == nullptr || !after->is(")"))
          throw RcParseError{") expected, \"" + label.text + "\" found"};
        pos_ += 2;
        checkLabel(rule.to);
        rs.labels.emplace_back(labelCode(rule.to), label.text);
      } else {
        ++pos_;
      }
    }
    if (keyword.has_value()) {
      if (*keyword == "else") rs.elseTo = rule.to;
      else if (*keyword == "missing") rs.missTo = rule.to;
      else rs.nonmissTo = rule.to;
    } else {
      rs.cond.push_back(std::move(rule));
    }
  }

  void checkLabel(const RcEl& to) {
    if (policy_ == RcLabelPolicy::InPlace)
      throw RcParseError{"Rules defining value labels not allowed when overwriting a variable"};
    if (policy_ == RcLabelPolicy::MultiNoLabel)
      throw RcParseError{
          "Rules defining value labels not allowed with labeled recode of multiple vars, option "
          "label() required"};
    if (to.kind == RcEl::Min || to.kind == RcEl::Max || (to.kind == RcEl::Miss && to.miss == 0))
      throw RcParseError{"Value label not allowed for missing value code ."};
    if (to.kind == RcEl::Num && to.num != std::floor(to.num))
      throw RcParseError{"Value label not allowed for noninteger " + to.typed};
  }

  static std::string labelCode(const RcEl& to) {
    if (to.kind == RcEl::Miss) return std::string(".") + static_cast<char>('a' + to.miss - 1);
    return eval::pythonIntStrFromDouble(to.num);
  }

  std::vector<RcTok> t_;
  RcLabelPolicy policy_;
  size_t pos_ = 0;
  bool sawKeyword_ = false;
};

// ---- evaluation ------------------------------------------------------------

// float(): the nearest single, and system missing from 2^127 up, where Stata's
// float range ends (StataBE: float(2e38) is `.`, so an element or to-value that
// large acts as `.` at single precision; [D] Data types).
double rcSingle(double v) {
  if (std::isnan(v)) return v;
  if (std::fabs(v) < 0x1p127) {
    const double f = static_cast<double>(static_cast<float>(v));
    if (std::fabs(f) < 0x1p127) return f;
  }
  return cecon::stata_missing(cecon::kMissSys);
}

// The rule set bound to one source: min/max resolved, and whether values are
// compared at single precision (byte/int/float sources).
struct RcBound {
  const RcRuleSet* rs = nullptr;
  bool single = false;
  double minV = 0, maxV = 0;
};

// An element bound as a comparable value: numbers first, then `.` < `.a` <
// ... < `.z`.
struct RcKey {
  bool missing = false;
  double num = 0;
  int kind = 0;
};

RcKey rcKeyOfValue(double v, bool single) {
  RcKey k;
  const double stored = single ? rcSingle(v) : v;
  if (std::isnan(stored)) {
    k.missing = true;
    k.kind = std::max(0, cecon::missing_kind(stored));
  } else {
    k.num = stored;
  }
  return k;
}

RcKey rcKeyOfEl(const RcEl& e, const RcBound& b) {
  switch (e.kind) {
    case RcEl::Miss: {
      RcKey k;
      k.missing = true;
      k.kind = e.miss;
      return k;
    }
    case RcEl::Min:
      return rcKeyOfValue(b.minV, b.single);
    case RcEl::Max:
      return rcKeyOfValue(b.maxV, b.single);
    case RcEl::Num:
      break;
  }
  return rcKeyOfValue(e.num, b.single);
}

int rcCompare(const RcKey& a, const RcKey& b) {
  if (a.missing != b.missing) return a.missing ? 1 : -1;
  if (a.missing) return a.kind < b.kind ? -1 : (a.kind > b.kind ? 1 : 0);
  return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
}

bool rcMatches(const RcElement& el, double v, const RcBound& b) {
  const RcKey x = rcKeyOfValue(v, b.single);
  if (!el.range) return rcCompare(x, rcKeyOfEl(el.lo, b)) == 0;
  return rcCompare(x, rcKeyOfEl(el.lo, b)) >= 0 && rcCompare(x, rcKeyOfEl(el.hi, b)) <= 0;
}

double rcToValue(const RcEl& e, const RcBound& b) {
  switch (e.kind) {
    case RcEl::Miss:
      return cecon::stata_missing(e.miss);
    case RcEl::Min:
      return b.minV;
    case RcEl::Max:
      return b.maxV;
    case RcEl::Num:
      break;
  }
  return e.num;
}

double rcApply(double v, const RcBound& b) {
  for (const RcRule& r : b.rs->cond)
    for (const RcElement& el : r.elems)
      if (rcMatches(el, v, b)) return rcToValue(r.to, b);
  if (b.rs->elseTo.has_value()) return rcToValue(*b.rs->elseTo, b);
  const bool miss = std::isnan(v);
  if (miss && b.rs->missTo.has_value()) return rcToValue(*b.rs->missTo, b);
  if (!miss && b.rs->nonmissTo.has_value()) return rcToValue(*b.rs->nonmissTo, b);
  return v;
}

// Whether a recode changed a value: at the source's precision (StataBE: a float
// holding .1 given the to-value .1 is no change, a long given a neighbour that
// rounds to the same float is one), missing kinds distinct.
bool rcDiffers(double before, double after, bool single) {
  const double a = single ? rcSingle(before) : before;
  const double b = single ? rcSingle(after) : after;
  const bool ma = std::isnan(a), mb = std::isnan(b);
  if (ma || mb) return ma != mb || cecon::missing_kind(a) != cecon::missing_kind(b);
  return a != b;
}

// ---- the dispatched expression ----------------------------------------------

std::string rcNumLiteral(double v) {
  const std::string s = eval::pythonFloatRepr(v);
  return v < 0 ? "(" + s + ")" : s;
}

std::string rcMissLiteral(int kind) {
  return kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + kind - 1);
}

std::string rcKeyLiteral(const RcKey& k) {
  return k.missing ? rcMissLiteral(k.kind) : rcNumLiteral(k.num);
}

// The condition one element tests, on source `x`. Numbers compare at the
// source's precision (`float(x)` against bounds already rounded to single for
// byte/int/float); missing codes compare exactly on `x` itself.
std::string rcElementCond(const RcElement& el, const std::string& x, const RcBound& b) {
  const std::string X = b.single ? "float(" + x + ")" : x;
  const RcKey lo = rcKeyOfEl(el.lo, b);
  if (!el.range) {
    if (lo.missing) return "(" + x + "==" + rcKeyLiteral(lo) + ")";
    return "(" + X + "==" + rcKeyLiteral(lo) + ")";
  }
  const RcKey hi = rcKeyOfEl(el.hi, b);
  if (!lo.missing && !hi.missing)
    return "(" + X + ">=" + rcKeyLiteral(lo) + "&" + X + "<=" + rcKeyLiteral(hi) + ")";
  if (lo.missing && hi.missing)
    return "(" + x + ">=" + rcKeyLiteral(lo) + "&" + x + "<=" + rcKeyLiteral(hi) + ")";
  if (lo.missing) return "(0)";  // a missing lower bound above a number: matches nothing
  return "((" + x + "<.&" + X + ">=" + rcKeyLiteral(lo) + ")|(" + x + ">=.&" + x +
         "<=" + rcKeyLiteral(hi) + "))";
}

std::string rcToLiteral(const RcEl& e, const RcBound& b) {
  const double v = rcToValue(e, b);
  if (std::isnan(v)) return rcMissLiteral(std::max(0, cecon::missing_kind(v)));
  return rcNumLiteral(v);
}

std::string rcExpression(const std::string& x, const RcBound& b) {
  std::string fallback;
  if (b.rs->elseTo.has_value()) {
    fallback = rcToLiteral(*b.rs->elseTo, b);
  } else if (b.rs->missTo.has_value() || b.rs->nonmissTo.has_value()) {
    fallback = "cond(" + x + ">=.," +
               (b.rs->missTo.has_value() ? rcToLiteral(*b.rs->missTo, b) : x) + "," +
               (b.rs->nonmissTo.has_value() ? rcToLiteral(*b.rs->nonmissTo, b) : x) + ")";
  } else {
    fallback = x;
  }
  std::string expr = fallback;
  for (auto it = b.rs->cond.rbegin(); it != b.rs->cond.rend(); ++it) {
    std::string c;
    for (const RcElement& el : it->elems) {
      if (!c.empty()) c += "|";
      c += rcElementCond(el, x, b);
    }
    expr = "cond(" + c + "," + rcToLiteral(it->to, b) + "," + expr + ")";
  }
  return expr;
}

// The storage type `: type` reports for a numeric source.
std::string rcSourceType(const SessionState& session, const std::string& v) {
  auto it = session.varMeta.find(v);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      storage::isNumericType(*it->second.stataType))
    return *it->second.stataType;
  if (session.implicitFloatVars.count(v) != 0) return "float";
  return "double";
}

// Whether a value stored at `created` precision survives a change to `target`
// unaltered ([D] recast without force; integer ranges per [D] Data types).
bool rcFitsType(double v, const std::string& target) {
  if (std::isnan(v)) return true;
  if (target == "double") return true;
  if (target == "float") return rcSingle(v) == v;
  if (v != std::floor(v)) return false;
  if (target == "byte") return v >= -127 && v <= 100;
  if (target == "int") return v >= -32767 && v <= 32740;
  if (target == "long") return v >= -2147483647.0 && v <= 2147483620.0;
  return false;
}

double rcStore(double v, const std::string& created) {
  return (created == "float" && !std::isnan(v)) ? rcSingle(v) : v;
}

// The variable label of a new variable: "RECODE of SRC (SRCLABEL)", at most
// 80 bytes, never cut inside a UTF-8 character.
std::string rcNewVarLabel(const SessionState& session, const std::string& src) {
  std::string text = "RECODE of " + src;
  auto it = session.varMeta.find(src);
  if (it != session.varMeta.end() && it->second.label.has_value() && !it->second.label->empty())
    text += " (" + *it->second.label + ")";
  if (text.size() > 80) {
    size_t cut = 80;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
  }
  return text;
}

std::string rcQuote(const std::string& s) {
  if (s.find('"') == std::string::npos) return "\"" + s + "\"";
  return "`\"" + s + "\"'";
}

// The first error block among a dispatch's returned blocks.
std::optional<ojson> rcFirstError(const ojson& blocks) {
  if (blocks.is_array()) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
  } else if (blocks.is_object() && blocks.value("type", std::string()) == "error") {
    return blocks;
  }
  return std::nullopt;
}

}  // namespace

ojson runRecode(const std::string& rawLine, SessionState& session,
                const std::optional<std::string>& ifExpr) {
  eval::ParsedCommand parsed;
  try {
    parsed = eval::tokenize(rawLine);
  } catch (const eval::TokenizerError& e) {
    return ojson(e.block);
  }
  eval::canonicalizeForCommand(parsed.opts, "recode");
  if (!session.hasData()) return eval::noDataLoaded();

  // ---- varlist and rule text (§ syntax 1 / syntax 2) ----
  const std::string args = trim(parsed.argText);
  if (!args.empty() && args[0] == '(') return rcErrorBlock("too few variables specified", 102);
  const size_t paren = args.find('(');
  const size_t eqPos = args.find('=');
  const bool parenthesised = paren != std::string::npos && (eqPos == std::string::npos || eqPos > paren);
  std::string varText, ruleText;
  if (parenthesised) {
    varText = args.substr(0, paren);
    ruleText = args.substr(paren);
  } else {
    const size_t sp = args.find_first_of(" \t");
    varText = sp == std::string::npos ? args : args.substr(0, sp);
    ruleText = sp == std::string::npos ? std::string() : args.substr(sp + 1);
  }
  ruleText = trim(ruleText);

  std::vector<std::string> sources;
  {
    std::istringstream ws(varText);
    std::string w;
    while (ws >> w) {
      if (w.find_first_of("*?~-") != std::string::npos) {
        auto vr = loops::expandVarlist(w, session);
        if (vr.isError) return vr.error;
        for (auto& n : vr.tokens) sources.push_back(n);
      } else {
        std::vector<std::string> one{w};
        if (auto err = resolveByKeyNames(session, one)) return *err;
        sources.push_back(one.front());
      }
    }
  }
  if (sources.empty()) return rcErrorBlock("varlist required", 100);
  if (!parenthesised && sources.size() > 1) return rcErrorBlock("too many variables specified", 103);

  const std::vector<VarInfo>* card = session.residentReadCard();
  auto dtypeOf = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      const VarInfo* vi = cardFind(*card, name);
      if (vi == nullptr) return std::nullopt;
      if (vi->dtype == "factor" || vi->dtype == "f64") return vi->dtype;
    }
    Datastore* d = session.datastore();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  for (const std::string& s : sources) {
    std::optional<std::string> dt = dtypeOf(s);
    if (!dt.has_value()) return rcErrorBlock("variable " + s + " not found", 111);
    if (*dt != "f64") return rcErrorBlock("recode only allows numeric variables", 108);
  }
  if (ruleText.empty()) return rcErrorBlock("rules expected", 198);

  // ---- options ----
  std::optional<std::string> genOpt, intoOpt, labelOpt, prefixOpt;
  bool copyrest = false, test = false;
  for (const auto& [k, v] : parsed.opts) {
    const bool flagOption = k == "copyrest" || k == "test";
    const bool nameOption = k == "generate" || k == "into" || k == "label" || k == "prefix";
    // StataBE: `gen` with no parentheses and `test(3)` are r(198); an empty
    // `gen()` is the same as not giving the option.
    if (nameOption && v.isBool) return rcErrorBlock("option " + k + " incorrectly specified", 198);
    if (flagOption && !v.isBool) return rcErrorBlock("option " + k + "() incorrectly specified", 198);
    const std::string text = v.isBool ? std::string() : trim(v.strValue);
    if (k == "copyrest") copyrest = v.boolValue;
    else if (k == "test") test = v.boolValue;
    else if (text.empty()) continue;
    else if (k == "generate") genOpt = text;
    else if (k == "into") intoOpt = text;
    else if (k == "label") labelOpt = text;
    else if (k == "prefix") prefixOpt = text;
  }
  std::vector<std::string> newNames;
  if (prefixOpt.has_value()) {
    if (genOpt.has_value() || intoOpt.has_value())
      return rcErrorBlock("option prefix() may not be combined with generate() or into()", 198);
    for (const std::string& s : sources) {
      const std::string nm = *prefixOpt + s;
      if (!strutil::isValidStataIdentifier(nm))
        return rcErrorBlock(nm + " invalid variable name", 198);
      if (dtypeOf(nm).has_value()) return rcErrorBlock("variable " + nm + " already exists", 110);
      newNames.push_back(nm);
    }
  } else if (genOpt.has_value() || intoOpt.has_value()) {
    if (genOpt.has_value() && intoOpt.has_value())
      return rcErrorBlock("options generate() and into() are synonyms", 198);
    std::istringstream ns(genOpt.has_value() ? *genOpt : *intoOpt);
    std::string nm;
    while (ns >> nm) {
      if (dtypeOf(nm).has_value()) return rcErrorBlock("variable " + nm + " already defined", 110);
      if (!strutil::isValidStataIdentifier(nm)) return rcErrorBlock(nm + " invalid name", 198);
      newNames.push_back(nm);
    }
    if (newNames.size() != sources.size())
      return rcErrorBlock("the number of new and transformed varnames should be the same", 198);
  }
  {
    std::set<std::string> seen;
    for (const std::string& nm : newNames)
      if (!seen.insert(nm).second)
        return rcErrorBlock("new variable " + nm + " mentioned more than once", 110);
  }
  const bool inPlace = newNames.empty();
  if (labelOpt.has_value()) {
    if (inPlace) return rcErrorBlock("option label() allowed only with generate()", 198);
    if (!strutil::isValidStataIdentifier(*labelOpt))
      return rcErrorBlock(*labelOpt + " invalid name", 7);
  }
  const RcLabelPolicy policy = inPlace ? RcLabelPolicy::InPlace
                               : (sources.size() > 1 && !labelOpt.has_value())
                                   ? RcLabelPolicy::MultiNoLabel
                                   : RcLabelPolicy::Allowed;

  // ---- rules ----
  RcRuleSet rules;
  {
    std::optional<std::vector<RcTok>> toks = recodeLex(ruleText);
    // A rule-grammar error leaves r() cleared: nothing is posted, so every
    // r() name reads as missing.
    auto clearR = [&]() {
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "recode", {});
    };
    if (!toks.has_value()) {
      clearR();
      return rcErrorBlock("unmatched quote in rule", 198);
    }
    RcParser parser(std::move(*toks), policy);
    try {
      if (parenthesised) parser.parseParenthesised(rules);
      else parser.parseBare(rules);
    } catch (const RcParseError& e) {
      clearR();
      return rcErrorBlock(e.msg, 198);
    }
  }

  // ---- the sample ----
  const std::optional<std::string> ifText =
      ifExpr.has_value() && !trim(*ifExpr).empty() ? ifExpr : parsed.ifExpr;
  const bool haveIf = ifText.has_value() && !trim(*ifText).empty();
  std::string rangeArg;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *parsed.inRange, "recode");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
  }
  // A qualified sample is marked once, before anything changes (a later
  // source's condition must not see an earlier source's recode).
  std::string marker;
  struct MarkerDrop {
    SessionState& s;
    std::string& name;
    ~MarkerDrop() {
      if (!name.empty()) dispatch("quietly drop " + name, s);
    }
  } markerDrop{session, marker};
  if (haveIf || !rangeArg.empty()) {
    for (int k = 0;; ++k) {
      std::string cand = "__recode_touse" + std::to_string(k);
      if (!dtypeOf(cand).has_value()) {
        marker = cand;
        break;
      }
    }
    std::string line = "quietly generate byte " + marker + " = 1";
    if (haveIf) line += " if (" + *ifText + ")";
    if (!rangeArg.empty()) line += " in " + rangeArg;
    ojson blocks = dispatch(line, session);
    if (auto err = rcFirstError(blocks)) {
      // Nothing was created; the marker name must not be dropped.
      if (!dtypeOf(marker).has_value()) marker.clear();
      return *err;
    }
    card = session.residentReadCard();
  }
  const std::string inSample = marker.empty() ? std::string() : "(" + marker + "<.)";
  std::optional<std::vector<bool>> sampleMask;  // read on demand
  auto sampleRows = [&]() -> const std::vector<bool>* {
    if (marker.empty()) return nullptr;  // every observation
    if (!sampleMask.has_value()) {
      std::optional<std::vector<double>> m = headColumnValues(session, marker);
      if (!m.has_value()) return nullptr;
      std::vector<bool> mask(m->size());
      for (size_t i = 0; i < m->size(); ++i) mask[i] = !std::isnan((*m)[i]);
      sampleMask = std::move(mask);
    }
    return &*sampleMask;
  };

  const std::string created = storage::resolveCreateType(session, "");
  double lastChanges = 0;
  bool overlapNote = false, unusedNote = false;
  // Every source is recoded from its values as they were when the command
  // started: a variable named twice is counted twice from those values and
  // rewritten once (StataBE: `recode w1 w1 (3=4)` reports the same change
  // count twice).
  std::map<std::string, std::vector<double>> startValues;
  std::set<std::string> rewritten;

  for (size_t si = 0; si < sources.size(); ++si) {
    const std::string& x = sources[si];
    const std::string srcType = rcSourceType(session, x);
    RcBound bound;
    bound.rs = &rules;
    bound.single = srcType == "byte" || srcType == "int" || srcType == "float";

    // Whether the new variable can take the source's type without reading
    // the data: every to-value fits it, and a pass-through value is the
    // source's own (so it fits unless float storage can move a long).
    std::string target;
    bool fitKnown = true, fits = true;
    if (!inPlace) {
      target = srcType;
      if (target != created && target != "double") {
        if (target == "long" && created == "float") {
          fitKnown = false;
        } else {
          auto toFits = [&](const RcEl& e) {
            return e.kind != RcEl::Num || rcFitsType(rcStore(e.num, created), target);
          };
          bool all = true;
          for (const RcRule& r : rules.cond) all = all && toFits(r.to);
          for (const auto* k : {&rules.elseTo, &rules.missTo, &rules.nonmissTo})
            if (k->has_value()) all = all && toFits(**k);
          if (!all) fitKnown = false;
        }
      }
    }

    // r(N) is the last source's count. In place: the changes. With a new
    // variable: the changes when its type is already the source's, else the
    // values that kept it from taking that type (0 when it did). StataBE
    // differential, oracle/cr_recode_fuzz.
    const bool countChanges =
        inPlace || (si + 1 == sources.size() && target == created);
    const bool needData = countChanges || test || rules.usesMinMax || !fitKnown;
    std::vector<double> values;
    const std::vector<bool>* mask = nullptr;
    bool transform = true;
    if (needData) {
      auto known = startValues.find(x);
      if (known != startValues.end()) {
        values = known->second;
      } else {
        std::optional<std::vector<double>> got = headColumnValues(session, x);
        if (!got.has_value())
          return engineErrorBlock("recode: could not read variable " + x);
        values = std::move(*got);
        if (inPlace) startValues[x] = values;
      }
      mask = sampleRows();
      if (mask == nullptr && !marker.empty())
        return engineErrorBlock("recode: could not read the if/in sample");
      if (rules.usesMinMax) {
        bool any = false;
        for (size_t r = 0; r < values.size(); ++r) {
          if (mask != nullptr && !(*mask)[r]) continue;
          const double v = values[r];
          if (std::isnan(v)) continue;
          if (!any) {
            bound.minV = bound.maxV = v;
            any = true;
          } else {
            bound.minV = std::min(bound.minV, v);
            bound.maxV = std::max(bound.maxV, v);
          }
        }
        // [D] recode: with no value to take min/max from, the source is not
        // transformed at all.
        transform = any;
      }
    }

    double changes = 0;
    if (needData && transform) {
      size_t nElems = 0;
      for (const RcRule& r : rules.cond) nElems += r.elems.size();
      std::vector<char> used(nElems, 0);
      for (size_t r = 0; r < values.size(); ++r) {
        if (mask != nullptr && !(*mask)[r]) continue;
        const double v = values[r];
        if (rcDiffers(v, rcApply(v, bound), bound.single)) changes += 1;
        if (test) {
          size_t idx = 0, hits = 0;
          for (const RcRule& rule : rules.cond)
            for (const RcElement& el : rule.elems) {
              if (rcMatches(el, v, bound)) {
                used[idx] = 1;
                ++hits;
              }
              ++idx;
            }
          if (hits > 1) overlapNote = true;
        }
      }
      if (test)
        for (char u : used)
          if (!u) unusedNote = true;
    }

    if (inPlace) {
      lastChanges = changes;
      if (!transform || !rewritten.insert(x).second) continue;
      std::string line = "quietly replace " + x + " = " + rcExpression(x, bound);
      if (!inSample.empty()) line += " if " + inSample;
      ojson blocks = dispatch(line, session);
      if (auto err = rcFirstError(blocks)) return *err;
      card = session.residentReadCard();
      continue;
    }

    // ---- a new variable ----
    const std::string& nv = newNames[si];
    std::string rhs;
    if (!transform) {
      rhs = x;  // nothing to take min/max from: a copy of the source everywhere
    } else if (inSample.empty()) {
      rhs = rcExpression(x, bound);
    } else {
      rhs = "cond(" + inSample + "," + rcExpression(x, bound) + "," + (copyrest ? x : ".") + ")";
    }
    ojson blocks = dispatch("quietly generate " + nv + " = " + rhs, session);
    if (auto err = rcFirstError(blocks)) return *err;
    card = session.residentReadCard();

    double misfits = 0;
    if (!fitKnown) {
      for (size_t r = 0; r < values.size(); ++r) {
        const bool in = mask == nullptr || (*mask)[r];
        double v;
        if (!transform) v = values[r];
        else if (in) v = rcApply(values[r], bound);
        else if (copyrest) v = values[r];
        else continue;
        if (!rcFitsType(rcStore(v, created), target)) misfits += 1;
      }
      fits = misfits == 0;
    }
    lastChanges = target == created ? changes : misfits;
    if (target != created && fits) {
      VarMeta& vm = session.varMeta[nv];
      vm.stataType = target;
      vm.format = storage::stataDefaultFormat(created);
      session.implicitFloatVars.erase(nv);
    }
    session.varMeta[nv].label = rcNewVarLabel(session, x);
  }

  // ---- value labels from the rules ----
  if (!inPlace && !rules.labels.empty()) {
    const std::string set = labelOpt.has_value() ? *labelOpt : newNames.front();
    std::map<std::string, std::string> latest;  // later rule wins
    std::vector<std::string> order;
    for (const auto& [code, text] : rules.labels) {
      if (latest.find(code) == latest.end()) order.push_back(code);
      latest[code] = text;
    }
    std::string line = "quietly label define " + set;
    for (const std::string& code : order) line += " " + code + " " + rcQuote(latest[code]);
    line += ", modify";
    ojson blocks = dispatch(line, session);
    if (auto err = rcFirstError(blocks)) return *err;
    for (const std::string& nv : newNames) {
      ojson vb = dispatch("quietly label values " + nv + " " + set, session);
      if (auto err = rcFirstError(vb)) return *err;
    }
  }

  // Drop the sample marker before the result card is taken.
  if (!marker.empty()) {
    ojson db = dispatch("quietly drop " + marker, session);
    marker.clear();
    if (auto err = rcFirstError(db)) return *err;
  }

  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", lastChanges);
  eval::declareRPosted(session.eval, "recode", {"N"});

  ojson result = inPlace ? mutationResult(session, "recode") : mutationResult(session, "recode", newNames);
  if (!test) return result;
  ojson out = ojson::array();
  out.push_back(result);
  auto note = [](const std::string& text) {
    ojson b;
    b["type"] = "display";
    b["class"] = "note";
    b["text"] = text;
    return b;
  };
  if (overlapNote) out.push_back(note("(transformation rules \"overlapped\")"));
  if (unusedNote) out.push_back(note("(one or more transformation rules did not match any values)"));
  return out;
}

// ---------------------------------------------------------------------------
// duplicates report|drop|tag|list [VARLIST] — C++ port of duplicates.py.
// report/drop/tag route to cobra's native `duplicates` verb; `list` (no
// cobra native equivalent) is built from `duplicates tag` + a keepif filter
// on the tag column (group_size-1 > 0 <=> duplicate).

namespace {

// _resolve_varlist: varlist or all names; validates each var exists
// ("variable `{v}' not found" — backtick-quote, matching Python's f-string).
std::optional<std::string> resolveVarlist(Datastore* ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          std::vector<std::string>& outCols) {
  if (!varlist.has_value() || varlist->empty()) {
    outCols = ds->names();
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    if (!ds->has(v)) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}

// GA sweep 2026-08-22. The card-answered twin. Everything the Datastore version
// asks of the frame is METADATA — the column names, and whether a name exists —
// which the resident shadow card holds exactly, so a caller does not have to
// materialize the whole virtual head to ask it. Same idiom and the same safety
// rule as joinby's INSPECT-2608 fix and runSort's varlist check: the card answers
// only when it provably describes the current head, and a nullptr card means the
// caller falls back to the byte-identical Datastore path above.
//
// Measured cause (census47h, caller-attributed materialization log): of the 962
// head flushes triggered by a `datastore()` call in one 47-task run,
// runDuplicatesDrop was 320 — and its entry call existed only to resolve a
// varlist. Its TAIL flush was already removed in the 08-20 phase-2 work; this is
// the entry half the same census named.
std::optional<std::string> resolveVarlistFromCard(const std::vector<VarInfo>* card,
                                                  const std::optional<std::vector<std::string>>& varlist,
                                                  std::vector<std::string>& outCols) {
  if (card == nullptr) return std::string("__no_card__");  // sentinel: caller falls back
  if (!varlist.has_value() || varlist->empty()) {
    outCols.clear();
    outCols.reserve(card->size());
    for (const auto& vi : *card) outCols.push_back(vi.name);
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    bool found = false;
    for (const auto& vi : *card) if (vi.name == v) { found = true; break; }
    if (!found) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}

// duplicates.ado is rclass: every subcommand returns r(N), the observations it
// considered (`qui count if `touse''), report adds r(unique_value) and drop
// r(N_drop); none of them is posted when there is nothing to consider, which
// is `error 2000'. w2ah_data_verbs.log ah_dup_report_all (N=8, unique_value=7),
// ah_dup_list, ah_dup_tag, ah_dup_drop_all (N=8, N_drop=1), ah_dup_drop_none
// (N_drop=0). Main posted none of them, so every read was a named r(111).
// duplicates.ado's tag: `gen long g = Ngroup - 1 if touse' then `compress g' --
// the smallest integer type of the counts, with long's %12.0g kept
// (w2ah_data_verbs.log ah_dup_tag: byte %12.0g; main left no claim, so `: type'
// read float and the format %9.0g).
void recordDuplicatesTagType(SessionState& session, const std::string& genvar) {
  auto vals = headColumnValues(session, genvar);
  const std::string t = vals.has_value() ? compressedIntegerType(*vals) : std::string();
  session.implicitFloatVars.erase(genvar);
  VarMeta& vm = session.varMeta[genvar];
  vm.stataType = t.empty() ? std::string("long") : t;
  vm.format = std::string("%12.0g");
}

std::optional<ojson> postDuplicatesR(SessionState& session, int64_t n,
                                     std::initializer_list<std::pair<const char*, double>> more) {
  if (n <= 0) return rcErrorBlock("no observations", 2000);
  session.eval.rValues.set("N", static_cast<double>(n));
  std::vector<std::string> names = {"N"};
  for (const auto& [k, v] : more) {
    session.eval.rValues.set(k, v);
    names.emplace_back(k);
  }
  eval::declareRPosted(session.eval, "duplicates", names);
  return std::nullopt;
}

}  // namespace

ojson runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  // BUG-042: `duplicates report ... if COND` — cobra's `duplicates` verb has
  // no --keepif of its own, so pre-filter via the same row-filter infra
  // summarize/count already use (`cobra eval --keepif` into a temp hbin,
  // then point the report at the filtered copy instead of the live data).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too
  // (piet::handlers::applyIfAndInFilter).
  std::string dataPath = ds->path();
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates report");
    dataPath = fr.dataPath;
  }

  ojson raw;
  try {
    raw = runCobraJson({"duplicates", "report", "--data", dataPath, "--vars", joined});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  bool allVars = (!varlist.has_value() || varlist->empty());
  std::string colStr = allVars ? "(all variables)" : joined;
  {
    // oracle's col_str is SPACE-joined, not comma.
    colStr = allVars ? "(all variables)" : std::string();
    if (!allVars) {
      for (size_t i = 0; i < cols.size(); ++i) {
        if (i) colStr += " ";
        colStr += cols[i];
      }
    }
  }

  std::ostringstream out;
  out << "Duplicates in terms of " << colStr << "\n\n";
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%8s  %14s  %9s", "copies", "observations", "surplus");
  out << buf << "\n";
  out << std::string(38, '-') << "\n";

  int64_t totalSurplus = 0;
  int64_t nObs = 0, nUnique = 0;
  if (raw.contains("rows") && raw["rows"].is_array()) {
    for (const auto& row : raw["rows"]) {
      int64_t copies = row.value("copies", 0);
      int64_t observations = row.value("observations", 0);
      int64_t surplus = row.value("surplus", 0);
      totalSurplus += surplus;
      nObs += observations;
      if (copies > 0) nUnique += observations / copies;
      std::snprintf(buf, sizeof(buf), "%8lld  %14lld  %9lld", static_cast<long long>(copies),
                    static_cast<long long>(observations), static_cast<long long>(surplus));
      out << buf << "\n";
    }
  }
  out << std::string(38, '-') << "\n";
  std::snprintf(buf, sizeof(buf), "%24s  %9lld", "Total surplus:",
               static_cast<long long>(totalSurplus));
  out << buf;

  if (auto bad = postDuplicatesR(session, nObs, {{"unique_value", static_cast<double>(nUnique)}}))
    return *bad;
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

ojson runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  // Card first (see resolveVarlistFromCard): this entry used to open the frame —
  // i.e. materialize the virtual head — purely to resolve a varlist. `ds` stays
  // null on the card path and every later use of it below is guarded.
  Datastore* ds = nullptr;
  std::vector<std::string> cols;
  auto err = resolveVarlistFromCard(session.shadowVarlistIfCurrent(), varlist, cols);
  if (err.has_value() && *err == "__no_card__") {
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    err = resolveVarlist(ds, varlist, cols);
  }
  if (err.has_value()) return engineErrorBlock(*err);

  // BS-031 (BETA campaign C2, silent-if-drop sweep): `duplicates drop ...
  // if COND` previously ignored `if` entirely (dedup ran over the WHOLE
  // dataset, live-verified this stint: `duplicates drop year if year==1982`
  // silently collapsed all 336 rows down to 7 — one per year — instead of
  // leaving the 288 out-of-sample rows untouched). A correct fix needs to
  // dedup ONLY the if-sample and re-splice the untouched out-of-sample rows
  // back in at their original relative position (row-id tag + append +
  // re-sort) — meaningfully more moving parts than tag/list's fixes below,
  // and NOT exercised by any oracle-pinned scenario this stint (real
  // Stata's `duplicates drop VARLIST` also requires `,force` whenever
  // VARLIST narrows from "all variables" -- enforced at the dispatcher arm
  // since data-verbs-effects-rename-recode-label -- so almost every real
  // `if`-qualified call already errors on that ground first). Converting the silent-wrong
  // into an honest reject here (rather than shipping an unverified
  // multi-step engine choreography with no oracle to pin it against) is the
  // deliberate, disclosed scope boundary for this item.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    return eval::unsupportedOption(
        "if",
        "`duplicates drop` with `if` is not yet supported in this build (dropping duplicates "
        "within an if-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `if`.");
  }
  // BS-090 (BETA Phase G): `in` needs the EXACT same dedup-then-splice-back
  // machinery `if` does (a plain filter would silently dedup only the
  // in-range rows and drop the out-of-range ones' own duplicates too) — the
  // SAME disclosed scope boundary as the `if` reject just above, not a new
  // gap this phase introduces.
  if (inRange.has_value() && !inRange->empty()) {
    return eval::unsupportedOption(
        "in " + *inRange,
        "`duplicates drop` with `in` is not yet supported in this build (dropping duplicates "
        "within an in-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `in`.");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string outPath = nextPath(session.workdir(), "dup_drop");
  // On the card path `ds` is deliberately null (no head was materialized), so the
  // two facts this call still needs come from the session instead: the head PATH
  // it would have reported, and the pre-mutation row count. Both are exactly the
  // substitutions the prior phase-2 work established for this file — a `--data`
  // that only needs a path takes `*currentHbin()`, and a row count the card holds
  // is read from the card. The Datastore path below is byte-identical to before.
  const std::string dataPath = ds ? ds->path() : *session.currentHbin();
  std::vector<std::string> args = {"duplicates", "drop", "--data", dataPath, "--vars", joined,
                                    "--out", outPath};
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  if (ds == nullptr && !cardRows.has_value()) {
    // The card answered the varlist but cannot answer the row count. Rather than
    // report a wrong `nBefore`, fall back to the exact prior behaviour.
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
  }
  int64_t nBefore = ds ? ds->nRows() : *cardRows;  // pre-mutation row count.
  {
    // MAXPERF Stage 2 (item 3): single terminal row-dedup mutation; --out never
    // read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  // Phase 2 (2026-08-20). This used to be `session.datastore()->nRows()`, which
  // MATERIALIZED the whole virtual head — the entire dedup output written to
  // disk — to learn ONE integer, and it did so BEFORE mutationResult ran, so the
  // card fold below could never save the flush it had already paid for. Measured
  // by the Phase 0 caller-attributed census: runDuplicatesDrop forced 100 of the
  // run's 476 head materializations (21%), second only to rename's 220.
  //
  // mutationResult already computes that integer, on BOTH of its paths, and the
  // ordering is the whole point: taking it from the result means the number
  // shipped as `n_dropped` is derived from the very card being shipped as
  // `varlist_after`, instead of from a separate read that could disagree with it.
  // Fold path: `out["n_rows_after"] = delta.rows` (session_state.cpp), the engine's
  // own post-mutation count. Rebuild path: `ds->nRows()` — the identical value
  // this line used to fetch, just no longer fetched twice.
  ojson result = mutationResult(session, "duplicates_drop");
  const int64_t nAfter = result.value("n_rows_after", int64_t{0});
  result["n_dropped"] = nBefore - nAfter;
  if (auto bad = postDuplicatesR(session, nBefore,
                                 {{"N_drop", static_cast<double>(nBefore - nAfter)}}))
    return *bad;
  return result;
}

// BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates tag ...
// if COND, gen(NEWVAR)` previously ignored `if` entirely — the sub-command
// dispatch in runDuplicates() below never even forwarded ifExpr to this
// function. Live-verified this stint (StataBE): tag is computed WITHIN the
// if-sample only, and if-EXCLUDED rows get NEWVAR = MISSING (oracle:
// `duplicates tag year if year==1982, gen(dtag)` -> dtag has N=48 (matches
// the if-sample size), mean=min=max=47, and the other 288 rows are
// missing). Unlike recode, `duplicates tag` is a genuine AGGREGATE over the
// CURRENT rows (group size depends on which OTHER rows are present) — a
// "compute unrestricted, mask after" shortcut would silently give the
// WRONG group sizes whenever `if` doesn't happen to align with whole
// dedup-key groups, so this must actually restrict the input before
// tagging. cobra's own `duplicates tag --keepif EXPR` (apps/cmds/
// cmds_data.cpp) does the restricting, but its `--out` is then only the
// N_sub-row filtered frame — reintegrating that into the FULL N-row
// dataset (untouched rows kept, tag missing for them) needs a join key:
// stamp every row with its own position (`_n`) before filtering, tag the
// filtered subset, then `cobra merge 1:1` the tagged subset's `genvar`
// back onto the full id-stamped frame by that same position (an ordinary
// master-keeps-all merge — unmatched rows get missing "for free", exactly
// Stata's semantics) and drop the synthetic id column again.
ojson runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);
  // BUG-1120 review: generate() becomes the name of a `generate`, so generate's
  // rule answers it: `duplicates tag x, gen(_N)' is "_N invalid varname" r(198),
  // gen(byte) "too few variables specified" r(102) (StataBE 19.5).
  if (auto r = strutil::generateNameRefusal(genvar)) return rcErrorBlock(r->message, r->rc);
  if (ds->has(genvar)) {
    return engineErrorBlock("variable `" + genvar + "' already defined");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) {
    std::string outPath = nextPath(session.workdir(), "dup_tag");
    std::vector<std::string> args = {"duplicates", "tag",    "--data", ds->path(), "--vars",
                                      joined,       "--gen",  genvar,   "--out",    outPath};
    // MAXPERF Stage 2 (item 3): the unrestricted `duplicates tag` is a single
    // terminal pure-ADD mutation (--out becomes the head, never read back as a
    // file). Opt into the resident frame plane. The if/in path below stays
    // STATELESS: it stamps _n, filters, tags, and merges back through
    // intermediate temps that are read AS FILES.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    recordDuplicatesTagType(session, genvar);
    ojson tagged = mutationResult(session, "duplicates_tag", {genvar});
    if (auto bad = postDuplicatesR(session, tagged.value("n_rows_after", int64_t{0}), {}))
      return *bad;
    return tagged;
  }

  const std::string ridCol = "__dup_rid__";
  std::string idPath = nextPath(session.workdir(), "dup_tag_id");
  try {
    // egen seq() is the engine's row-number op — bare `_n` in engine eval
    // requires a bysort context (rejected), unlike PIET-side gen.
    runCobraJson({"eval", "--data", ds->path(), "--egen", ridCol + "=seq()", "--out", idPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BS-090 (BETA Phase G): `ridCol` (already the GLOBAL row index, 1..N,
  // materialized above for the merge-back key regardless of `in`) doubles
  // as the `in a/b` predicate's target — no SEPARATE idx materialization
  // needed here, unlike piet::handlers::applyIfAndInFilter's general case.
  std::string combinedIf = ifExpr.value_or("");
  if (haveIn) {
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.negative = rr.negative;
      return inRangeRejectError(fr, *inRange, "duplicates tag");
    }
    std::string rangePred = ridCol + ">=" + std::to_string(rr.a) + " & " + ridCol + "<=" +
                            std::to_string(rr.b);
    combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  }
  std::string taggedPath = nextPath(session.workdir(), "dup_tag_sample");
  {
    std::vector<std::string> args = {"duplicates", "tag",     "--data", idPath, "--vars",
                                      joined,       "--gen",   genvar,   "--keepif", combinedIf,
                                      "--out",      taggedPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string mergedPath = nextPath(session.workdir(), "dup_tag_merged");
  {
    // merge prints a plain-text "merge 1:1: wrote ..." line, not JSON —
    // runCobra (ok-check), never runCobraJson (same idiom as the
    // duplicates-tag call above).
    CobraRunResult cr = runCobra({"merge", "--data", idPath, "--using", taggedPath, "--type",
                                  "1:1", "--key", ridCol, "--keepusing", genvar, "--nogenerate",
                                  "--out", mergedPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string finalPath = nextPath(session.workdir(), "dup_tag_final");
  {
    CobraRunResult cr = runCobra({"eval", "--data", mergedPath, "--drop", ridCol, "--out",
                                  finalPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  const int64_t nSample = Datastore(taggedPath).nRows();
  // An empty if/in sample is r(2000) BEFORE the new variable exists
  // (w2ao_label_family.log:3177/:3193 lb_duptag_empty: `confirm variable dd`
  // is r(111) afterwards).
  if (nSample == 0)
    if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  session.load(finalPath);
  recordDuplicatesTagType(session, genvar);
  ojson tagged = mutationResult(session, "duplicates_tag", {genvar});
  if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  return tagged;
}

ojson runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string tagVar = "__dup_tag__";
  std::string tagPath = nextPath(session.workdir(), "dup_list_tag");
  {
    // BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates list
    // ... if COND` previously ignored `if` (tag was always computed over
    // the FULL dataset). `list` is read-only/display-only — unlike tag's
    // gen() target, there is no full-N column to preserve, so pre-filtering
    // the SOURCE data (BS-090, BETA Phase G: piet::handlers::
    // applyIfAndInFilter — composes `if`/`in` via AND, replacing the old
    // direct `--keepif` forward on this internal `duplicates tag` call,
    // which could not have evaluated an `in`-derived `_n` predicate anyway)
    // is enough: it restricts BOTH which rows are considered for the dedup
    // AND which rows land in the (already scratch, never session-loaded)
    // `tagPath` output — the browse below then naturally only ever sees the
    // if/in-sample. Live-verified this stint (StataBE): `duplicates list
    // state if year==1982` correctly reports 0 duplicates (each state
    // appears once per year), `duplicates list year if year==1982` lists
    // all 48 in-sample rows (all share year 1982). Known, disclosed
    // cosmetic gap: real Stata's list table shows each row's ORIGINAL
    // (pre-filter) observation number in its "Obs" column — this handler's
    // browse()-based output has no such column at all (pre-existing, not
    // introduced by this fix), so that particular renumbering nuance isn't
    // addressed here.
    std::string dupListDataPath = ds->path();
    if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
      InRangeFilterResult fr;
      try {
        fr = applyIfAndInFilter(session, dupListDataPath, ifExpr, inRange);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates list");
      dupListDataPath = fr.dataPath;
    }
    std::vector<std::string> args = {"duplicates", "tag",  "--data", dupListDataPath, "--vars",
                                      joined,       "--gen", tagVar,  "--out",         tagPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string filteredPath = nextPath(session.workdir(), "dup_list_filtered");
  try {
    runCobraJson({"eval", "--data", tagPath, "--keepif", tagVar + ">0", "--out", filteredPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  {
    const int64_t nSample = Datastore(tagPath).nRows();
    if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  }
  Datastore filteredDs(filteredPath);
  if (filteredDs.nRows() == 0) {
    ojson block;
    block["type"] = "display";
    block["text"] = "No duplicates found.";
    return block;
  }
  std::optional<std::vector<std::string>> browseVars;
  if (varlist.has_value() && !varlist->empty()) browseVars = *varlist;
  // Value labels live on the SESSION keyed by varname, so they apply to the
  // cobra-filtered temp dataset too (same column names) — an empty map here
  // rendered value-labeled f64 columns as raw codes, diverging from the
  // oracle's duplicates.py (value_label_map=session.get_value_label_map()).
  // format-display-default-lifecycle (FU-1015): and every column's display
  // format from the one resolver, as list carries it (handlers_m3 runList).
  ojson block = filteredDs.browse(0, 200, browseVars, session.valueLabelMapForBrowse(),
                                  session.displayFormatMap(filteredDs.formatCard(browseVars)));
  block["editable"] = false;
  block["note"] = std::to_string(filteredDs.nRows()) + " duplicate observations";
  return block;
}

namespace {

// The display columns `s` fills: one per character, two for the East Asian wide
// ranges (StataBE 19.5 lists "héllo" in five columns and "日本" in four).
size_t displayCols(const std::string& s) {
  size_t cols = 0;
  for (size_t i = 0; i < s.size();) {
    char32_t cp = 0;
    const size_t n = cecon::ident::decodeUtf8(s, i, &cp);
    if (n == 0) {  // a stray byte takes one column
      ++cols;
      ++i;
      continue;
    }
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
    cols += wide ? 2 : 1;
    i += n;
  }
  return cols;
}

// list's header abbreviation of a variable name to `n` columns: the leading
// characters that fit, `~`, then the last character (`household_id` -> `househ~d`,
// `ééééééééé` -> `éééééé~é`, StataBE 19.5). A name that fits is kept whole.
std::string abbrevName(const std::string& name, size_t n) {
  if (displayCols(name) <= n) return name;
  size_t last = name.size() - 1;
  while (last > 0 && (static_cast<unsigned char>(name[last]) & 0xC0) == 0x80) --last;
  const std::string tail = name.substr(last);
  const size_t budget = n - 1 - displayCols(tail);
  std::string head;
  for (size_t i = 0; i < last;) {
    const size_t len = cecon::ident::utf8SeqLen(static_cast<unsigned char>(name[i]));
    const std::string ch = name.substr(i, len);
    if (displayCols(head) + displayCols(ch) > budget) break;
    head += ch;
    i += len;
  }
  return head + "~" + tail;
}

}  // namespace

// BUG-1096: `duplicates examples [varlist] [if] [in]` -- one line per group of
// observations that share their varlist values (two or more of them in the
// sample): the group's size, the observation number of its first member and
// the shared values, groups in the varlist's sort order. duplicates.ado lists
// them in `list`'s box (Group only when there are two groups or more), after
// "Duplicates in terms of x" ("... all variables" without a varlist); with no
// such group it says "(0 observations are duplicates)". r(N) is the sample size,
// and an empty sample is r(2000). StataBE 19.5 (x = 1 2 1 3 2 1):
//   +--------------------------+
//   | Group   #   e.g. obs   x |
//   |--------------------------|
//   |     1   3          1   1 |
//   |     2   2          2   2 |
//   +--------------------------+
// Each column is as wide as its header or widest cell (a value label for a
// labelled code, a missing code as written), in display columns, and a rule
// follows every fifth line, as `list` draws it. A variable's header is its name
// unless that is wider than both eight columns and the widest cell, when list
// abbreviates it to the wider of the two (`household_id` over 1 and 2 is
// `househ~d`, `averylongvariablename` over 12-column strings `averylongv~e`); a
// variable whose format is left-justified (%-12s, %-9.0g) is left-aligned in its
// header and cells, every other column right-aligned (StataBE 19.5, %~12s too).
// list picks this table only when its lines fit c(linesize); PIET keeps no line
// width (`set linesize` is ignored), so it prints the table Stata prints at a
// linesize wide enough, and refuses by name a table wider than 255, the widest
// line Stata allows, which Stata always lists in its display format.
ojson runDuplicatesExamples(const std::optional<std::vector<std::string>>& varlist,
                            SessionState& session, const std::optional<std::string>& ifExpr,
                            const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  // The sample, with each row's observation number in the whole data.
  std::optional<Datastore> sampleFile;
  const Datastore* sample = ds;
  std::vector<int64_t> obs;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    const std::string ridCol = "__dup_rid__";
    const std::string idPath = nextPath(session.workdir(), "dup_ex_id");
    InRangeFilterResult fr;
    try {
      runCobraJson({"eval", "--data", ds->path(), "--egen", ridCol + "=seq()", "--out", idPath});
      fr = applyIfAndInFilter(session, idPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "duplicates examples");
    sampleFile.emplace(fr.dataPath);
    sample = &*sampleFile;
    const double* rid = sample->col(ridCol).f64();
    for (int64_t i = 0; i < sample->nRows(); ++i) obs.push_back(static_cast<int64_t>(rid[i]));
  } else {
    for (int64_t i = 0; i < sample->nRows(); ++i) obs.push_back(i + 1);
  }
  if (auto bad = postDuplicatesR(session, sample->nRows(), {})) return *bad;

  // Group the rows by their cells (a number by its bits, so `.a` is not `.`).
  std::vector<const cecon::Column*> keyCols;
  for (const auto& c : cols) keyCols.push_back(&sample->col(c));
  auto cellText = [](const cecon::Column& c, int64_t i) -> std::string {
    if (c.dtype == cecon::DType::Factor) {
      const int32_t code = c.codes()[i];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    }
    uint64_t bits;
    const double v = c.f64()[i] == 0.0 ? 0.0 : c.f64()[i];
    std::memcpy(&bits, &v, sizeof bits);
    return std::to_string(bits);
  };
  struct Group {
    int64_t first = 0;  // the sample row of its first member
    int64_t size = 0;
  };
  std::map<std::vector<std::string>, Group> groups;
  for (int64_t i = 0; i < sample->nRows(); ++i) {
    std::vector<std::string> key;
    for (const cecon::Column* c : keyCols) key.push_back(cellText(*c, i));
    auto [it, fresh] = groups.emplace(std::move(key), Group{i, 0});
    (void)fresh;
    ++it->second.size;
  }
  std::vector<Group> dups;
  for (const auto& [key, g] : groups)
    if (g.size > 1) dups.push_back(g);
  // Stata's sort order of the varlist: numbers ascending then the missing
  // codes, strings by their bytes.
  std::sort(dups.begin(), dups.end(), [&](const Group& a, const Group& b) {
    for (const cecon::Column* c : keyCols) {
      int d = 0;
      if (c->dtype == cecon::DType::Factor) {
        d = cellText(*c, a.first).compare(cellText(*c, b.first));
      } else {
        d = cecon::stata_cmp(c->f64()[a.first], c->f64()[b.first]);
      }
      if (d != 0) return d < 0;
    }
    return false;
  });

  const bool allVars = !varlist.has_value() || varlist->empty();
  std::string text = "Duplicates in terms of ";
  if (allVars) {
    text += "all variables";
  } else {
    for (size_t i = 0; i < cols.size(); ++i) text += (i ? " " : "") + cols[i];
  }
  if (dups.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = text + "\n\n(0 observations are duplicates)";
    return block;
  }

  std::vector<std::string> head;
  if (dups.size() > 1) head.push_back("Group");
  head.push_back("#");
  head.push_back("e.g. obs");
  const size_t firstVar = head.size();
  for (const auto& c : cols) head.push_back(c);
  // Each variable's display format, from the one resolver list uses.
  std::map<std::string, std::string> fmtOf;
  for (const auto& [name, fmt] : session.displayFormatMap(ds->formatCard(cols)))
    fmtOf[name] = fmt;
  std::vector<bool> left(head.size(), false);
  for (size_t k = 0; k < cols.size(); ++k)
    left[firstVar + k] = fmtOf[cols[k]].compare(0, 2, "%-") == 0;
  std::vector<std::vector<std::string>> rows;
  for (size_t g = 0; g < dups.size(); ++g) {
    std::vector<std::string> row;
    if (dups.size() > 1) row.push_back(std::to_string(g + 1));
    row.push_back(std::to_string(dups[g].size));
    row.push_back(std::to_string(obs[static_cast<size_t>(dups[g].first)]));
    for (size_t k = 0; k < cols.size(); ++k) {
      const cecon::Column& c = *keyCols[k];
      if (c.dtype == cecon::DType::Factor) {
        row.push_back(cellText(c, dups[g].first));
        continue;
      }
      const double v = c.f64()[dups[g].first];
      const int kind = cecon::missing_kind(v);
      if (kind >= 0) {
        row.push_back(cecon::missing_text(kind));
        continue;
      }
      auto vl = session.valueLabels.find(cols[k]);
      if (vl != session.valueLabels.end() && v == std::floor(v)) {
        auto lab = vl->second.find(static_cast<int>(v));
        if (lab != vl->second.end()) {
          row.push_back(lab->second);
          continue;
        }
      }
      row.push_back(trim(eval::applyStataFmt(v, fmtOf[cols[k]])));
    }
    rows.push_back(std::move(row));
  }
  std::vector<size_t> width(head.size());
  for (size_t j = 0; j < head.size(); ++j) {
    size_t widest = 0;
    for (const auto& r : rows) widest = std::max(widest, displayCols(r[j]));
    if (j >= firstVar) head[j] = abbrevName(head[j], std::max<size_t>(8, widest));
    width[j] = std::max(displayCols(head[j]), widest);
  }
  auto line = [&](const std::vector<std::string>& cells) {
    std::string s = "  |";
    for (size_t j = 0; j < cells.size(); ++j) {
      const std::string pad(width[j] - displayCols(cells[j]), ' ');
      s += (j ? "   " : " ") + (left[j] ? cells[j] + pad : pad + cells[j]);
    }
    return s + " |\n";
  };
  size_t inner = 2 + 3 * (head.size() - 1);
  for (size_t w : width) inner += w;
  if (inner + 4 > 255) {
    return eval::errorBlock(
        "unsupported_command",
        "duplicates examples: these groups make a table " + std::to_string(inner + 4) +
            " columns wide, wider than Stata's widest line (linesize 255), so Stata lists them "
            "in list's display format, which this build does not implement; name fewer "
            "variables",
        std::string("duplicates"), std::nullopt, std::nullopt, "refused:list-display-format");
  }
  const std::string box = "  +" + std::string(inner, '-') + "+\n";
  const std::string rule = "  |" + std::string(inner, '-') + "|\n";
  text += "\n\n" + box + line(head) + rule;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (r > 0 && r % 5 == 0) text += rule;
    text += line(rows[r]);
  }
  text += box;
  text.pop_back();  // the block carries no trailing newline
  ojson block;
  block["type"] = "display";
  block["text"] = text;
  return block;
}

// ---------------------------------------------------------------------------
// reshape long|wide STUB..., i(IDVARS) j(JVAR) — C++ port of reshape.py's
// run_reshape (direction dispatch + _parse_ivar_jvar) + run_reshape_long/
// run_reshape_wide's OWN pre-validation (id/j/stub existence, i()/j()
// presence). cobra's native `reshape` verb requires --i/--j as mandatory
// CLI args and has no equivalent to the oracle's "i() option required —
// e.g. ..." message (a bare CLI usage error, not the structured JSON error
// shape) — pre-validate everything here so error cells match byte-for-byte,
// success path calls cobra for the actual melt/pivot.


}  // namespace piet::handlers
