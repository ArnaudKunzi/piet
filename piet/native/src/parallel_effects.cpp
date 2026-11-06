// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · L2 — see parallel_effects.h for the safety axiom this file exists to serve.
#include "piet/parallel_effects.h"

#include <map>
#include <sstream>
#include <regex>
#include <set>

#include "piet/program.h"
#include "piet/strutil.h"
#include "registry.h"

namespace piet::parallel {

namespace {

Effects mk(int wms, bool wr, bool rr, bool rd, bool wd, bool ctrl, bool rng, bool wf) {
  Effects e;
  // wms: 0 = none, 1 = LOCALS only (scoped), 2 = session-global state.
  e.writesLocals = (wms == 1);
  e.writesGlobalState = (wms == 2);
  e.writesResults = wr;
  e.readsResults = rr;
  e.readsDataset = rd;
  e.writesDataset = wd;
  e.control = ctrl;
  e.rng = rng;
  e.writesFiles = wf;
  return e;
}

// The table. Sparse ON PURPOSE (see the header): a command missing from here is
// UNKNOWN, and unknown refuses. Rows are the commands the acceptance corpus and
// the probe fixtures actually use, which is the MVP scope the design brief set —
// coverage grows monotonically with performance and never with risk.
//
// Column order: writesMacroState, writesResults, readsResults, readsDataset,
//               writesDataset, control, rng, writesFiles.
const std::map<std::string, Effects>& table() {
  static const std::map<std::string, Effects> t = {
      // -- pure display / read-only -----------------------------------------
      {"display", mk(0, 0, 1, 1, 0, 0, 0, 0)},   // may reference r()/e() and vars
      {"list", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      {"describe", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"codebook", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      {"browse", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      // -- r-class readers ---------------------------------------------------
      {"summarize", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"tabulate", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"count", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"levelsof", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"inspect", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // -- dataset mutation --------------------------------------------------
      {"generate", mk(0, 0, 1, 1, 1, 0, 0, 0)},
      {"replace", mk(0, 0, 1, 1, 1, 0, 0, 0)},
      {"drop", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"keep", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"rename", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"sort", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"gsort", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"egen", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"encode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"destring", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"tostring", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"label", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"order", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"recode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"duplicates", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"collapse", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // `clear` is a self-seeder in the only sense that matters here: after it,
      // nothing of the inherited frame is observable, so the task does not
      // depend on what the parent had loaded.
      {"clear",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      {"set", mk(0, 0, 0, 0, 0, 1, 0, 0)},  // session-global knob => control
      // -- data in (self-seeding) -------------------------------------------
      // These are the ONLY rows that set datasetSelfSeed: a task that begins
      // with one of them does not depend on whatever frame it inherited.
      {"use", [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      {"sysuse",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      {"import",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      // -- data out ----------------------------------------------------------
      {"save", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"export", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"outsheet", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"graph", mk(0, 0, 0, 1, 0, 0, 0, 1)},  // graph export writes; graph twoway draws
      // -- estimation --------------------------------------------------------
      {"regress", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"reghdfe", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"logit", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"probit", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"cloglog", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"poisson", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"nbreg", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"xtreg", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"xtset", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"ivregress", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"sureg", mk(0, 1, 0, 1, 0, 0, 0, 0)},  // reg3 ..., sure: an e() writer like ivregress
      {"areg", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"lincom", mk(0, 1, 1, 1, 0, 0, 0, 0)},
      {"test", mk(0, 1, 1, 1, 0, 0, 0, 0)},
      {"testparm", mk(0, 1, 1, 1, 0, 0, 0, 0)},  // BUG-367: same effects as `test`
      {"predict", mk(0, 0, 1, 1, 1, 0, 0, 0)},
      {"margins", mk(0, 1, 1, 1, 0, 0, 0, 0)},
      // -- macro / scalar writers (region-fatal by rule 5, but classified so the
      //    refusal names THEM rather than "unknown command") ------------------
      {"local", mk(1, 0, 1, 0, 0, 0, 0, 0)},
      {"global", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"scalar", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"matrix", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"tempfile", mk(1, 0, 0, 0, 0, 0, 0, 0)},
      {"tempname", mk(1, 0, 0, 0, 0, 0, 0, 0)},
      {"tempvar", mk(1, 0, 0, 0, 0, 0, 0, 0)},
      {"args", mk(1, 0, 0, 0, 0, 0, 0, 0)},
      {"gettoken", mk(1, 0, 0, 0, 0, 0, 0, 0)},
      {"confirm", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      {"assert", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      // -- control: anything whose effect is the SESSION, not the data --------
      {"exit", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"log", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"cd", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"do", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"run", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      // W2 doinclude: `include` runs a file in the CALLER's scope -- the same
      // session-control effect as do/run, and more so (it can write its locals).
      {"include", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"program", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"preserve", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"restore", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      // postfile/post/postclose write a FILE through a handle whose namespace is
      // process-global. Under process workers that namespace is not shared
      // (design brief §10.4's P-5 verdict, which the corpus's fixed handle names
      // `overall_post`/`league_post`/`epl_post` prompted), so what is left to
      // decide is only the destination path — which rule 10 decides, like any
      // other file write. Classifying them as session control instead would
      // refuse on a hazard that process isolation already removed.
      {"postfile", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      {"post", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      {"postclose", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // The estimates store is session state, but a worker's store dies with it
      // and the region predicate's successor walk is what proves the parent never
      // reads one back. Marking it `control` instead refused every program that
      // stores its own fits — which on the acceptance corpus is all of them.
      {"estimates",
       [] { Effects e = mk(0, 0, 0, 1, 0, 0, 0, 0); e.usesEstimates = true; return e; }()},
      {"eststo",
       [] { Effects e = mk(0, 0, 0, 1, 0, 0, 0, 0); e.usesEstimates = true; return e; }()},
      {"esttab",
       [] { Effects e = mk(0, 0, 0, 1, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      {"return", mk(0, 1, 0, 0, 0, 0, 0, 0)},
      {"timer", mk(0, 1, 0, 0, 0, 1, 0, 0)},
      {"frame", mk(0, 0, 0, 1, 1, 1, 0, 0)},
      {"discard", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      // -- second fill (P9 entry wave): the commands the acceptance corpus's own
      //    program bodies reach, added one refusal at a time. Coverage is monotone
      //    with performance and orthogonal to safety, so growing this list can
      //    only make the recognizer say yes more often — never wrongly.
      {"format", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"merge", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"append", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"reshape", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"tsset", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"tsfill", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // BUG-370: these two rows predated any dispatcher arm — they described
      // commands PIET did not yet have. Both are live as of 2026-08-31; the
      // classification was right all along and needed no change.
      {"xtile", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"ipolate", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"tssmooth", mk(0, 0, 0, 1, 1, 0, 0, 0)},  // BUG-370: same shape as its siblings
      {"decode", mk(0, 0, 0, 1, 1, 0, 0, 0)},   // BUG-371: reads + appends a column, like encode
      {"ds", mk(0, 1, 0, 1, 0, 0, 0, 0)},       // BUG-371: r-class reader (r(varlist))
      {"unab", mk(1, 0, 0, 1, 0, 0, 0, 0)},     // BUG-371: writes a LOCAL, reads the varlist
      {"xtsum", mk(0, 1, 0, 1, 0, 0, 0, 0)},    // BUG-371: r-class reader, like summarize
      {"xttab", mk(0, 1, 0, 1, 0, 0, 0, 0)},    // BUG-371: r-class reader, like tabulate
      {"winsor2", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"expand", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"fillin", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"split", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"joinby", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"contract", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"notes", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"char", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"mvencode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"mvdecode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"correlate", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"pwcorr", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"ttest", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"centile", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"pctile", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"table", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"tab1", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"tab2", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"histogram", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      // kdensity is rclass (r(kernel bwidth n scale)) and generate() adds
      // variables (w2aq_histogram_family hk_kd_*_r, hk_kd_*_g).
      {"kdensity", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"scatter", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      {"line", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      {"twoway", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      // serset-twoway SS: `serset` reads the session's graph-owned serset store
      // (the graphs drawn before it, in this process), posts r(), and `serset
      // use` replaces the data: session control, never moved to a worker.
      {"serset", mk(0, 1, 0, 0, 1, 1, 0, 0)},
      {"estat", mk(0, 1, 1, 1, 0, 0, 0, 0)},
      {"fepois", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"ppmlhdfe", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"ivreghdfe", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"xtpoisson", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"xtlogit", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"csdid", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"nlcom", mk(0, 1, 1, 1, 0, 0, 0, 0)},
      {"which", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"quietly", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // -- RNG ---------------------------------------------------------------
      {"bootstrap", mk(0, 1, 0, 1, 0, 0, 1, 0)},
      {"permute", mk(0, 1, 0, 1, 0, 0, 1, 0)},
      {"simulate", mk(0, 1, 0, 1, 0, 0, 1, 0)},
      {"boottest", mk(0, 1, 1, 1, 0, 0, 1, 0)},
      // ---- AER-SCOPE1 bundle C (GTOOLS, 2026-09-03) -----------------------
      // Each alias carries the classification of the command it forwards to.
      // The head is what this table is keyed on, and the translation happens
      // AFTER the recognizer has already read it, so an alias missing here
      // would refuse a region its native twin allows.
      {"gcollapse", mk(0, 0, 0, 1, 1, 0, 0, 0)},   // = collapse
      {"fcollapse", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"gegen", mk(0, 0, 0, 1, 1, 0, 0, 0)},       // = egen
      {"gduplicates", mk(0, 1, 0, 1, 1, 0, 0, 0)}, // = duplicates
      {"glevelsof", mk(1, 1, 0, 1, 0, 0, 0, 0)},   // = levelsof (writes a local)
      {"gisid", mk(0, 0, 0, 1, 0, 0, 0, 0)},       // read-only assertion
      {"greshape", mk(0, 0, 0, 1, 1, 0, 0, 0)},    // = reshape
      {"fmerge", mk(0, 0, 0, 1, 1, 0, 0, 0)},      // = merge
      {"gquantiles", mk(0, 1, 0, 1, 1, 0, 0, 0)},  // xtile form writes, _pctile form sets r()
      {"gstats", mk(0, 1, 0, 1, 1, 0, 0, 0)},

      // ---- AER-SCOPE1 bundle B (RESULTS, 2026-09-03) ----------------------
      // `return` already has a row above (it is a macro-layer verb); these three
      // are new. svmat WRITES the dataset and may extend _N; mkmat and matlist
      // write/read the session matrix store, which is global state.
      {"svmat", mk(0, 0, 1, 1, 1, 0, 0, 0)},
      {"mkmat", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      {"matlist", mk(0, 0, 1, 0, 0, 0, 0, 0)},

      // ---- ml-getmata T4 (2026-09-27) -------------------------------------
      // `mata` writes only the separate Mata store and reads the Stata matrix
      // store through st_matrix(); M0 has no st_data/st_store and never touches
      // r()/e(). `getmata` reads the Mata store and the dataset (existing
      // names, _N), writes variables, may extend _N, and publishes
      // r(K_new)/r(K_existing). These rows describe the commands; they do NOT
      // make either one parallelizable. The Mata store is neither shipped to a
      // worker nor returned from one, so summarizeStatement refuses both by
      // name before this table is consulted.
      {"mata", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"getmata", mk(0, 1, 1, 1, 1, 0, 0, 0)},

      // ---- AER-SCOPE1 bundle A (2026-09-03) -------------------------------
      {"ivreg", mk(0, 1, 0, 1, 0, 0, 0, 0)},   // same shape as ivregress
      {"winsor", mk(0, 0, 0, 1, 1, 0, 0, 0)},  // reads a column, writes a new one

      // ---- AER-SCOPE1 bundle D (ENV, 2026-09-03) --------------------------
      // Environment verbs are `control`: they change what a LATER statement in
      // the region sees (the cwd a path resolves against, the files on disk, a
      // recorded search path) without that dependency appearing in any varlist
      // the recognizer can read. Refusing a region that contains one is the
      // right answer, and `control` is how this table says so.
      {"shell", mk(0, 0, 0, 0, 0, 1, 0, 1)},   // + writes files, by design
      {"type", mk(0, 0, 0, 0, 0, 1, 0, 0)},    // reads a file a sibling task may write
      {"dir", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"ls", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"net", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"ado", mk(0, 0, 0, 0, 0, 1, 0, 0)},
      {"adopath", mk(0, 0, 0, 0, 0, 1, 0, 0)},  // session-global search list
      {"sysdir", mk(0, 0, 0, 0, 0, 1, 0, 0)},   // session-global directory map
      {"findfile", mk(0, 1, 0, 0, 0, 1, 0, 0)},  // sets r(fn); reads the filesystem
      {"error", mk(0, 0, 0, 0, 0, 1, 0, 0)},     // aborts the region, like exit
      {"frames", mk(0, 0, 0, 1, 1, 1, 0, 0)},    // same shape as `frame`
      {"tset", mk(0, 1, 0, 1, 1, 0, 0, 0)},      // same shape as `tsset`

      // ---- AER-SCOPE1 bundle E (TABLES, 2026-09-04) -----------------------
      // These read the ESTIMATES store rather than the dataset, so
      // `usesEstimates` carries the dependency the varlist cannot show: a
      // region holding `eststo m1` and a later `estout m1` is ordered even
      // though neither statement names a variable in common. Every row that
      // takes `using` also sets writesFiles.
      {"estadd",
       [] { Effects e = mk(0, 1, 0, 1, 0, 0, 0, 0); e.usesEstimates = true; return e; }()},
      {"estpost",
       [] { Effects e = mk(0, 1, 0, 1, 0, 0, 0, 0); e.usesEstimates = true; return e; }()},
      {"estout",
       [] { Effects e = mk(0, 0, 1, 1, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // outreg2 reads e() through addstat(), so it is readsResults as well.
      {"outreg2",
       [] { Effects e = mk(0, 0, 1, 1, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // texsave/tabout render the DATASET, not the estimates.
      {"texsave", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"tabout", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      // frmttable and mat2txt render a MATRIX out of the session store.
      {"frmttable", mk(0, 0, 1, 0, 0, 0, 0, 1)},
      {"mat2txt", mk(0, 0, 1, 0, 0, 0, 0, 1)},
      // texdoc holds an open file across statements, which is exactly what
      // `control` is for: a later `tex` line depends on an earlier `init` with
      // nothing in either varlist to say so.
      {"texdoc", mk(0, 0, 0, 0, 0, 1, 0, 1)},
      {"tex", mk(0, 0, 0, 0, 0, 1, 0, 1)},
      // regsave/parmest REPLACE the dataset in memory when no `using`/`saving()`
      // is given, so they write the dataset as well as a file.
      {"regsave",
       [] { Effects e = mk(0, 0, 1, 1, 1, 0, 0, 1); e.usesEstimates = true; return e; }()},
      {"parmest",
       [] { Effects e = mk(0, 0, 1, 1, 1, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // erepost mutates the ACTIVE e(), which every later postestimation reads.
      {"erepost",
       [] { Effects e = mk(0, 1, 1, 0, 0, 0, 0, 0); e.usesEstimates = true; return e; }()},

      // ---- AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) ----------------------
      // Shape verbs read the frame and write it back.
      {"split", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"xpose", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"insobs", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"stack", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"separate", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"mvencode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"cumul", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"pctile", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // recast rewrites the declared storage type and no cell, but that type is
      // session-global state a later `save` reads, so it is not effect-free.
      {"recast", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      // notes writes `characteristics`, which is session-global and persists
      // through save/use.
      {"notes", mk(2, 0, 0, 0, 0, 0, 0, 0)},
      {"compare", mk(0, 0, 0, 1, 0, 0, 0, 0)},
      // `_pctile` publishes r(r1..rN) and leaves the dataset alone; `pctile`
      // is the variable-creating twin above.
      {"_pctile", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // The samplers set rng: two runs of the same region are only identical
      // when the seed is, so a parallel region containing one must serialize.
      {"sample", mk(0, 0, 0, 1, 1, 0, 1, 0)},
      {"splitsample", mk(0, 0, 0, 1, 1, 0, 1, 0)},
      {"bsample", mk(0, 0, 0, 1, 1, 0, 1, 0)},
      // drawnorm reads the session MATRIX store for corr()/cov().
      {"drawnorm", mk(0, 0, 1, 1, 1, 0, 1, 0)},
      // `input` BEGINS by creating its own data, exactly like `use` -- which is
      // what datasetSelfSeed says, and why it is not merely writesDataset.
      {"input",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},

      // ---- AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) ----------------------
      // The three readers BEGIN by loading their own data, exactly like `use`.
      {"insheet",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      {"infile",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      {"infix",
       [] { Effects e = mk(0, 0, 0, 0, 1, 0, 0, 0); e.datasetSelfSeed = true; return e; }()},
      // The writers read the frame and leave a file.
      {"outsheet", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"outfile", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"saveold", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      // putexcel reads e()/r()/scalars through evalScalarExpr and the matrix
      // store through matrix(); it also carries an open workbook ACROSS
      // statements, which is what `control` says.
      {"putexcel",
       [] { Effects e = mk(0, 0, 1, 1, 0, 1, 0, 1); return e; }()},
      // filefilter publishes r(occurrences) and rewrites a file on disk.
      {"filefilter", mk(0, 1, 0, 0, 0, 0, 0, 1)},
      {"copy", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      // The four refusals never run, but an UNKNOWN command refuses a parallel
      // region, and "this command is refused" is not a reason to serialize a
      // region that merely mentions it. writesFiles is the honest shape for
      // what they would do if implemented.
      {"unzipfile", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      {"zipfile", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      // AER tier-1 (2026-10-28). Each row copies the neighbour it is wired over:
      // macro/matsort/matselrc write session-global stores like `matrix`; fvunab
      // writes a local like `unab`; xtdescribe/findname/cf publish r() off the
      // data like `xtsum`; lrtest reads stored estimates and publishes r();
      // integ publishes r() and may generate; the data writers copy `xtile`/
      // `rename`/`label`; zipuse replaces the data like `use` (without its
      // self-seed claim) and zipsave writes a file like `save`; postutil copies
      // `postclose`; require/_dots touch nothing a later statement reads.
      {"macro", mk(2, 0, 0, 0, 0, 0, 0, 0)},
      {"xtdescribe", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"mvdecode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"lrtest", mk(0, 1, 1, 0, 0, 0, 0, 0)},
      {"fvunab", mk(1, 0, 0, 1, 0, 0, 0, 0)},
      {"integ", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"numlabel", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"_dots", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"cf", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"renpfix", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"markout", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"postutil", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"require", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"labvars", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"astile", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"matsort", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"matselrc", mk(2, 0, 1, 0, 0, 0, 0, 0)},
      {"fillmissing", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"findname", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"sencode", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"zipuse", mk(0, 0, 0, 0, 1, 0, 0, 0)},
      {"zipsave", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      {"odbc", mk(0, 0, 0, 0, 0, 0, 0, 1)},
      {"spshape2dta", mk(0, 0, 0, 0, 0, 0, 0, 1)},

      // ---- AER-SCOPE1 bundle I (UTIL, 2026-09-04) -------------------------
      // The counters publish r() and (for unique's by()+gen() form) a column.
      {"unique", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"gunique", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"distinct", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"mdesc", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"splitvallabels", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // missings drops variables or observations depending on its sub-verb.
      {"missings", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      // Column writers.
      {"carryforward", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"lookfor", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"ihstrans", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"sctorezone", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"rangestat", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"tsspell", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // Metadata writers: the label store and variable names are session-global
      // state a later `save` reads.
      {"labmask", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      {"renvarlab", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      // marksample defines a LOCAL and generates a temporary marker column.
      {"marksample", mk(1, 0, 0, 1, 1, 0, 0, 0)},
      // filelist REPLACES the dataset from the filesystem -- it seeds its own
      // data exactly like `use`, and what it reads is a directory a sibling
      // task may be writing into, which is what `control` says.
      {"filelist",
       [] { Effects e = mk(0, 1, 0, 0, 1, 1, 0, 0); e.datasetSelfSeed = true; return e; }()},

      // ---- AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) ----------------------
      // grc1leg2 and event_plot read the session's named graphs / stored
      // estimates and leave a graph behind, the same shape `graph` has.
      {"grc1leg2",
       [] { Effects e = mk(0, 0, 1, 0, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // ---- AER-SCOPE2 bundle K1 (2026-09-07) ------------------------------
      // `grc1leg` routes to grc1leg2's handler, so it has grc1leg2's shape
      // exactly. Classified rather than left out: an unclassified head refuses
      // the whole auto-parallel region, which is safe and silent, and silent is
      // the thing this table exists to stop.
      {"grc1leg",
       [] { Effects e = mk(0, 0, 1, 0, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // The three renamers. A variable NAME is session-global state a later
      // `save` reads, which is why renvarlab above is wms=2 and not a dataset
      // writer -- these three take that shape exactly. `move` only permutes
      // columns and writes no name, but the column ORDER is the same kind of
      // state for the same reason, so it is classified with them rather than
      // given a laxer row it would be the first command to need.
      {"tolower", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      {"renvars", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      {"move", mk(2, 0, 0, 1, 0, 0, 0, 0)},
      // ---- AER scope 2 bundle B2 (2026-09-07) -----------------------------
      // strdist reads two string columns, writes one numeric column, and (in
      // the two-literal form) publishes r(d)/r(strdist). Same shape as the
      // column writers above plus the results bit.
      {"strdist", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      // `parallel do` re-enters the do-file executor, so it can do ANYTHING a
      // do-file can. `control` is the honest classification -- it makes the
      // auto-parallel region refuse rather than reason about a body it cannot
      // see. Every other bit is set too, for the same reason.
      {"parallel", mk(2, 1, 1, 1, 1, 1, 1, 1)},
      {"event_plot",
       [] { Effects e = mk(0, 0, 1, 0, 0, 0, 0, 1); e.usesEstimates = true; return e; }()},
      // cdfplot reads the frame and emits a plot.
      {"cdfplot", mk(0, 0, 0, 1, 0, 0, 0, 1)},
      // The geo verbs create a column from coordinates. geonear READS a using
      // file but writes none, so writesFiles stays 0.
      {"geodist", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      {"geonear", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // ---- AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ------------------------
      // E1 aliases inherit their target's shape exactly: areg is regress
      // (:90), the two ivreg2 spellings are ivregress (:98).
      {"areg", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"ivreg2", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"xtivreg2", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // sureg = reg3 ..., sure (sureg): reads the frame, posts e()
      // and a LastModel, writes no column and no file -- the estimation shape.
      {"sureg", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // E2 computed here: read the frame, publish r(), touch nothing else --
      // the shape pwcorr (:183) already has.
      {"spearman", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"sktest", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"signrank", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"centile", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      {"pca", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // oaxaca (AER scope-2 B2): reads the frame, posts e() and a LastModel,
      // writes no column and no file. Same shape as pca.
      {"oaxaca", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // glm left the refusal list below with X2-GLM (2026-10-12): the native
      // fitter reads the frame and posts e() and a LastModel, writes no column
      // and no file. Same shape as pca.
      {"glm", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // qreg2 left E3 (handlers_e3_qreg2.cpp): it reads the frame and posts e(),
      // and without cluster() an if/in fit leaves the sample rows first
      // (qreg2.ado's `sort _gv`), so it also writes the dataset's order.
      {"qreg2", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      // teffects left the refusal list below 2026-09-09 (AER scope-2 B2-4).
      // `teffects nnmatch` reads the frame and posts e()/a LastModel like
      // oaxaca, and under gen(stub) it WRITES stub1..stubK -- so it carries
      // writesDataset for the command, not for the particular invocation.
      {"teffects", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      // These three can WRITE a column (tsfilter's cycle/trend newvars,
      // lpoly/lowess under generate()), so writesDataset is set conservatively
      // -- it is set for the command, not for the particular invocation.
      {"tsfilter", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"lpoly", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      {"lowess", mk(0, 1, 0, 1, 1, 0, 0, 0)},
      // xi WRITES columns and its prefix form also runs the wrapped command,
      // so it is the union: writesDataset plus readsDataset.
      {"xi", mk(0, 0, 0, 1, 1, 0, 0, 0)},
      // bcal left the refusal list 2026-09-09 (AER scope-2 B2-3). `bcal
      // create` READS the from() column, WRITES a .stbcal file, posts r(), and
      // with generate() writes a dataset column -- all four are real effects,
      // and writesFiles is what makes a parallel region refuse it.
      {"bcal", mk(0, 1, 0, 1, 1, 0, 0, 1)},
      // E3 refusals are genuinely effect-free: they emit an error block and
      // change nothing a later statement can read. Classified rather than added
      // to the frozen exempt list, because "this command does nothing" is a
      // fact about them and not an exception to the rule.
      {"suest", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // A fitted lasso reads the data, posts e(), and may select a random CV path.
      {"lasso", mk(0, 1, 0, 1, 0, 0, 1, 0)},
      // ml model/init/search/maximize carry a persistent model specification.
      // Keep the whole lifecycle serial rather than split it between workers.
      {"ml", mk(2, 1, 1, 1, 0, 1, 1, 0)},
      {"lassocoef", mk(0, 0, 1, 0, 0, 0, 0, 0)},
      {"rlasso", mk(0, 1, 0, 1, 0, 0, 0, 0)},
      // oaxaca left this list 2026-09-08 (AER scope-2 B2): its row now sits
      // with pca above, because it reads the frame and posts e().
      {"honestdid", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"ritest", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"acreg", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"swindex", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // leebounds reads the frame and posts e() (replay reads it back); with
      // if/in it adds and drops a temporary qualifier column, so it writes too.
      {"leebounds", mk(0, 1, 1, 1, 1, 0, 0, 0)},

      // grstyle is genuinely effect-free: it is an accept-note, and nothing a
      // later statement can read changes.
      {"grstyle", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // Genuinely effect-free: they change nothing a later statement can read.
      // `sleep` burns wall-clock and `pause`/`cls`/`gr_edit` are inert here, so
      // unlike the rows above these do NOT have to refuse a parallel region.
      {"sleep", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"pause", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"cls", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      {"gr_edit", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // Refusal rows. They reach an arm that returns an error and nothing else,
      // so "no effects" is the honest classification -- not an oversight the
      // campaign ratchet should have to shelter behind an exemption.
      {"winexec", mk(0, 0, 0, 0, 0, 0, 0, 0)},
      // Served since gfx-histogram-kdensity-family: rclass (r(bin start width
      // min max N area n_x type)) and generate() adds two variables.
      {"twoway__histogram_gen", mk(0, 1, 0, 1, 1, 0, 0, 0)},
  };
  return t;
}

std::string firstToken(const std::string& s) {
  std::string t = strutil::trim(s);
  size_t i = 0;
  while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != ',') ++i;
  return t.substr(0, i);
}

// Strip the prefixes that do not change WHAT a statement does. quietly/noisily
// only change whether output is shown. `capture` is NOT here: it swallows
// errors and writes _rc, which changes control flow and macro state, so it is
// handled as its own refusal rather than stripped.
std::string stripQuietPrefixes(const std::string& stmt) {
  static const std::regex re(R"(^\s*(quietly|qui|quie|quiet|noisily|noi)\s+)", std::regex::icase);
  std::string s = stmt;
  std::smatch m;
  while (std::regex_search(s, m, re)) s = s.substr(static_cast<size_t>(m.length(0)));
  return strutil::trim(s);
}

}  // namespace

const Effects* lookupCommand(const std::string& canonical) {
  auto it = table().find(canonical);
  return it == table().end() ? nullptr : &it->second;
}

size_t classifiedCommandCount() { return table().size(); }

StatementSummary summarizeStatement(const std::string& stmt, SessionState& session) {
  StatementSummary out;
  const std::string s = stripQuietPrefixes(stmt);
  if (s.empty() || s[0] == '*') {  // blank or comment: no effects at all
    out.known = true;
    return out;
  }

  // A macro-headed statement builds its own command name at run time. §2.2's
  // first refusal rule, unchanged: we cannot know what it will be.
  if (s.find('`') == 0 || s.find('$') == 0) {
    out.why = "the command name is built from a macro";
    return out;
  }

  const std::string head = firstToken(s);
  const std::string headLower = strutil::lower(head);

  // `capture` is refused rather than stripped: it writes _rc (which the very
  // next line usually reads) and converts a halt into a continue, so it changes
  // both macro state and which later statements run at all.
  if (headLower == "capture" || headLower == "cap") {
    out.why = "`capture` sets _rc and changes which statements run";
    return out;
  }
  // Sort-order-dependent prefixes: `bysort` also mutates the frame.
  if (headLower == "by" || headLower == "bysort" || headLower == "bys") {
    out.why = "`" + head + "` depends on (and sets) sort order";
    return out;
  }

  // A user program call takes priority over the command table — a program named
  // `list` shadows the built-in, and summarizing the built-in would then be a
  // false positive of exactly the forbidden kind.
  if (session.programs.count(head) != 0 || session.programs.count(headLower) != 0) {
    const std::string key = session.programs.count(head) != 0 ? head : headLower;
    return summarizeProgram(key, session);
  }

  // `set` is two different commands wearing one word. `set obs N` sizes the
  // DATASET; `set seed`/`set more`/`set type` are session knobs. Classifying the
  // word as control refuses every task that merely sizes its own data, and
  // classifying it as data would wave through a real session mutation — so the
  // subcommand decides.
  if (headLower == "set") {
    const std::string rest = strutil::trim(s.substr(head.size()));
    const std::string sub = strutil::lower(firstToken(rest));
    if (sub == "obs") {
      out.known = true;
      out.eff.writesDataset = true;
      return out;
    }
    out.known = true;
    out.eff.control = true;
    return out;
  }

  // `clear` is split the same way. Bare `clear` empties the dataset only and
  // keeps the self-seeding row. Every subcommand (`all`, `*`, `mata`, `matrix`,
  // `results`, `programs`, ...) also empties a session store -- Mata, Stata
  // matrices, scalars, r()/e(), programs -- and a worker never gives that back,
  // so a later `getmata` or `matrix list` would find what serial had cleared.
  if (headLower == "clear") {
    const std::string sub = firstToken(strutil::trim(s.substr(head.size())));
    if (!sub.empty()) {
      out.why = "`clear " + sub + "` clears session state a worker never gives back";
      return out;
    }
  }

  // Session-global writes are recorded BY NAME. `scalar x = ...`,
  // `matrix b = e(b)`, `global g value` — the target is the first token after the
  // (optional) `define`. A target built from a macro cannot be reasoned about at
  // all, so it refuses rather than being recorded as some literal backtick text.
  if (headLower == "scalar" || headLower == "matrix" || headLower == "global") {
    std::string rest = strutil::trim(s.substr(head.size()));
    std::string firstWord = firstToken(rest);
    if (strutil::lower(firstWord) == "define") {
      rest = strutil::trim(rest.substr(firstWord.size()));
      firstWord = firstToken(rest);
    }
    // `global drop x` defines $drop, so the management forms below are
    // `scalar`/`matrix` subcommands only.
    const std::string sub = headLower == "global" ? std::string() : strutil::lower(firstWord);
    // `scalar drop s` / `matrix drop M` REMOVE a session-global, and a worker's
    // removal dies with it: measured on main, `scalar drop s` in an approved
    // region left s defined (`confirm scalar s` rc 0; serial r(111)). A drop is
    // a write to each name it lists, decided by name like any other; `_all`, a
    // wildcard, a macro-built name or an empty list cannot be, so it refuses.
    // `matrix rename a b` removes a and creates b: a write to both.
    if (sub == "drop" || sub == "rename") {
      std::istringstream names(strutil::trim(rest.substr(firstWord.size())));
      std::string n;
      while (names >> n) {
        if (n == "_all" || n.find_first_of("*?~`$") != std::string::npos) {
          out = StatementSummary{};
          out.why = "`" + head + " " + sub + " " + n +
                    "` changes session-globals it does not name, and a worker never gives "
                    "that back";
          return out;
        }
        out.eff.globalWrites.push_back(n);
      }
      if (out.eff.globalWrites.empty()) {
        out.why = "`" + head + " " + sub + "` names nothing, so what it changes is unknown";
        return out;
      }
      out.known = true;
      out.eff.writesGlobalState = true;
      return out;
    }
    // `matrix list`, `matrix dir` … are management forms, not definitions; they
    // neither create nor read a value another task could need.
    if (sub == "list" || sub == "dir") {
      out.known = true;
      return out;
    }
    std::string target = firstWord;
    const size_t eq = target.find('=');
    if (eq != std::string::npos) target = target.substr(0, eq);
    const size_t br = target.find('[');
    if (br != std::string::npos) target = target.substr(0, br);
    target = strutil::trim(target);
    out.known = true;
    out.eff.writesGlobalState = true;
    out.eff.readsResults = true;
    if (target.empty() || target.find('`') != std::string::npos ||
        target.find('$') != std::string::npos)
      out.eff.globalWriteUnresolved = true;
    else
      out.eff.globalWrites.push_back(target);
    return out;
  }

  // `display` is the one row where the blanket "may read the dataset" answer
  // refuses everything for nothing: `display "a literal"` touches no data, and a
  // program that announces itself before loading would otherwise never count as
  // self-seeding. Decide it from the text, conservatively — strip the quoted
  // segments and if ANY identifier character survives, assume it names something.
  if (headLower == "display" || headLower == "di" || headLower == "dis" ||
      headLower == "disp") {
    std::string rest = strutil::trim(s.substr(head.size()));
    std::string unquoted;
    bool inStr = false;
    for (char c : rest) {
      if (c == '"') { inStr = !inStr; continue; }
      if (!inStr) unquoted += c;
    }
    bool namesSomething = false;
    for (char c : unquoted)
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { namesSomething = true; break; }
    out.known = true;
    out.eff.readsDataset = namesSomething;
    out.eff.readsResults = namesSomething;
    return out;
  }

  // The Mata store (session.eval.mataMatrices) is neither in a worker's spec
  // (parallel.cpp ships globals, scalars, locals and programs) nor returned by
  // one. In a region, `mata` writes are silently lost and `getmata` finds
  // nothing, so both refuse by name. `mata:` with the colon attached is the
  // same command.
  if (headLower == "mata" || headLower.rfind("mata:", 0) == 0 || headLower == "getmata") {
    out.why = "`" + head + "` uses the Mata store, which a worker is neither given nor gives back";
    return out;
  }

  const std::string canonical = piet::registry::resolveCanonical(headLower);
  const Effects* e = lookupCommand(canonical.empty() ? headLower : canonical);
  if (e == nullptr) e = lookupCommand(headLower);
  if (e == nullptr) {
    out.why = "`" + head + "` is not in the parallel effect table";
    return out;
  }
  out.eff = *e;
  // BUG-699: planRegion decides session-global writes only BY NAME
  // (eff.globalWrites). A table row cannot name what it writes, so a
  // `writesGlobalState` row (renvars, tolower, move, recast, labmask,
  // renvarlab, mkmat) would otherwise reach the region as a write-free reader,
  // and the worker's change would be dropped without a word. A control row
  // already refuses under rule 4 and keeps that message.
  if (out.eff.writesGlobalState && out.eff.globalWrites.empty() && !out.eff.control) {
    out.why = "`" + head + "` writes session state that is not tracked by name, and a worker "
              "never gives it back";
    return out;
  }
  out.known = true;
  // Any file write whose path this layer cannot resolve. MVP: we never resolve
  // them, so a file writer is always "unresolved". Named, not hidden — see the
  // predicate's rule 10 and the design doc.
  if (out.eff.writesFiles) out.eff.filePathsUnresolved = true;
  return out;
}

StatementSummary summarizeProgram(const std::string& name, SessionState& session) {
  struct CacheEntry {
    unsigned long long epoch;
    StatementSummary sum;
  };
  // Per-session cache keyed on the program-table epoch: `program define` and
  // `program drop` bump it, so a redefinition can never be served a stale
  // summary. Recursion is guarded by an in-flight set — a program that calls
  // itself is refused rather than summarized forever.
  static thread_local std::map<std::string, CacheEntry> cache;
  static thread_local std::set<std::string> inFlight;

  StatementSummary out;
  auto it = session.programs.find(name);
  if (it == session.programs.end()) {
    out.why = "`" + name + "` is not a defined program";
    return out;
  }
  auto c = cache.find(name);
  if (c != cache.end() && c->second.epoch == session.programsEpoch) return c->second.sum;

  if (inFlight.count(name) != 0) {
    out.why = "`" + name + "` is recursive";
    return out;
  }
  inFlight.insert(name);

  bool sawDataTouch = false;
  // Names minted by `tempname`/`tempvar`/`tempfile` in THIS body. Stata drops a
  // tempname's object when the defining program ends, so a scalar or matrix
  // written through one never outlives the call — it is task-local by
  // construction, and in a worker it is process-local twice over. Without this
  // distinction the acceptance corpus refuses on `capture scalar `b_pooled' =
  // _b[...]`, which is the SAFEST shape a session-global write can have.
  std::set<std::string> tempNames;
  out.known = true;
  for (const std::string& raw : it->second.body) {
    const std::string line = strutil::trim(raw);
    if (line.empty() || line[0] == '*') continue;
    // Body control flow (`if`, loops, braces) does not itself have effects; its
    // BODY lines are visited by this same walk because ProgramDef.body is the
    // flattened, preprocessed text. A bare brace contributes nothing.
    if (line == "{" || line == "}") continue;
    const std::string head = strutil::lower(firstToken(line));
    // Body control FLOW is not a body EFFECT: the loop/branch keywords and the
    // in-loop jumps carry no effect of their own, and the lines they guard are
    // already visited by this same walk (ProgramDef.body is flattened text).
    if (head == "if" || head == "else" || head == "foreach" || head == "forvalues" ||
        head == "while" || head == "end" || head == "continue" || head == "break")
      continue;

    // CONTAINMENT — the same scope argument that lets a program's own locals be
    // dropped applies to two statements that are region-fatal at top level:
    //   * `capture` writes `_rc` and swallows an error. Both are confined to the
    //     program's own scope (program.h's pushScope/popScope pops the `_rc` the
    //     capture arm wrote), and which body lines run is internal to the task —
    //     identical whether the task runs in a worker or in the parent.
    //   * `exit` inside a body is Stata's program-RETURN, not a do-file halt. The
    //     acceptance corpus has 30 of them and every one is a task-local error
    //     path; treating them as halts refuses every real program there is.
    // Both stay region-fatal when they appear at TOP level, where they really do
    // change what the parent sees — that check lives in summarizeStatement.
    std::string bodyLine = line;
    {
      static const std::regex reCap(R"(^\s*(cap(?:ture)?)\s+)", std::regex::icase);
      std::smatch cm;
      while (std::regex_search(bodyLine, cm, reCap))
        bodyLine = strutil::trim(bodyLine.substr(static_cast<size_t>(cm.length(0))));
    }
    if (strutil::lower(firstToken(bodyLine)) == "exit") continue;
    // `by`/`bysort` is refused at top level because it both depends on and sets
    // sort order — inherited state a region's tasks would not share. Inside a
    // body that argument does not apply: the task owns whatever frame it built,
    // so sorting it is a task-local mutation with a deterministic result. Strip
    // the prefix, keep the sort as a dataset write, and summarize the real verb.
    // `preserve`/`restore` snapshot and restore the task's OWN frame. That is a
    // frame effect, and whether the parent can still see the region's frame is
    // already decided by the dataset successor walk — so inside a body they are
    // contained. NOT relaxed for `set`/`timer`/`estimates`/`log`/`do`/`program`:
    // those leave parent-visible session state that no walk here checks.
    {
      const std::string h0 = strutil::lower(firstToken(bodyLine));
      if (h0 == "preserve" || h0 == "restore") continue;
    }
    bool bodySorted = false;
    {
      const std::string h = strutil::lower(firstToken(bodyLine));
      if (h == "by" || h == "bys" || h == "bysort") {
        const size_t colon = bodyLine.find(':');
        if (colon == std::string::npos) {
          out.known = false;
          out.why = "in program `" + name + "`: a `" + h + "` prefix with no `:`";
          break;
        }
        bodySorted = true;
        bodyLine = strutil::trim(bodyLine.substr(colon + 1));
        if (bodyLine.empty()) continue;
      }
    }

    {
      const std::string h1 = strutil::lower(firstToken(bodyLine));
      if (h1 == "tempname" || h1 == "tempvar" || h1 == "tempfile") {
        std::istringstream ts(strutil::trim(bodyLine.substr(firstToken(bodyLine).size())));
        std::string tok;
        while (ts >> tok) tempNames.insert(tok);
      } else if (h1 == "scalar" || h1 == "matrix") {
        // A write through a tempname is contained; record nothing and move on.
        std::string rest = strutil::trim(bodyLine.substr(firstToken(bodyLine).size()));
        std::string tgt = firstToken(rest);
        if (tgt.size() > 2 && tgt.front() == '`' && tgt.back() == '\'') {
          const std::string inner = tgt.substr(1, tgt.size() - 2);
          if (tempNames.count(inner) != 0) continue;
        }
      }
    }

    StatementSummary st = summarizeStatement(bodyLine, session);
    // A file write whose destination is a `tempfile` minted in this body is
    // task-local for the same reason a tempname scalar is: the name is minted per
    // invocation and, in a worker, per process. The path is then knowably
    // disjoint without resolving it — which is the question rule 10 asks. A write
    // that ALSO names a literal path is not covered by this and stays unresolved.
    if (st.eff.writesFiles && !tempNames.empty()) {
      bool viaTemp = false;
      for (const std::string& tn : tempNames) {
        if (bodyLine.find("`" + tn + "'") != std::string::npos) { viaTemp = true; break; }
      }
      bool literalPath = false;
      bool inStr = false;
      std::string cur;
      for (char c : bodyLine) {
        if (c == '"') {
          if (inStr) {
            if (cur.find('/') != std::string::npos || cur.find('.') != std::string::npos)
              literalPath = true;
            cur.clear();
          }
          inStr = !inStr;
          continue;
        }
        if (inStr) cur += c;
      }
      if (viaTemp && !literalPath) {
        st.eff.writesFiles = false;
        st.eff.filePathsUnresolved = false;
      }
    }
    if (!st.known) {
      out.known = false;
      out.why = "in program `" + name + "`: " + st.why;
      break;
    }
    // The first line that touches data decides self-seeding for the whole
    // program: a body that opens with `use ..., clear` does not care what frame
    // it inherited, and one that reads before loading does.
    // A control statement inside a body is refused HERE, where the command name
    // is still in hand, rather than merged into a flag the region layer can only
    // describe as "touches session control". A refusal the user cannot act on is
    // barely better than a silent one.
    if (st.eff.control) {
      out.known = false;
      out.why = "in program `" + name + "`: `" + firstToken(bodyLine) +
                "` changes session state a worker would not give back";
      break;
    }
    // Same naming rule for session-global macro state. `scalar`/`matrix`/`global`
    // are not scoped the way locals are, so a body that writes one leaves it in
    // the session — and a worker's session is not the parent's. Named here while
    // the command is still in hand.
    // A session-global write inside a body is no longer fatal on sight: the
    // region predicate decides it by NAME (is it read after the region, and does
    // any task read one it did not write). An UNRESOLVABLE target still refuses
    // here, because nothing downstream can reason about a name that does not
    // exist until run time.
    if (st.eff.globalWriteUnresolved) {
      out.known = false;
      out.why = "in program `" + name + "`: `" + firstToken(bodyLine) +
                "` writes a session-global whose NAME is built from a macro";
      break;
    }
    if (bodySorted) st.eff.writesDataset = true;
    if (!sawDataTouch && (st.eff.readsDataset || st.eff.writesDataset)) {
      sawDataTouch = true;
      out.eff.datasetSelfSeed = st.eff.datasetSelfSeed;
    }
    out.eff.merge(st.eff);
  }

  // A program's own locals died with its scope, so they are not something the
  // caller (or a worker's parent) could ever have observed. Globals, scalars and
  // matrices are NOT dropped — those outlive the call.
  out.eff.writesLocals = false;

  inFlight.erase(name);
  cache[name] = CacheEntry{session.programsEpoch, out};
  return out;
}


bool mentionsWord(const std::string& text, const std::string& name) {
  if (name.empty()) return false;
  size_t pos = 0;
  while ((pos = text.find(name, pos)) != std::string::npos) {
    const bool leftOk =
        pos == 0 || (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
    const size_t end = pos + name.size();
    const bool rightOk =
        end >= text.size() ||
        (!std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '_');
    if (leftOk && rightOk) return true;
    pos = end;
  }
  return false;
}

bool programGlobalsAreSelfWritten(const std::string& program, const std::vector<std::string>& names,
                                  SessionState& session, std::string* why) {
  auto it = session.programs.find(program);
  if (it == session.programs.end()) {
    if (why) *why = "`" + program + "` is not a defined program";
    return false;
  }
  std::set<std::string> written;
  for (const std::string& raw : it->second.body) {
    const std::string line = strutil::trim(raw);
    if (line.empty() || line[0] == '*') continue;
    // A read of a name this body has not written yet is the only way a task
    // could observe another task's leftovers. Checked BEFORE recording this
    // line's own writes, so `scalar x = x + 1` (a genuine carry-over) refuses
    // while `scalar x = 1` followed by a use of x does not.
    for (const std::string& n : names) {
      if (written.count(n) != 0) continue;
      if (mentionsWord(line, n)) {
        if (why)
          *why = "in program `" + program + "`: reads session-global `" + n +
                 "` before writing it, so it could be observing another task";
        return false;
      }
    }
    StatementSummary st = summarizeStatement(line, session);
    for (const std::string& n : st.eff.globalWrites) written.insert(n);
    // A nested program call can write these too; recurse so its writes count.
    const std::string head = firstToken(line);
    if (session.programs.count(head) != 0) {
      std::string sub;
      if (!programGlobalsAreSelfWritten(head, names, session, &sub)) {
        if (why) *why = sub;
        return false;
      }
      StatementSummary ps = summarizeProgram(head, session);
      for (const std::string& n : ps.eff.globalWrites) written.insert(n);
    }
  }
  return true;
}

}  // namespace piet::parallel
