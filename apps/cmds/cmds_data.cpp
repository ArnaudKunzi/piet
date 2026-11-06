// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "io/dta_reader.h"  // read_dta/DtaResult (import_dta, append, merge with a .dta `using`)
#include "io/dta_writer.h"  // write_dta (export_dta — P1-A3 ARC 3 numeric writer)
#include "data/datasignature.h"  // data_signature (P1-A3 reverse-oracle head)
#include "core/ident.h"  // isIdentByte: a non-ASCII neighbour is not a word break
#include "core/stata_pctile.h"  // --winsor cutoffs
#include "core/eng_counters.h"  // WS-D2: merge_file_opens (S06 ARC-MRG)
#include "core/verb_diag.h"  // COBRA_VERB_DIAG: joinby/merge/keepif phase timing (measurement-only)
#include "data/frame_internal.h"  // frame_detail::round_to_store (round_column_to_store)

#include <cstdlib>  // getenv (COBRA_MERGE_IO_DIAG)
#include <fstream>  // cmd_setcol --ones-as reads its 0/1 values file
#include <limits>  // cmd_append --type-clash force: numeric missing (BUG-550)
#include <unordered_set>  // cmd_append --keepusing membership set (BUG-549)

namespace cecon {
namespace cli {

// ---- the egen vocabulary: one table per family, and the parsers read them --------
//
// These three lists used to live only inside cmd_eval's parser — a lambda, three
// hardcoded `fnm ==` tests, and an if/else chain — so anything that wanted to REPORT
// what egen accepts had to restate them and could silently under-report. The parse
// sites below now resolve against these tables, and `cobra functions --egen` dumps
// them, so a spelling is added in exactly one place.
//
// Three families because the three take different INPUT, which is what the dump's
// `kind` records: an expression (`--egen o=mean(x)`), a varlist (`--egen o=tag(a b)`;
// seq() takes no input at all), or a row of columns (`--rowegen o=rowmean(a,b)`).

// Aggregate-over-expression, optionally by(). Fourteen spellings, thirteen accumulators:
// `sum` and `total` are Stata's two names for one of them.
struct EgenExprSpec { const char* name; Frame::EgenFn fn; };
const EgenExprSpec EGEN_EXPR[] = {
    {"max", Frame::EgenFn::Max}, {"min", Frame::EgenFn::Min},
    {"mean", Frame::EgenFn::Mean}, {"median", Frame::EgenFn::Median},
    {"sum", Frame::EgenFn::Sum}, {"total", Frame::EgenFn::Sum},
    {"count", Frame::EgenFn::Count}, {"sd", Frame::EgenFn::Sd},
    {"rank", Frame::EgenFn::Rank}, {"pctile", Frame::EgenFn::Pctile},
    {"std", Frame::EgenFn::Std}, {"iqr", Frame::EgenFn::Iqr},
    // BUG-1001: pc() (egen's pc) -- the percent (or, under `prop`, the proportion)
    // of the group total. Oracle w2ap_egen_family eg_pc_*.
    {"pc", Frame::EgenFn::Pc},
    // B4/W6. `var` is egenmore (SSC / corpus-bundled), NOT StataCorp — ACCEPTED-NAMED:
    // the third-party ado's spelling and semantics are the contract. See Frame::EgenFn
    // in src/data/frame.h for the reopening condition. rank's track/field/unique are
    // OPTIONS on rank, resolved in the dispatch below, not separate spellings.
    {"var", Frame::EgenFn::Var},
};

// Varlist / no-input egen functions. Names only: each reaches a different Frame entry
// point with its own option parsing, so the dispatch below stays explicit and this
// table is the membership test that routes to it.
const char* const EGEN_VARLIST[] = {"tag", "group", "seq"};

// B4/W6 functions that reach their own Frame entry point with their own option
// grammar (mode's minmode/maxmode/nummode/missing, cut's at/group/icodes/label,
// nvals' varlist + missing). Named here so `cobra functions --egen` reports them —
// a capability probe that cannot see a function reads as a gap.
struct EgenSpecialSpec { const char* name; const char* kind; };
const EgenSpecialSpec EGEN_SPECIAL[] = {
    {"mode", "expr"}, {"cut", "varname"}, {"nvals", "varlist"},
    // B4/W7: concat's inputs are a VARLIST (strings appended raw, numerics through
    // string()), so it never reaches the expression family.
    {"concat", "varlist"},
};

// Row functions across columns (--rowegen). Nine spellings, seven operations:
// `rowtotal`, `rowsum` and `rsum` are three names for one of them (rsum is
// rowtotal's Stata 8 name; BUG-1001).
struct EgenRowSpec { const char* name; Frame::RowFn fn; };
const EgenRowSpec EGEN_ROW[] = {
    {"rowmean", Frame::RowFn::Mean}, {"rowsd", Frame::RowFn::Sd},
    {"rowtotal", Frame::RowFn::Total}, {"rowsum", Frame::RowFn::Total},
    {"rsum", Frame::RowFn::Total},
    {"rowmax", Frame::RowFn::Max}, {"rowmin", Frame::RowFn::Min},
    {"rowmiss", Frame::RowFn::Miss}, {"rownonmiss", Frame::RowFn::NonMiss},
};

const std::vector<EgenFnInfo>& egen_function_table() {
    static const std::vector<EgenFnInfo> T = [] {
        std::vector<EgenFnInfo> v;
        for (const EgenExprSpec& s : EGEN_EXPR) v.push_back({s.name, "expr"});
        for (const char* n : EGEN_VARLIST) v.push_back({n, "varlist"});
        for (const EgenSpecialSpec& s2 : EGEN_SPECIAL) v.push_back({s2.name, s2.kind});
        for (const EgenRowSpec& s : EGEN_ROW) v.push_back({s.name, "row"});
        return v;
    }();
    return T;
}

// BUG-471 T1: the --store-type token of every verb that stores a numeric column
// (eval, setcol, predict -- cmds_postest.cpp declares it). Absent = double, so a
// caller that does not ask keeps its bytes.
StoreType parse_store_type(const std::vector<std::string>& a, const char* verb) {
    const std::string sv = opt(a, "--store-type");
    if (sv == "float") return StoreType::Float;
    if (sv == "byte") return StoreType::Byte;
    if (sv == "int") return StoreType::Int;
    if (sv == "long") return StoreType::Long;
    if (!sv.empty() && sv != "double")
        throw EngineError("bad_input", std::string(verb) +
                                           ": --store-type expects byte|int|long|float|double");
    return StoreType::Double;
}

// BUG-924: the store boundary for a column built outside Frame's own writers
// (setcol's payload, predict's fitted values): the ONE rule Frame::set_or_replace
// applies (frame_detail::round_to_store), never a second one.
void round_column_to_store(double* v, size_t n, StoreType st) {
    if (st == StoreType::Double) return;
    for (size_t i = 0; i < n; ++i) v[i] = frame_detail::round_to_store(v[i], st);
}

// cobra eval — Stata-style data prep, applied in command-line order so dependent
// transforms work (e.g. an egen that references a column made by an earlier bygen).
int cmd_eval(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), out = opt(a, "--out");
    if (data_missing(data)) {
        std::fprintf(stderr,
            "usage: cobra eval --data in.hbin [ops...] [--summarize v1,v2 [--detail] [--fweight W | --aweight W]] [--out f.hbin]\n"
            "  ops (applied in order): --xtset id,time | --tsset time | --tsdelta D\n"
            "    | --tsma \"o=in,lags,1,leads\"\n"
            "    (--xtset/--tsset also enable L./F./D./S. inside --gen/--egen/--keepif exprs)\n"
            "    | --gen \"o=expr\" [--gen-strict, reject if o already exists] | --egen \"o=fn(expr), by(g..)\"\n"
            "        (fn over expr: max min mean median sum count sd rank std iqr | pctile(x),p(#))\n"
            "        (fn over varlist: tag(v..) group(v..)[,missing] | seq()[,by(g..)] — running 1..N)\n"
            "        (fn over varlist: concat(v..)[,punct(s) format(%%fmt) maxlength(#)] — text result)\n"
            "    | --rowegen \"o=rowfn(v1,v2,..)[,missing]\" (rowmean rowsd rowtotal/rsum rowmax rowmin rowmiss rownonmiss)\n"
            "    | --mkspline \"stub=var, cubic [, nknots(5)] [, knots(v1,v2,..)]\" (restricted cubic spline basis)\n"
            "    | --bygen \"o=expr, by(g..), sort(t..)\" | --winsor \"o=src,plo,phi\"\n"
            "    | --xtile \"out=var, nq(k) [w(wvar)]|cutpoints(cvar)\" | --ipolate \"out=yvar xvar [, by(g..)] [, epolate]\"\n"
            "    | --drop v | --keepif \"expr\"\n"
            "  --fweight W applies to every var in --summarize (Stata [fw=W]; integer weights only;\n"
            "    not combinable with --detail)\n"
            "  --aweight W same, for Stata [aw=W]: N stays the ROW count, sum_w is reported,\n"
            "    non-integer weights are legal, and the variance divides by rows-1\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    const int64_t n0 = fr.n_rows();
    const int64_t overflow0 = fr.store_overflow();
    bool detail = has(a, "--detail");
    // BUG-112(1) engine-half: opt-in — a bare `--gen` stays create-or-replace (every
    // existing caller, including PIET's `replace` verb which ALSO issues `--gen`, is
    // unaffected); only a caller that passes `--gen-strict` gets Stata `generate`'s
    // reject-if-exists semantics.
    bool genStrict = has(a, "--gen-strict");
    std::string fweight = opt(a, "--fweight");  // applies to every var in --summarize below
    // [aw=] travels down the same weighted path as [fw=], differing only in the N/
    // integrality/variance-divisor conventions (Frame::summarize's `analytic`). Passing
    // both is a contradiction, not a precedence question, so it is rejected.
    std::string aweight = opt(a, "--aweight");
    // [iw=]: Stata's importance weight -- N is the ROW count, sum_w/sum/mean are the
    // weighted totals and Var = Sum w(x-m)^2/(Sum w - 1) (w2aj_qualifiers.log:826
    // qf_sw_iw_*: N 12, sum_w 28.5); any real weight is legal.
    std::string iweight = opt(a, "--iweight");
    if ((!fweight.empty()) + (!aweight.empty()) + (!iweight.empty()) > 1)
        throw EngineError("bad_input", "eval --summarize: --fweight, --aweight and --iweight are mutually exclusive");
    const bool analytic_w = !aweight.empty();
    const bool importance_w = !iweight.empty();
    const std::string& sumw = analytic_w ? aweight : importance_w ? iweight : fweight;
    std::vector<std::string> summvars;

    auto egen_fn = [](const std::string& f, Frame::EgenFn& o) -> bool {
        for (const EgenExprSpec& s : EGEN_EXPR) if (f == s.name) { o = s.fn; return true; }
        return false;
    };

    // ---- time-series context: a PRE-pass, deliberately not an ordered op --------
    // BUG-369: the loop below applies its flags in ARGV ORDER, so an --xtset that
    // arrives after --gen would not be in effect while the gen expression is parsed.
    // tsset/xtset is CONTEXT (which columns are time and panel), not a step in a
    // pipeline, and every producer appends it at the end — PIET's
    // translate::injectTsContext builds the estimator argv that way, and the
    // expression surfaces now reuse it. Reading it up front is what makes
    // `--gen "ly=L.y" --tsset t` mean the same thing as `--tsset t --gen "ly=L.y"`.
    // The loop keeps a branch for these flags purely to consume their value.
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == "--tsdelta") fr.set_ts_delta(std::stod(a[i + 1]));
    }
    // Stata's `if` qualifier, carried SEPARATELY rather than folded into the
    // expression. PIET normally folds it (`x = cond((c),(rhs),.)`) and that is
    // exactly equivalent -- except for sum(), a running total, where folding
    // makes the total run over rows the qualifier excluded. Read as context
    // (like --tsset) so argv order cannot matter.
    const std::string genMask = opt(a, "--gen-mask");
    // BUG-841/842: Stata's `in a/b` as its own channel, so group() can number within
    // the range while an `if` (even `_n>=a & _n<=b`) does not renumber; and
    // --gen-replace, replace's rule that a row the qualifiers exclude keeps its value.
    // Both are context for every --gen/--bygen of this invocation, like --gen-mask.
    GenQualifier genQual;
    {
        const std::string gi = opt(a, "--gen-in");
        if (!gi.empty()) {
            const size_t slash = gi.find('/');
            auto bound = [&](const std::string& t) -> int64_t {
                const std::string u = trimx(t);
                if (u.empty() || u.find_first_not_of("0123456789") != std::string::npos)
                    throw EngineError("bad_input", "eval: --gen-in expects FIRST/LAST "
                                                   "observation numbers, got '" + gi + "'");
                return std::stoll(u);
            };
            if (slash == std::string::npos)
                throw EngineError("bad_input", "eval: --gen-in expects FIRST/LAST "
                                               "observation numbers, got '" + gi + "'");
            genQual.in_first = bound(gi.substr(0, slash));
            genQual.in_last = bound(gi.substr(slash + 1));
            if (genQual.in_first < 1)
                throw EngineError("bad_input", "eval: --gen-in's first observation must be "
                                               "at least 1, got '" + gi + "'");
        }
        genQual.keep_unselected = has(a, "--gen-replace");
    }
    // BUG-471 T1: the Stata storage type every --gen/--egen/--rowegen/--bygen output
    // of this invocation is stored as (Frame::set_or_replace rounds to it). Read once,
    // as context. Absent = double, so every existing caller's bytes are unchanged.
    // egen tag()/seq()/mode() ignore it (as their ados do); the replace-built egen results
    // refuse byte/int/long (Frame::row_egen, egen_by rank, egen_cut).
    const StoreType storeType = parse_store_type(a, "eval");
    // The tsset variable's display format, for tin()/twithin() literals.
    { const std::string tf = opt(a, "--tsfmt"); if (!tf.empty()) fr.set_ts_format(tf); }
    // B3(c): draw coordinates. `set seed` is remembered by the PIET session --
    // this process is a subprocess and remembers nothing -- so the seed and a
    // per-call stream number ride the argv the way tsset does. They belong to
    // this call: a Frame expression call spends them when it returns, and the
    // guard drops them on every other way out, so a call that fails before any
    // expression ran (the --xtset/--tsset pass below, a malformed --rng-mt64 or
    // --gen) does not lend them to the next `cobra serve` request (BUG-1200).
    struct RngCoordsGuard {
        ~RngCoordsGuard() { cecon::clear_rng_coords(); }
    } rng_coords_guard;
    {
        const std::string sd = opt(a, "--rng-seed"), st = opt(a, "--rng-stream");
        if (!sd.empty() || !st.empty())
            cecon::set_rng_coords(sd.empty() ? 123456789LL : std::stoll(sd),
                                  st.empty() ? 0LL : std::stoll(st));
    }
    // Stata's session stream (core/rng_mt64.h): PIET sends its c(rngstate)-format
    // state, the draws come off it in evaluation order, and the response says how
    // many raw draws were taken (`rng_draws`) so the session can skip past them.
    // The guard turns the stream off on every exit, so a resident `cobra serve`
    // never lends it to the next request.
    struct Mt64CallGuard {
        ~Mt64CallGuard() { (void)cecon::end_rng_mt64(); }
    } mt64_guard;
    bool mt64_on = false;
    {
        const std::string ms = opt(a, "--rng-mt64");
        if (!ms.empty()) {
            if (!cecon::begin_rng_mt64(ms))
                throw EngineError("bad_input", "eval: --rng-mt64 is not an mt64 state string");
            mt64_on = true;
        }
    }
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == "--xtset") {
            auto pv = split_csv(a[i + 1]);
            if (pv.size() != 2) { std::fprintf(stderr, "--xtset expects id,time\n"); return 2; }
            fr.xtset(pv[0], pv[1]);
        } else if (a[i] == "--tsset") {
            // A PURE time series (Stata `tsset t`, no panel). Only --xtset existed
            // here before, so PIET's injectTsContext — which emits --tsset for a
            // session with a timevar and no panelvar — had nothing to talk to.
            fr.tsset(a[i + 1]);
        }
    }

    for (size_t i = 0; i + 1 < a.size(); ++i) {
        const std::string& f = a[i];
        const std::string v = a[i + 1];
        const size_t at = i;
        bool op = true;  // f is one of the ops below, and v its value
        if (f == "--xtset" || f == "--tsset" || f == "--tsdelta") {
            ++i;  // already applied by the ts pre-pass above — see its comment
        } else if (f == "--tsma") {
            size_t eq = v.find('=');
            auto p = split_csv(v.substr(eq + 1));
            if (eq == std::string::npos || p.size() != 4) {
                std::fprintf(stderr, "--tsma expects \"out=in,lags,1,leads\"\n"); return 2; }
            // BUG-378: the third CSV field is Stata's centre weight and this used
            // to be parsed and ignored — the engine always included the focal
            // observation. window(1) in real Stata is (1/1)*[x(t-1) + 0*x(t)],
            // so a dropped centre weight is a silently different window.
            fr.tssmooth_ma(trimx(v.substr(0, eq)), p[0], std::stoi(p[1]), std::stoi(p[3]),
                           /*include_focal=*/std::stoi(p[2]) != 0); ++i;
        } else if (f == "--gen") {
            size_t eq = find_assign(v);
            if (eq == std::string::npos) { std::fprintf(stderr, "bad --gen '%s'\n", v.c_str()); return 2; }
            fr.generate(trimx(v.substr(0, eq)), v.substr(eq + 1), genStrict, genMask, storeType,
                        genQual); ++i;
        } else if (f == "--egen") {
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            size_t fp = rhs.find('(');
            std::string fnm = trimx(rhs.substr(0, fp));
            int depth = 1; size_t p = fp + 1, st = p;
            for (; p < rhs.size() && depth > 0; ++p) { if (rhs[p] == '(') ++depth; else if (rhs[p] == ')') --depth; }
            std::string inner = rhs.substr(st, p - 1 - st);
            std::string opts = rhs.substr(p);  // tail after the inner ')': options like , by(..) p(..)
            std::vector<std::string> by = split_vars(paren_arg(opts, "by"));
            // Varlist / no-arg egen functions (tag/group/seq) — inner is a VARLIST, not an expr.
            // EGEN_VARLIST owns the membership test so the vocabulary lives in one
            // place; the chain still dispatches by name, because each of the three
            // reaches a different Frame entry point with its own options.
            bool varlist_egen = false;
            for (const char* n : EGEN_VARLIST) if (fnm == n) { varlist_egen = true; break; }
            // ---- B4/W6 -----------------------------------------------------
            // A word test that cannot be fooled by a substring: "field" must not
            // match inside a by() variable called `fieldid`, and "label" must not
            // match inside "labeled". Options are bare words in this tail.
            auto has_word = [](const std::string& hay, const char* w) {
                const std::string needle(w);
                for (size_t p2 = hay.find(needle); p2 != std::string::npos;
                     p2 = hay.find(needle, p2 + 1)) {
                    const bool lok = (p2 == 0) || !cecon::ident::isIdentByte(hay[p2 - 1]);
                    const size_t e = p2 + needle.size();
                    const bool rok = (e >= hay.size()) || !cecon::ident::isIdentByte(hay[e]);
                    if (lok && rok) return true;
                }
                return false;
            };
            if (fnm == "mode") {
                Frame::ModeOpts mo;
                mo.minmode = has_word(opts, "minmode");
                mo.maxmode = has_word(opts, "maxmode");
                mo.missing_as_value = has_word(opts, "missing");
                std::string nm = paren_arg(opts, "nummode");
                if (!nm.empty()) mo.nummode = std::stoi(trimx(nm));
                // egen mode stores in the source's own type: --store-type does not reach it.
                fr.egen_mode(outn, inner, by, mo); ++i;
            } else if (fnm == "nvals") {
                fr.egen_nvals(outn, split_vars(inner), by, has_word(opts, "missing"), storeType); ++i;
            } else if (fnm == "concat") {
                // Stata refuses a `by` prefix on concat() with r(190), as it does
                // for cut() (help egen: "may not be combined with by"; BUG-1106).
                if (!by.empty())
                    throw EngineError("bad_input",
                        "egen concat(): may not be combined with by");
                Frame::ConcatOpts co;
                // punct("_") / format(%9.2f) arrive with their quotes when the caller
                // wrote any; strip ONE matching pair, never trim the content itself —
                // a separator of "  " is a legitimate two-space separator.
                auto unquote = [](std::string s) {
                    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                                          (s.front() == '\'' && s.back() == '\'')))
                        s = s.substr(1, s.size() - 2);
                    return s;
                };
                co.punct = unquote(paren_arg(opts, "punct"));
                co.format = trimx(unquote(paren_arg(opts, "format")));
                co.decode = has_word(opts, "decode");
                // Only the long spelling is read. paren_arg does a plain substring
                // find, so a bare `length(` probe would also match inside
                // `maxlength(` (the mkspline knots/nknots hazard above) — concat has
                // no `length()` option, so there is nothing to disambiguate and
                // nothing that could be read twice.
                std::string ml = trimx(paren_arg(opts, "maxlength"));
                if (!ml.empty()) {
                    // Stata accepts only an integer in [1, 32000]; anything else, a
                    // non-integer included, is r(198) "invalid maxlength( )" (egen-six
                    // k4; measured on StataBE) — so "12abc" must be rejected,
                    // not silently read as 12.
                    bool ok = true;
                    for (size_t q = 0; q < ml.size(); ++q)
                        if (!std::isdigit(static_cast<unsigned char>(ml[q])) &&
                            !(q == 0 && (ml[q] == '-' || ml[q] == '+'))) ok = false;
                    long parsed = 0;
                    if (ok) { try { parsed = std::stol(ml); } catch (const std::exception&) { ok = false; } }
                    if (!ok || parsed < 1 || parsed > 32000)
                        throw EngineError("bad_input", "egen concat(): invalid maxlength( )");
                    co.maxlength = static_cast<int>(parsed);
                }
                // Text result: --store-type (a numeric storage type) does not reach it.
                fr.egen_concat(outn, split_vars(inner), co); ++i;
            } else if (fnm == "cut") {
                // Stata refuses a `by` prefix on cut() with r(190) — captured in
                // egen-six as `c7_by_prefix` rc 190.
                if (!by.empty())
                    throw EngineError("bad_input", "egen cut(): may not be combined with by");
                Frame::CutOpts co;
                std::string at = paren_arg(opts, "at");
                if (!at.empty()) {
                    // Stata numlist accepts a `from(step)to` range — `at(0(.1)1)`. NOT
                    // supported here on purpose: the egen-six capture shows Stata's own
                    // answer for that form is the FLOAT-rounded bin (d = 0.29999999999
                    // lands in [0.3,0.4), icode 3, not 2), and a double-precision
                    // expansion of the range would silently disagree. Refuse until a
                    // capture pins where the rounding happens.
                    if (at.find('(') != std::string::npos)
                        throw EngineError("bad_input",
                            "egen cut(): at() numlist ranges like 0(.1)1 are not supported — "
                            "list the edges explicitly");
                    for (const auto& t : split_vars(at)) co.at.push_back(std::stod(t));
                }
                std::string gp = paren_arg(opts, "group");
                if (!gp.empty()) co.group = std::stoi(trimx(gp));
                co.icodes = has_word(opts, "icodes");
                co.label = has_word(opts, "label");
                fr.egen_cut(outn, trimx(inner), co, storeType); ++i;
            } else if (varlist_egen && fnm == "tag") {
                // _gtag.ado ignores the requested type: --store-type does not reach it.
                fr.egen_tag(outn, split_vars(inner)); ++i;
            } else if (varlist_egen && fnm == "group") {
                bool miss = (opts.find("missing") != std::string::npos);
                fr.egen_group(outn, split_vars(inner), miss, storeType); ++i;
            } else if (varlist_egen && fnm == "seq") {
                // BUG-433: from()/to()/block() and their documented one-letter
                // spellings. `inner` stays ignored — seq has no input variable.
                auto num = [&](const char* a, const char* b, double dflt, bool* seen) {
                    std::string v = paren_arg(opts, a);
                    if (v.empty()) v = paren_arg(opts, b);
                    if (v.empty()) { if (seen) *seen = false; return dflt; }
                    if (seen) *seen = true;
                    return std::stod(trimx(v));
                };
                bool has_to = false;
                const double sq_from = num("from", "f", 1.0, nullptr);
                const double sq_to = num("to", "t", 0.0, &has_to);
                const double sq_block = num("block", "b", 1.0, nullptr);
                // Stata's seq() ignores the requested type: --store-type does not reach it.
                fr.egen_seq(outn, by, sq_from, sq_to, has_to,
                            static_cast<int64_t>(sq_block)); ++i;
            } else if (varlist_egen) {
                // Listed in EGEN_VARLIST but with no branch above: refuse, rather than
                // fall into the expression family, which would read a varlist as an expr.
                std::fprintf(stderr, "--egen unknown fn '%s'\n", fnm.c_str()); return 2;
            } else {
                Frame::EgenFn ef;
                if (!egen_fn(fnm, ef)) { std::fprintf(stderr, "--egen unknown fn '%s'\n", fnm.c_str()); return 2; }
                // B4/W6: rank's three tie rules are OPTIONS, so they are resolved here
                // rather than spelled as separate function names (Stata spells them
                // `egen r = rank(x), track|field|unique`). Oracle captures b and c.
                if (ef == Frame::EgenFn::Rank) {
                    const bool trk = has_word(opts, "track"), fld = has_word(opts, "field"),
                               unq = has_word(opts, "unique");
                    if (static_cast<int>(trk) + static_cast<int>(fld) + static_cast<int>(unq) > 1)
                        throw EngineError("bad_input",
                            "egen rank(): track, field and unique are mutually exclusive");
                    if (trk) ef = Frame::EgenFn::RankTrack;
                    else if (fld) ef = Frame::EgenFn::RankField;
                    else if (unq) ef = Frame::EgenFn::RankUnique;
                }
                double pp = 50.0;  // pctile percentile (default 50)
                if (ef == Frame::EgenFn::Pctile) {
                    std::string ps = paren_arg(opts, "p");
                    if (!ps.empty()) pp = std::stod(trimx(ps));
                }
                // BUG-1001: the options the ados read (see Frame::EgenOpts); each is
                // read only for its own function, so `missing` on max() stays inert
                // here exactly as before (PIET's per-function net refuses it).
                Frame::EgenOpts eo;
                if (ef == Frame::EgenFn::Sum) eo.missing = has_word(opts, "missing");
                if (ef == Frame::EgenFn::Pc) eo.pc_prop = has_word(opts, "prop");
                if (ef == Frame::EgenFn::Sum || ef == Frame::EgenFn::Mean)
                    eo.data_order = has_word(opts, "dataorder");
                if (ef == Frame::EgenFn::Std) {
                    const std::string mv = trimx(paren_arg(opts, "mean"));
                    const std::string sv = trimx(paren_arg(opts, "sd"));
                    if (!mv.empty()) eo.std_mean = std::stod(mv);
                    if (!sv.empty()) eo.std_sd = std::stod(sv);
                    if (!(eo.std_sd > 0.0))
                        throw EngineError("bad_input", "egen std(): sd() must be positive");
                }
                fr.egen_by(outn, ef, inner, by, pp, storeType, eo); ++i;
            }
        } else if (f == "--rowegen") {
            // egen row function across columns: out = rowfn(v1,v2,...)
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            size_t fp = rhs.find('(');
            std::string fnm = trimx(rhs.substr(0, fp));
            size_t rp = rhs.rfind(')');
            std::vector<std::string> vars = split_vars(rhs.substr(fp + 1, rp - fp - 1));
            const EgenRowSpec* rs = nullptr;
            for (const EgenRowSpec& s : EGEN_ROW) if (fnm == s.name) { rs = &s; break; }
            if (!rs) { std::fprintf(stderr, "--rowegen unknown fn '%s' (rowmean/rowsd/rowtotal/rsum/rowmax/rowmin/rowmiss/rownonmiss)\n", fnm.c_str()); return 2; }
            // BUG-1001: `, missing` after the varlist -- rowtotal()/rsum()'s only
            // option (help egen).
            const std::string tail = rhs.substr(rp + 1);
            const bool row_missing =
                rs->fn == Frame::RowFn::Total && tail.find("missing") != std::string::npos;
            // `, strok`: rownonmiss()'s only option (help egen).
            const bool row_strok =
                rs->fn == Frame::RowFn::NonMiss && tail.find("strok") != std::string::npos;
            fr.row_egen(outn, rs->fn, vars, storeType, row_missing, row_strok); ++i;
        } else if (f == "--mkspline") {
            // classic mkspline ,cubic (RCS): out = stub; generates stub1..stub(nk-1)
            size_t eq = find_assign(v);
            std::string stub = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            size_t comma = rhs.find(',');
            std::string var = trimx(comma == std::string::npos ? rhs : rhs.substr(0, comma));
            std::string opts = (comma == std::string::npos) ? "" : rhs.substr(comma + 1);
            int nk = 5;
            std::string nks = paren_arg(opts, "nknots");
            if (!nks.empty()) nk = std::stoi(trimx(nks));
            // paren_arg() does a plain substring find of "knots(", which also matches inside
            // "nknots(...)" (that's "n" + "knots("). A bare paren_arg(opts,"knots") on an opts
            // string carrying nknots(k) would steal its digits as a bogus 1-element explicit
            // knot list, which mkspline_cubic then rejects as "need at least 3 knots" — the
            // live repro (`--mkspline "sp=price, cubic, nknots(4)"`). Strip the nknots(...)
            // span first so only a genuine standalone knots() option can match.
            std::string opts_kl = opts;
            size_t nkp = opts_kl.find("nknots(");
            if (nkp != std::string::npos) {
                size_t p = nkp + 7; int depth = 1;
                for (; p < opts_kl.size() && depth > 0; ++p) { if (opts_kl[p]=='(') ++depth; else if (opts_kl[p]==')') --depth; }
                opts_kl.erase(nkp, p - nkp);
            }
            std::vector<double> knots;
            std::string kl = paren_arg(opts_kl, "knots");
            if (!kl.empty()) for (const auto& s : split_vars(kl)) knots.push_back(std::stod(s));
            auto nm = fr.mkspline_cubic(stub, var, nk, knots);
            std::fprintf(stderr, "mkspline: generated %zu RCS basis cols (%s..)\n", nm.size(), nm.empty() ? "" : nm.front().c_str());
            ++i;
        } else if (f == "--bygen") {
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            int depth = 0; size_t cpos = std::string::npos;
            for (size_t q = 0; q < rhs.size(); ++q) {
                char ch = rhs[q];
                if (ch == '(' || ch == '[') ++depth; else if (ch == ')' || ch == ']') --depth;
                else if (ch == ',' && depth == 0) { cpos = q; break; }
            }
            std::string expr = (cpos == std::string::npos) ? rhs : trimx(rhs.substr(0, cpos));
            std::string opts = (cpos == std::string::npos) ? "" : rhs.substr(cpos + 1);
            fr.bygen(outn, expr, split_vars(paren_arg(opts, "by")), split_vars(paren_arg(opts, "sort")),
                     genMask, storeType, genQual); ++i;
        } else if (f == "--winsor") {
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq));
            auto p = split_csv(v.substr(eq + 1));  // src, plo, phi
            if (p.size() != 3) { std::fprintf(stderr, "--winsor expects \"out=src,plo,phi\"\n"); return 2; }
            // Stata's _pctile at ANY cut (core/stata_pctile.h): the Summary
            // fields hold nine percentiles, and the old lookup answered 0 for every
            // other, so `winsor2 x, cuts(0 98)` set each value to 0 (AER 007's
            // text_prev_24hr_w3; StataBE uni-oracle/wq.do).
            double lo, hi;
            {
                const F64Reader r = fr.col(p[0]).f64.reader();
                std::vector<double> vals;
                vals.reserve(static_cast<std::size_t>(fr.n_rows()));
                for (int64_t q = 0; q < fr.n_rows(); ++q) {
                    const double x = r[static_cast<std::size_t>(q)];
                    if (!std::isnan(x)) vals.push_back(x);
                }
                std::sort(vals.begin(), vals.end());
                lo = cecon::stata_pctile_sorted(vals, std::stod(p[1]));
                hi = cecon::stata_pctile_sorted(vals, std::stod(p[2]));
            }
            char buf[256];
            std::snprintf(buf, sizeof buf,
                "cond(missing(%s), ., cond(%s>%.17g, %.17g, cond(%s<%.17g, %.17g, %s)))",
                p[0].c_str(), p[0].c_str(), hi, hi, p[0].c_str(), lo, lo, p[0].c_str());
            fr.generate(outn, buf, false, "", storeType); ++i;  // the source's type (winsor2 clonevar)
        } else if (f == "--xtile") {
            // xtile out = var, nq(k)
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            size_t comma = rhs.find(',');
            std::string var = trimx(comma == std::string::npos ? rhs : rhs.substr(0, comma));
            std::string opts = (comma == std::string::npos) ? "" : rhs.substr(comma + 1);
            // BUG-371: cutpoints() takes precedence when given — it is a DIFFERENT
            // source of bin edges, not a modifier of nquantiles(), and Stata treats
            // the two as mutually exclusive.
            std::string cuts = paren_arg(opts, "cutpoints");
            if (!cuts.empty()) {
                fr.xtile_cuts(outn, var, trimx(cuts)); ++i;
            } else {
                std::string nqs = paren_arg(opts, "nq");
                if (nqs.empty()) nqs = paren_arg(opts, "nquantiles");
                int nq = nqs.empty() ? 4 : std::stoi(trimx(nqs));
                // BUG-1015: `w(wvar)` -- the [aw=]/[fw=] weight of the cutpoints.
                fr.xtile(outn, var, nq, trimx(paren_arg(opts, "w"))); ++i;
            }
        } else if (f == "--ipolate") {
            // ipolate out = yvar xvar [, by(g..)] [, epolate]  (Stata `ipolate yvar xvar, gen() [epolate]`)
            size_t eq = find_assign(v);
            std::string outn = trimx(v.substr(0, eq)), rhs = trimx(v.substr(eq + 1));
            size_t comma = rhs.find(',');
            std::string yx = trimx(comma == std::string::npos ? rhs : rhs.substr(0, comma));
            std::string opts = (comma == std::string::npos) ? "" : rhs.substr(comma + 1);
            std::vector<std::string> yxv = split_vars(yx);
            if (yxv.size() != 2) {
                std::fprintf(stderr, "--ipolate expects \"out=yvar xvar[, by(g..)][, epolate]\"\n"); return 2; }
            bool epol = opts.find("epolate") != std::string::npos;
            fr.ipolate(outn, yxv[0], yxv[1], epol, split_vars(paren_arg(opts, "by"))); ++i;
        } else if (f == "--drop") {
            fr.drop(v); ++i;
        } else if (f == "--keepif") {
            // COBRA_VERB_DIAG (measurement-only, see src/core/verb_diag.h): the
            // in-memory `eval --keepif` row filter (Stata keep if/drop if) —
            // the sibling of maybe_keepif's scratch-writing path in
            // apps/cmds/common.cpp, which times the same Frame::keep_if kernel
            // call but around a full .hbin scratch write. No scratch file here,
            // so scratch_bytes is always 0. OFF path costs one bool check.
            const bool kVerbDiag = cecon::verb_diag_enabled();
            const int64_t rows_in = kVerbDiag ? fr.n_rows() : 0;
            const auto t0 = cecon::verb_diag_now(kVerbDiag);
            fr.keep_if(v);
            if (kVerbDiag) {
                const double eval_ms = cecon::verb_diag_ms(kVerbDiag, t0);
                const int64_t rows_out = fr.n_rows();
                cecon::verb_diag_emit(
                    "[verb-diag] verb=keepif_inline total_ms=%.3f eval_ms=%.3f other_ms=0.000 "
                    "scratch_bytes=0 rows_in=%lld rows_out=%lld\n",
                    eval_ms, eval_ms, static_cast<long long>(rows_in),
                    static_cast<long long>(rows_out));
            }
            ++i;
        } else if (f == "--summarize") {
            for (const std::string& sv : split_csv(v)) summvars.push_back(sv); ++i;
        } else {
            op = false;
        }
        // BUG-1181: this scanner reads its ops without opt()/has(), so it marks
        // them consumed itself; the E6 net (dispatch_guard) then refuses any other
        // `--flag`, which it used to ignore -- `--store-type` sent to an engine
        // that predates it stored doubles at rc 0.
        if (op) {
            mark_consumed(a, at);
            mark_consumed(a, at + 1);
        }
    }

    json summ = json::object();
    for (const std::string& v : summvars) {
        Frame::Summary s = fr.summarize(v, detail, sumw, analytic_w, nullptr, importance_w);
        json js = {{"N", s.n}, {"mean", s.mean}, {"sd", s.sd}, {"min", s.min}, {"max", s.max}};
        // BUG-344: r(sum)/r(sum_w) are posted by EVERY Stata summarize — plain, detail,
        // and meanonly alike — so they cannot be gated on `detail` (which is a display
        // decision) or on `analytic_w` (which is a display decision too: sum_w doubles as
        // the "Weight" column). Both values are already computed on every path; withholding
        // them from the response was what left `r(sum)` unset for a command that otherwise
        // looked like it had worked. Additive to contract A (MINOR).
        js["sum"] = s.sum;
        js["sum_w"] = s.sum_w;
        if (detail) {
            js["var"] = s.var; js["skewness"] = s.skewness; js["kurtosis"] = s.kurtosis;
            js["p1"] = s.p1; js["p5"] = s.p5; js["p10"] = s.p10; js["p25"] = s.p25; js["p50"] = s.p50;
            js["p75"] = s.p75; js["p90"] = s.p90; js["p95"] = s.p95; js["p99"] = s.p99;
        }
        summ[v] = js;
    }
    // §26: in resident-run mode stage fr as the pending resident (mutations persist in
    // the session); one-shot keeps the exact old guarded write + "wrote" notice. The
    // n_rows/n_cols log below reads fr BEFORE any staging move, so it's unaffected.
    json log = {{"n_rows", fr.n_rows()}, {"n_rows_initial", n0}, {"n_cols", fr.n_cols()}};
    // BUG-1123: the values this call's --store-type turned into missing (absent when
    // none), so PIET's generate note can leave them out as Stata's does.
    if (const int64_t k = fr.store_overflow() - overflow0; k > 0) log["store_overflow"] = k;
    if (!commit_frame(std::move(fr), out) && !out.empty())
        std::fprintf(stderr, "wrote %s\n", out.c_str());
    if (!summ.empty()) log["summarize"] = summ;
    if (mt64_on) log["rng_draws"] = cecon::end_rng_mt64();
    emit(log, "");
    return 0;
}

int cmd_info(const std::string& path) {
    Dataset ds = Dataset::open(path);
    std::printf("%s\n  rows=%lld  cols=%d\n", path.c_str(),
                static_cast<long long>(ds.n_rows()), ds.n_cols());
    for (const auto& c : ds.columns()) {
        if (c.dtype == DType::Factor)
            std::printf("  %-28s factor (%zu levels)\n", c.name.c_str(), c.levels.size());
        else
            std::printf("  %-28s f64\n", c.name.c_str());
    }
    return 0;
}

// cobra datasignature --data f.hbin
//
// Stata `datasignature`: print a reproducible fingerprint of the data in memory.
// P1-A3-DATASIG-VERB — the head of P1 ARC 3 and the .dta writer's reverse oracle
// (DESIGN-dta-writer.md §4.2 sequences it FIRST precisely because it is useful on
// its own: users call it to prove two datasets are the same data). The algorithm
// and the Q9 "engine-deterministic, NOT Stata-exact" ruling live in
// src/data/datasignature.h; the contract it must satisfy is pinned by the eval cell
// P1A3_datasignature_reproducibility.
//
// Read-only, so no --out/--keepif surface: the one accepted flag is --data (empty in
// a resident §26 session, which supplies the frame). Signing must not materialize a
// borrowed column, which is why this hands `frame_from_acquired`'s Frame straight to
// a const-only reader instead of copying anything out of it.
// e(sample) T2 (internal note esample/SPEC.md §4.3): every column
// PIET names with the `__piet_` prefix is PIET's own (today the one that persists
// on the working frame is the estimation-sample mask `__piet_esample__`, which
// PIET hides from its listings, piet::isHiddenColumn in
// piet/native/include/piet/datastore.h; the rest are scratch). None is the
// user's data, so the two verbs that describe or write the user's data drop them:
// datasignature (Stata's signature never sees e(sample)) and export-dta (PIET's
// save/saveold/tempfile path; Stata's `save` does not write the marker either --
// w2_esample.log E14 reads e(sample) 0 after `use` of a saved copy). Kept here,
// not in src/data/frame.h, because only these command verbs apply the rule.
static void drop_piet_hidden_columns(Frame& fr) {
    std::vector<std::string> hidden;
    for (const auto& c : fr.columns())
        if (c.name.rfind("__piet_", 0) == 0) hidden.push_back(c.name);
    for (const auto& n : hidden) fr.drop(n);
}

int cmd_datasignature(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    if (data_missing(data)) {
        std::fprintf(stderr, "usage: cobra datasignature --data f.hbin\n"
                             "  Prints <rows>:<cols>(<meta>):<vals-a>:<vals-b> — deterministic for\n"
                             "  identical content; changes on variable reorder, a cell edit or a\n"
                             "  storage-type narrowing; unchanged by a display-format-only edit.\n"
                             "  Stata-SHAPED, not Stata-comparable (see src/data/datasignature.h).\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    // --unordered [--types v1=byte,v2=str5,...] (BUG-1461): PIET's user-facing
    // `datasignature` -- row order left out of the value plane and each named
    // column's Stata storage type signed with it, as Stata's signature is.
    SignatureOptions sopts;
    sopts.unordered = has(a, "--unordered");
    for (const auto& t : split_csv(opt(a, "--types"))) {
        const std::string kv = trimx(t);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) sopts.types[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    // --vars v1,v2,... [--keepif EXPR]: the estimation-sample signature a fit posts
    // as e(datasignature) (signestimationsample: `_datasignature varlist, esample
    // nonames`), over the named columns in order -- repeats allowed -- and the rows
    // EXPR keeps (PIET passes its e(sample) mask). Without --vars the verb is the
    // whole-frame `datasignature` above, unchanged.
    const std::string vars = opt(a, "--vars");
    if (!vars.empty()) {
        const std::string keepif = opt(a, "--keepif");
        if (!keepif.empty()) fr.keep_if(keepif);
        std::vector<std::string> names;
        for (const auto& t : split_csv(vars)) {
            std::string v = trimx(t);
            if (!v.empty()) names.push_back(v);
        }
        std::printf("%s\n", data_signature_vars(fr, names, sopts).c_str());
        return 0;
    }
    drop_piet_hidden_columns(fr);
    std::printf("%s\n", data_signature(fr, sopts).c_str());
    return 0;
}

// cobra version — engine ↔ kernel handshake (CONTRACT-engine §9). `--json` emits the
// machine shape the kernel reads at startup; bare form is human-readable. contract_version
// is the Boundary A semver, distinct from the engine release version (cobra_version).
int cmd_version(const std::vector<std::string>& a) {
    if (has(a, "--json")) {
        json v;
        v["cobra_version"] = COBRA_VERSION;
        v["contract_version"] = CONTRACT_VERSION;
        std::cout << v.dump(2) << "\n";
    } else {
        std::printf("cobra %s (contract %s)\n", COBRA_VERSION, CONTRACT_VERSION);
    }
    return 0;
}

// cobra gsort --data f.hbin --vars "-price,foreign" --out new.hbin
// Stata `gsort [-]v1 [-]v2 ...`. A leading '-' on a var means descending.
int cmd_gsort(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), varspec = opt(a, "--vars"), out = opt(a, "--out");
    if (data_missing(data) || varspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra gsort --data f.hbin --vars \"-price,foreign\" [--keepif EXPR] --out new.hbin\n"
                             "  A leading '-' on a var means descending for that key; default ascending.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> keys;
    std::vector<bool> asc;
    for (const auto& tok0 : split_csv(varspec)) {
        std::string tok = trimx(tok0);
        if (tok.empty()) continue;
        if (tok[0] == '-') { keys.push_back(tok.substr(1)); asc.push_back(false); }
        else if (tok[0] == '+') { keys.push_back(tok.substr(1)); asc.push_back(true); }
        else { keys.push_back(tok); asc.push_back(true); }
    }
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    Frame res = fr.gsort(keys, asc);
    int64_t nrows = res.n_rows();  // capture before the move into commit_frame
    if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "gsort: wrote %s (%lld rows)\n", out.c_str(), static_cast<long long>(nrows));
    return 0;
}

// cobra order --data f.hbin --vars make,price [--first|--last|--before V|--after V] --out new.hbin
int cmd_order(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), varspec = opt(a, "--vars"), out = opt(a, "--out");
    if (data_missing(data) || varspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra order --data f.hbin --vars v1,v2 "
                             "[--first|--last|--before V|--after V] [--keepif EXPR] --out new.hbin\n"
                             "  Default (no mode flag): first.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> varlist = split_csv(varspec);
    // Read all four mode flags unconditionally (not as an else-if chain) so each is
    // always looked up — and therefore always marked "consumed" for the E6 net —
    // regardless of which mode ends up winning; only the precedence below is
    // else-if (--first|--last|--before|--after are documented mutually exclusive,
    // default first).
    bool last = has(a, "--last"), first = has(a, "--first");
    std::string before = opt(a, "--before"), after = opt(a, "--after");
    std::string mode = "first";
    std::string anchor;
    if (last) mode = "last";
    else if (!before.empty()) { mode = "before"; anchor = before; }
    else if (!after.empty()) { mode = "after"; anchor = after; }
    else if (first) mode = "first";
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    Frame res = std::move(fr).order(varlist, mode, anchor);  // rvalue: steal, don't copy
    if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "order: wrote %s\n", out.c_str());
    return 0;
}

// Shared "--pairs a=b,c=d" parser for rename/clonevar. Empty spec, an entry missing
// '=', or an empty LHS/RHS is a bad_input (never silently skipped or truncated).
static std::vector<std::pair<std::string, std::string>> parse_pairs(
        const std::string& spec, const char* cmd) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& tok : split_csv(spec)) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos)
            throw EngineError("bad_input", std::string(cmd) + ": --pairs entry missing '=': '" + tok + "'");
        std::string lhs = trimx(tok.substr(0, eq)), rhs = trimx(tok.substr(eq + 1));
        if (lhs.empty() || rhs.empty())
            throw EngineError("bad_input", std::string(cmd) + ": --pairs entry has an empty name: '" + tok + "'");
        out.emplace_back(lhs, rhs);
    }
    if (out.empty())
        throw EngineError("bad_input", std::string(cmd) + ": --pairs must list at least one pair");
    return out;
}

// cobra rename --data f.hbin --pairs old=new[,old2=new2,...] --out new.hbin
// Stata `rename old new` — relabels column headers; every value/dtype/factor-
// dictionary byte is preserved. Multiple pairs apply atomically (see Frame::rename's
// doc comment in frame.h for the exact validation rule, incl. chain/swap support).
int cmd_rename(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), pairspec = opt(a, "--pairs"), out = opt(a, "--out");
    if (data_missing(data) || pairspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra rename --data f.hbin --pairs old=new[,old2=new2,...] "
                             "[--keepif EXPR] --out new.hbin\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    auto pairs = parse_pairs(pairspec, "rename");
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    fr.rename(pairs);
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "rename: wrote %s (%d pairs)\n", out.c_str(), static_cast<int>(pairs.size()));
    return 0;
}

// cobra clonevar --data f.hbin --pairs src=dst[,src2=dst2,...] --out new.hbin
// Stata `clonevar dst = src` — appends an exact duplicate of `src` (dtype incl.
// factor dictionary preserved) as a NEW column at the END of the column order;
// `src` is left untouched. Multiple pairs apply atomically.
int cmd_clonevar(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), pairspec = opt(a, "--pairs"), out = opt(a, "--out");
    if (data_missing(data) || pairspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra clonevar --data f.hbin --pairs src=dst[,src2=dst2,...] "
                             "[--keepif EXPR] --out new.hbin\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    auto pairs = parse_pairs(pairspec, "clonevar");
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    fr.clonevar(pairs);
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "clonevar: wrote %s (%d pairs)\n", out.c_str(), static_cast<int>(pairs.size()));
    return 0;
}

// cobra append --data master.hbin --using using.hbin [--keepusing v1,v2]
//              [--type-clash error|force] --out new.hbin
// `using` may be .hbin, .dta, or .csv/.tsv (auto-converted first).
int cmd_append(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), usingp = opt(a, "--using"), out = opt(a, "--out");
    std::string keepspec = opt(a, "--keepusing"), clash = opt(a, "--type-clash");
    if (data_missing(data) || usingp.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra append --data master.hbin --using using.(hbin|dta|csv) "
                             "[--keepusing v1,v2] [--type-clash error|force] [--keepif EXPR] --out new.hbin\n"
                             "  --keepusing: Stata `append using F, keep(varlist)` -- take only these\n"
                             "  columns from the using side (exact names, kept in the USING file's\n"
                             "  column order); a name absent from the using data is missing_column.\n"
                             "  --type-clash: a variable numeric on one side and string on the other.\n"
                             "  Unset = both sides promoted to f64 (contract default, internal padding\n"
                             "  callers); `error` = bad_input (Stata r(106)); `force` = the using side's\n"
                             "  cells become missing of the master's type (Stata `append, force`).\n");
        return 2;
    }
    if (!clash.empty() && clash != "error" && clash != "force")
        throw EngineError("bad_input", "append: --type-clash must be error or force", clash);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& mds = acquire_data(data);
    Frame master = frame_from_acquired(mds);

    Frame usingf = open_using_frame(usingp);  // WS-D2: ONE open (was two on the .hbin path)
    if (!keepspec.empty()) {
        // BUG-549: `keep(varlist)` narrows the USING frame before the concat, so both
        // the streaming writer and the resident path below see only the kept columns.
        // Order is the using file's, not the list's (w2_data_io.log app2: keep(z s id)
        // gives id ... s z). Names arrive already expanded (PIET resolves wildcards,
        // ranges and abbreviations against the using names); here they are exact.
        std::unordered_set<std::string> want;
        for (const auto& nm : split_csv(keepspec)) {
            if (!usingf.has(nm))
                throw EngineError("missing_column",
                                  "append: variable " + nm + " not found in the using data", nm);
            want.insert(nm);
        }
        std::vector<std::string> ordered;
        for (const auto& c : usingf.columns())
            if (want.count(c.name)) ordered.push_back(c.name);
        usingf = usingf.project(ordered);
    }
    if (!clash.empty()) {
        // BUG-550: Stata refuses a numeric/string clash (w2_data_io.log app15: r(106)
        // "variable s is float in master but str3 in using data") and under `force`
        // takes the using cells as missing of the MASTER's type (app16/app17: s = .
        // on the using rows). The unflagged default above promotes both sides to f64,
        // which turns string codes into made-up numbers; PIET's `append` asks for the
        // Stata rule. Checked after the keep() projection: only kept columns clash.
        // The engine carries no Stata storage type, so the message says numeric/string.
        std::vector<std::string> clashing;
        for (const auto& uc : usingf.columns())
            if (master.has(uc.name) && master.col(uc.name).is_factor != uc.is_factor)
                clashing.push_back(uc.name);
        for (const auto& nm : clashing) {
            const bool master_str = master.col(nm).is_factor;
            if (clash == "error")
                throw EngineError("bad_input",
                                  "variable " + nm + " is " + (master_str ? "string" : "numeric") +
                                      " in master but " + (master_str ? "numeric" : "string") +
                                      " in using data",
                                  "append_type_clash:" + nm);
            FrameColumn nc;
            nc.is_factor = master_str;
            if (master_str) nc.codes.assign(static_cast<std::size_t>(usingf.n_rows()), -1);
            else nc.f64.assign(static_cast<std::size_t>(usingf.n_rows()),
                               std::numeric_limits<double>::quiet_NaN());
            usingf.setcol(nm, std::move(nc), false);
            std::fprintf(stderr, "(note: variable %s was %s in the using data, but will be %s now)\n",
                         nm.c_str(), master_str ? "numeric" : "string", master_str ? "string" : "numeric");
        }
    }

    int64_t n_master = master.n_rows(), n_using = usingf.n_rows();
    if (!serve_resident_active() && !out.empty()) {
        // One-shot `--out`: stream the column-union row-concat straight to disk —
        // byte-identical to master.append(usingf).write_hbin(out) but without ever
        // materializing the full (nm+nu)-row result Frame (ENG-APPEND: kills the
        // 786MB full-result-column copy; wall is I/O-floor either way). The resident
        // /serve path needs the real staged Frame, so it falls through to below.
        master.write_hbin_append(usingf, out);
        std::fprintf(stderr, "append: wrote %s (%lld master + %lld using = %lld rows)\n",
                    out.c_str(), static_cast<long long>(n_master), static_cast<long long>(n_using),
                    static_cast<long long>(n_master + n_using));
        return 0;
    }
    Frame res = master.append(usingf);
    int64_t nrows = res.n_rows();  // capture before the move into commit_frame
    if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "append: wrote %s (%lld master + %lld using = %lld rows)\n",
                    out.c_str(), static_cast<long long>(n_master), static_cast<long long>(n_using),
                    static_cast<long long>(nrows));
    return 0;
}

// cobra joinby --data master.hbin --using using.hbin --keys k1[,k2,...] --out new.hbin
// Stata `joinby k1 [k2 ...] using FILE`, BARE form only (no unmatched()/
// update/updatemiss/nolabel — this build implements exactly ONE mode). See
// frame.h's Frame::joinby doc comment for the full semantics: within-key
// Cartesian fan-out, unmatched master rows dropped entirely, factor keys
// matched by LEVEL STRING (not per-file code), a non-key column name
// collision is bad_input. `--using` is an .hbin only (unlike append/merge,
// which also auto-detect .dta/.csv) — this replaces PIET's own C++ join +
// CSV+`cobra convert` round-trip (handlers_data_verbs.cpp runJoinby), whose
// `using` file PIET already resolves to .hbin itself before ever reaching
// cobra.
int cmd_joinby(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), usingp = opt(a, "--using"), keyspec = opt(a, "--keys"),
               out = opt(a, "--out");
    if (data_missing(data) || usingp.empty() || keyspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra joinby --data master.hbin --using using.hbin "
                             "--keys k1[,k2,...] [--keepif EXPR] --out new.hbin\n"
                             "  Stata `joinby k1 [k2 ...] using FILE`, bare form only (no options): for\n"
                             "  every master row, emits one row per using row sharing ALL key values (the\n"
                             "  full within-key Cartesian cross); a master row with zero using matches is\n"
                             "  DROPPED entirely. Factor keys match by LEVEL STRING, not per-file code.\n"
                             "  A non-key column name present on both sides is bad_input.\n"
                             "  [--unmatched none|master|using|both] [--merge-var NAME]: keep the\n"
                             "  unmatched rows (codes 1/2) and add the 1/2/3 marker (joinby.ado).\n");
        return 2;
    }
    // COBRA_VERB_DIAG (measurement-only, see src/core/verb_diag.h): phase split of
    // this handler into input load/materialization, kernel (Frame::joinby), and
    // output write, for the same performance-campaign attribution pass as the
    // merge/keepif/preserve-restore probes below. OFF path costs one bool check.
    const bool kVerbDiag = cecon::verb_diag_enabled();
    const auto t_req0 = cecon::verb_diag_now(kVerbDiag);

    const auto t_load0 = cecon::verb_diag_now(kVerbDiag);
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> keys = split_csv(keyspec);

    const Dataset& mds = acquire_data(data);
    Frame master = frame_from_acquired(mds);

    Frame usingf = open_using_frame(usingp);  // WS-D2: ONE open (was two)
    const double load_ms = cecon::verb_diag_ms(kVerbDiag, t_load0);

    int64_t n_master = master.n_rows(), n_using = usingf.n_rows();
    const auto t_kernel0 = cecon::verb_diag_now(kVerbDiag);
    // joinby.ado's unmatched() (none|master|using|both) and its marker.
    Frame res = master.joinby(usingf, keys, opt(a, "--unmatched"), opt(a, "--merge-var"));
    const double kernel_ms = cecon::verb_diag_ms(kVerbDiag, t_kernel0);
    int64_t nrows = res.n_rows();  // capture before the move into commit_frame

    const auto t_write0 = cecon::verb_diag_now(kVerbDiag);
    bool staged = commit_frame(std::move(res), out);  // §26: stage into resident session, or write+notice one-shot
    const double write_ms = cecon::verb_diag_ms(kVerbDiag, t_write0);
    if (!staged)
        std::fprintf(stderr, "joinby: wrote %s (%lld master + %lld using -> %lld rows, %d key%s)\n",
                    out.c_str(), static_cast<long long>(n_master), static_cast<long long>(n_using),
                    static_cast<long long>(nrows), static_cast<int>(keys.size()), keys.size() == 1 ? "" : "s");

    if (kVerbDiag) {
        // A staged (resident-session) commit writes no file; a one-shot commit
        // wrote `out` — stat its final size for scratch_bytes.
        const int64_t scratch_bytes = (!staged && !out.empty())
            ? cecon::verb_diag_file_size(cecon::to_native_path(out)) : 0;
        const double total_ms = cecon::verb_diag_ms(kVerbDiag, t_req0);
        const double other_ms = total_ms - load_ms - kernel_ms - write_ms;
        cecon::verb_diag_emit(
            "[verb-diag] verb=joinby total_ms=%.3f load_ms=%.3f kernel_ms=%.3f write_ms=%.3f "
            "other_ms=%.3f scratch_bytes=%lld rows_in=%lld rows_out=%lld\n",
            total_ms, load_ms, kernel_ms, write_ms, other_ms,
            static_cast<long long>(scratch_bytes), static_cast<long long>(n_master),
            static_cast<long long>(nrows));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// P1 wave data verbs. Every semantic is oracle-pinned against StataBE 19.5 on
// the P1 wave fixture (2026-08-04) — see each Frame method's doc comment in
// src/data/frame.h for the pinned numbers.

// cobra expand --data f.hbin --n N --out new.hbin
// Stata `expand N`: replace each observation with N copies. N<=1 leaves the
// data unchanged (Stata does not error). Copies are APPENDED after the
// original block, so the original row order survives at the head.
int cmd_expand(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), nspec = opt(a, "--n"), out = opt(a, "--out");
    if (data_missing(data) || nspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra expand --data f.hbin --n N [--keepif EXPR] --out new.hbin\n"
                             "  Stata `expand N`: each observation becomes N copies (N<=1 = no change).\n");
        return 2;
    }
    int64_t n = 0;
    try {
        size_t pos = 0;
        n = static_cast<int64_t>(std::stoll(nspec, &pos));
        if (pos != nspec.size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
        throw EngineError("bad_input", "expand: --n must be an integer (got '" + nspec + "')");
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    int64_t added = 0;
    Frame res = fr.expand(n, &added);
    int64_t nrows = res.n_rows();
    if (!commit_frame(std::move(res), out))
        std::fprintf(stderr, "expand: wrote %s (%lld obs created, %lld rows)\n", out.c_str(),
                     static_cast<long long>(added), static_cast<long long>(nrows));
    return 0;
}

// cobra fillin --data f.hbin --vars v1[,v2,...] --out new.hbin
// Stata `fillin v1 v2 ...`: rectangularize over the OBSERVED values of the
// listed variables. New rows are missing in every other column and are marked
// by a new `_fillin` column (1 = created, 0 = pre-existing).
int cmd_fillin(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), varspec = opt(a, "--vars"), out = opt(a, "--out");
    if (data_missing(data) || varspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra fillin --data f.hbin --vars v1[,v2,...] [--keepif EXPR] "
                             "--out new.hbin\n"
                             "  Stata `fillin v1 v2 ...`: add the rows needed so every combination of\n"
                             "  the observed values is present; new rows carry missing elsewhere and\n"
                             "  a `_fillin` flag column marks them.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> vars = split_csv(varspec);
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    int64_t added = 0;
    Frame res = fr.fillin(vars, &added);
    int64_t nrows = res.n_rows();
    if (!commit_frame(std::move(res), out))
        std::fprintf(stderr, "fillin: wrote %s (%lld row%s added, %lld total)\n", out.c_str(),
                     static_cast<long long>(added), added == 1 ? "" : "s",
                     static_cast<long long>(nrows));
    return 0;
}

// cobra cross --data master.hbin --using using.hbin --out new.hbin
// Stata `cross using FILE`: the full Cartesian product. A column name present
// on both sides is taken from the MASTER (the using copy is dropped) — this is
// the observed Stata behavior, not an error.
int cmd_cross(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), usingp = opt(a, "--using"), out = opt(a, "--out");
    if (data_missing(data) || usingp.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra cross --data master.hbin --using using.hbin "
                             "[--keepif EXPR] --out new.hbin\n"
                             "  Stata `cross using FILE`: every master row paired with every using\n"
                             "  row. A name present on both sides comes from the master.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& mds = acquire_data(data);
    Frame master = frame_from_acquired(mds);
    Frame usingf = open_using_frame(usingp);  // WS-D2: ONE open (was two)
    int64_t nm = master.n_rows(), nu = usingf.n_rows();
    Frame res = master.cross(usingf);
    int64_t nrows = res.n_rows();
    if (!commit_frame(std::move(res), out))
        std::fprintf(stderr, "cross: wrote %s (%lld x %lld -> %lld rows)\n", out.c_str(),
                     static_cast<long long>(nm), static_cast<long long>(nu),
                     static_cast<long long>(nrows));
    return 0;
}

// cobra contract --data f.hbin --vars v1[,v2,...] --out new.hbin
// Stata `contract varlist`: the distinct combinations of varlist with their
// frequencies in `_freq`. The result holds ONLY varlist + `_freq`.
int cmd_contract(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), varspec = opt(a, "--vars"), out = opt(a, "--out");
    if (data_missing(data) || varspec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra contract --data f.hbin --vars v1[,v2,...] "
                             "[--keepif EXPR] --out new.hbin\n"
                             "  Stata `contract varlist`: one row per distinct combination, with the\n"
                             "  count in a new `_freq` column. Only varlist + `_freq` survive.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> vars = split_csv(varspec);
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    Frame res = fr.contract(vars);
    int64_t nrows = res.n_rows();
    if (!commit_frame(std::move(res), out))
        std::fprintf(stderr, "contract: wrote %s (%lld distinct combination%s)\n", out.c_str(),
                     static_cast<long long>(nrows), nrows == 1 ? "" : "s");
    return 0;
}

// cobra merge --data master.hbin --using using.(hbin|dta|csv) --type 1:1|m:1|1:m
//             --key k1,k2 [--keepusing v1,v2] [--nogenerate | --generate NAME]
//             [--update [--replace]] [--keep master|using|matched|all]
//             [--assert match|master|using|all] --out new.hbin
int cmd_merge(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), usingp = opt(a, "--using"), out = opt(a, "--out");
    std::string type = opt(a, "--type"), keyspec = opt(a, "--key");
    const bool stata_plan = has(a, "--stata-plan");
    const bool sequential = stata_plan && has(a, "--sequential");
    if (data_missing(data) || usingp.empty() || out_missing(out) || type.empty() ||
        (keyspec.empty() && !sequential)) {
        std::fprintf(stderr, "usage: cobra merge --data master.hbin --using using.(hbin|dta|csv) "
                             "--type 1:1|m:1|1:m --key k1,k2\n"
                             "  [--keepusing v1,v2] [--nogenerate | --generate NAME]\n"
                             "  [--update [--replace]]  (Stata `merge ..., update [replace]`: matched rows\n"
                             "     take the using's value where the master is MISSING, and with --replace\n"
                             "     where the master merely disagrees; the marker gains 4=missing updated\n"
                             "     and 5=nonmissing conflict, assigned per ROW over all overlapping vars)\n"
                             "  [--keep master|using|matched|all|1..5] [--assert match|master|using|all]\n"
                             "  [--keepif EXPR] --out new.hbin\n"
                             "  [--stata-plan [--sequential] [--sorted]]  (Stata's built-in _merge row\n"
                             "     plan: key-sorted master, appended rows in using key order, using\n"
                             "     values on using-only rows, exact keep/assert codes; --type m:m;\n"
                             "     --sequential = merge 1:1 _n with no --key; --sorted = no re-sort)\n");
        return 2;
    }
    if (type != "1:1" && type != "m:1" && type != "1:m" && !(stata_plan && type == "m:m"))
        throw EngineError("bad_input", "merge: --type must be 1:1, m:1, or 1:m");

    // WS-D2 / S06 ARC-MRG. The P2-PERF close NAMED merge as opening its files three
    // times; this brace is the measurement that makes the claim falsifiable. The
    // counter is a DELTA over the whole invocation rather than hand-placed
    // increments, so it also catches opens performed by code this function never
    // reads — `maybe_keepif`'s scratch round trip and anything the writer does.
    // Target = 2: master once, using once.
    const int64_t opens_at_entry = engctr::dataFileOpens();
    struct MergeOpenScope {
        int64_t at_entry;
        ~MergeOpenScope() {
            engctr::setMergeFileOpens(engctr::dataFileOpens() - at_entry);
            // Env-gated, one line on stderr, nothing on stdout: a one-shot CLI run has
            // no `frame counters` reader (that op only exists inside a serve session),
            // and stdout is a frozen fixture surface. Unset => byte-identical output.
            static const bool diag = std::getenv("COBRA_MERGE_IO_DIAG") != nullptr;
            if (diag)
                std::fprintf(stderr, "[merge-io] merge_file_opens=%lld\n",
                             static_cast<long long>(engctr::mergeFileOpens()));
        }
    } merge_open_scope{opens_at_entry};

    // COBRA_VERB_DIAG (measurement-only, see src/core/verb_diag.h): phase split
    // of this handler into input load/materialization, kernel (merge_frames),
    // and output write — same probe family as joinby/keepif/preserve-restore.
    // Independent of MergeOpenScope above (left untouched); OFF path costs one
    // bool check.
    const bool kVerbDiag = cecon::verb_diag_enabled();
    const auto t_req0 = cecon::verb_diag_now(kVerbDiag);

    const auto t_load0 = cecon::verb_diag_now(kVerbDiag);
    data = maybe_keepif(data, opt(a, "--keepif"));

    const Dataset& mds = acquire_data(data);
    Frame master = frame_from_acquired(mds);

    Frame usingf = open_using_frame(usingp);  // WS-D2: ONE open (was two on the .hbin path)
    const double load_ms = cecon::verb_diag_ms(kVerbDiag, t_load0);

    MergeOpts opts;
    opts.type = type;
    opts.keylist = sequential ? std::vector<std::string>{} : split_csv(keyspec);
    opts.stata_plan = stata_plan;
    opts.sequential = sequential;
    opts.presorted = stata_plan && has(a, "--sorted");
    opts.keepusing = split_csv(opt(a, "--keepusing"));
    opts.nogenerate = has(a, "--nogenerate");
    // `merge ..., update [replace]`. `--replace` alone is legal and means the same as
    // `update replace`: [D] merge (d.pdf, PDF page 711) — "update and replace both perform an
    // update merge rather than a standard merge ... If replace is specified, then
    // matched observations will contain values from the using dataset, unless the
    // value in the using dataset is missing."
    opts.update = has(a, "--update");
    opts.replace = has(a, "--replace");
    opts.generate = opt(a, "--generate");
    if (!opts.generate.empty() && opts.nogenerate)
        throw EngineError("bad_input", "merge: --generate and --nogenerate are mutually exclusive");

    auto parse_codes = [](const std::string& s) -> std::vector<int> {
        std::vector<int> codes;
        std::string low = s;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& tok0 : split_vars(low)) {
            std::string tok = trimx(tok0);
            if (tok == "master" || tok == "1") codes.push_back(1);
            else if (tok == "using" || tok == "2") codes.push_back(2);
            else if (tok == "matched" || tok == "match" || tok == "3") codes.push_back(3);
            // Update-merge codes, spellable only as numbers (Stata has no keyword for
            // them): 4 = missing updated, 5 = nonmissing conflict. Naming either one
            // makes the whole spec exact, so `match` stops covering them.
            else if (tok == "4") codes.push_back(4);
            else if (tok == "5") codes.push_back(5);
            else if (tok == "all") { codes = {1, 2, 3}; }
        }
        return codes;
    };
    std::string keep_s = opt(a, "--keep");
    if (!keep_s.empty()) opts.keep_codes = parse_codes(keep_s);
    std::string assert_s = opt(a, "--assert");
    if (!assert_s.empty()) opts.assert_codes = parse_codes(assert_s);

    int64_t n_master = master.n_rows();
    const auto t_kernel0 = cecon::verb_diag_now(kVerbDiag);
    MergeResult r = merge_frames(master, usingf, opts);
    const double kernel_ms = cecon::verb_diag_ms(kVerbDiag, t_kernel0);
    int64_t nrows = r.frame.n_rows();  // capture before the move (r.master_only/... survive)

    const auto t_write0 = cecon::verb_diag_now(kVerbDiag);
    bool staged = commit_frame(std::move(r.frame), out);  // §26: stage into resident session, or write+notice one-shot
    const double write_ms = cecon::verb_diag_ms(kVerbDiag, t_write0);
    if (!staged) {
        std::fprintf(stderr, "merge %s: wrote %s (%lld rows; master_only=%lld using_only=%lld matched=%lld)\n",
                    type.c_str(), out.c_str(), static_cast<long long>(nrows),
                    static_cast<long long>(r.master_only), static_cast<long long>(r.using_only),
                    static_cast<long long>(r.matched));
        // Stata's update-merge summary table splits `Matched` three ways; without this
        // line an update merge's notice is indistinguishable from a standard one.
        if (opts.update || opts.replace)
            std::fprintf(stderr, "merge %s: update (not_updated=%lld missing_updated=%lld nonmissing_conflict=%lld)\n",
                        type.c_str(), static_cast<long long>(r.matched_notupdated),
                        static_cast<long long>(r.matched_missupdated),
                        static_cast<long long>(r.matched_conflict));
    }
    else
        // A staged merge writes no file and prints no notice, so the counts would be
        // lost — they go out on the frame response instead. Same three numbers, same
        // values, a channel that survives residency.
        serve_publish_merge_counts(r.master_only, r.using_only, r.matched);

    if (kVerbDiag) {
        const int64_t scratch_bytes = (!staged && !out.empty())
            ? cecon::verb_diag_file_size(cecon::to_native_path(out)) : 0;
        const double total_ms = cecon::verb_diag_ms(kVerbDiag, t_req0);
        const double other_ms = total_ms - load_ms - kernel_ms - write_ms;
        cecon::verb_diag_emit(
            "[verb-diag] verb=merge total_ms=%.3f load_ms=%.3f kernel_ms=%.3f write_ms=%.3f "
            "other_ms=%.3f scratch_bytes=%lld rows_in=%lld rows_out=%lld\n",
            total_ms, load_ms, kernel_ms, write_ms, other_ms,
            static_cast<long long>(scratch_bytes), static_cast<long long>(n_master),
            static_cast<long long>(nrows));
    }
    return 0;
}

// cobra duplicates report|drop|tag --data f.hbin [--vars v1,v2] [--gen NEWVAR] [--out ...]
int cmd_duplicates(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::fprintf(stderr, "usage: cobra duplicates report|drop|tag --data f.hbin [--vars v1,v2] "
                             "[--gen NEWVAR (tag only)] [--keepif EXPR] [--out out.(hbin|json)]\n");
        return 2;
    }
    std::string sub = a[1];
    std::string data = opt(a, "--data"), out = opt(a, "--out");
    if (data_missing(data)) { std::fprintf(stderr, "duplicates: --data required\n"); return 2; }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> varlist = split_csv(opt(a, "--vars"));

    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);

    if (sub == "report") {
        auto rows = fr.duplicates_report(varlist);
        json jrows = json::array();
        int64_t total_surplus = 0;
        for (const auto& r : rows) {
            jrows.push_back({{"copies", r.copies}, {"observations", r.observations}, {"surplus", r.surplus}});
            total_surplus += r.surplus;
        }
        json result = {{"command", "duplicates report"}, {"vars", varlist}, {"rows", jrows},
                       {"total_surplus", total_surplus}};
        emit(result, out);
    } else if (sub == "drop") {
        if (out_missing(out)) { std::fprintf(stderr, "duplicates drop: --out (.hbin) required\n"); return 2; }
        int64_t n_dropped = 0;
        Frame res = fr.duplicates_drop(varlist, &n_dropped);
        int64_t nrows = res.n_rows();  // capture before the move into commit_frame
        if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
            std::fprintf(stderr, "duplicates drop: wrote %s (%lld rows, %lld dropped)\n",
                        out.c_str(), static_cast<long long>(nrows), static_cast<long long>(n_dropped));
    } else if (sub == "tag") {
        std::string genvar = opt(a, "--gen");
        if (genvar.empty() || out_missing(out)) {
            std::fprintf(stderr, "duplicates tag: --gen NEWVAR and --out (.hbin) required\n");
            return 2;
        }
        if (fr.has(genvar)) throw EngineError("bad_input", "duplicates tag: variable '" + genvar + "' already exists");
        auto tag = fr.duplicates_tag(varlist);
        fr.set_f64_column(genvar, std::move(tag));
        if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
            std::fprintf(stderr, "duplicates tag: wrote %s (new var %s)\n", out.c_str(), genvar.c_str());
    } else {
        std::fprintf(stderr, "duplicates: unknown subcommand '%s' (report|drop|tag)\n", sub.c_str());
        return 2;
    }
    return 0;
}

// cobra recode --data f.hbin --var VAR --rules "(1 2=1)(3 4=2)(else=.)" [--gen NEWVAR] --out new.hbin
// Rule syntax matches Stata recode: (v=new) (v1 v2=new) (lo/hi=new) (lo:hi=new)
// (min/hi=new) (lo/max=new) (missing=new)/(.=new) (else=new).
int cmd_recode(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var"), rulestr = opt(a, "--rules"), out = opt(a, "--out");
    if (data_missing(data) || var.empty() || rulestr.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra recode --data f.hbin --var VAR --rules \"(1 2=1)(3 4=2)(else=.)\" "
                             "[--gen NEWVAR] [--keepif EXPR] --out new.hbin\n"
                             "  Rule forms: (v=new) (v1 v2 ..=new) (lo/hi=new) (lo:hi=new) (min/hi=new)\n"
                             "  (lo/max=new) (missing=new) (.=new) (else=new).\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));

    auto parse_new_val = [](const std::string& rhs0, Frame::RecodeRule& r) {
        std::string rhs = trimx(rhs0);
        if (rhs == "." || rhs == "sysmis" || rhs.empty()) { r.new_is_missing = true; r.new_val = 0; }
        else { r.new_is_missing = false; r.new_val = std::stod(rhs); }
    };
    auto parse_bound = [](const std::string& s0) -> double {
        std::string s = trimx(s0);
        std::string low = s;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low == "min") return -std::numeric_limits<double>::infinity();
        if (low == "max") return std::numeric_limits<double>::infinity();
        return std::stod(s);
    };

    std::vector<Frame::RecodeRule> rules;
    // Extract each (...) group.
    size_t p = 0;
    while (p < rulestr.size()) {
        size_t open = rulestr.find('(', p);
        if (open == std::string::npos) break;
        size_t close = rulestr.find(')', open);
        if (close == std::string::npos) throw EngineError("bad_input", "recode: unmatched '(' in rules");
        std::string group = trimx(rulestr.substr(open + 1, close - open - 1));
        p = close + 1;

        size_t eq = group.rfind('=');
        if (eq == std::string::npos) throw EngineError("bad_input", "recode: rule group missing '=': (" + group + ")");
        std::string lhs = trimx(group.substr(0, eq));
        std::string rhs = group.substr(eq + 1);
        std::string lhs_low = lhs;
        for (auto& c : lhs_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lhs_low == "else") {
            Frame::RecodeRule r; r.kind = 3; parse_new_val(rhs, r); rules.push_back(r);
        } else if (lhs_low == "." || lhs_low == "missing" || lhs_low == "sysmis") {
            Frame::RecodeRule r; r.kind = 2; parse_new_val(rhs, r); rules.push_back(r);
        } else if (lhs.find('/') != std::string::npos || lhs.find(':') != std::string::npos) {
            char sep = (lhs.find('/') != std::string::npos) ? '/' : ':';
            size_t sp = lhs.find(sep);
            Frame::RecodeRule r; r.kind = 1;
            r.a = parse_bound(lhs.substr(0, sp));
            r.b = parse_bound(lhs.substr(sp + 1));
            parse_new_val(rhs, r);
            rules.push_back(r);
        } else {
            for (const auto& v : split_vars(lhs)) {
                std::string vlow = v;
                for (auto& c : vlow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                Frame::RecodeRule r;
                if (vlow == "." || vlow == "missing" || vlow == "sysmis") r.kind = 2;
                else { r.kind = 0; r.a = std::stod(v); }
                parse_new_val(rhs, r);
                rules.push_back(r);
            }
        }
    }
    if (rules.empty()) throw EngineError("bad_input", "recode: no valid rule groups parsed from --rules");

    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    if (!fr.has(var)) throw EngineError("missing_column", "recode: variable '" + var + "' not found", var);
    auto result = fr.recode_apply(var, rules);

    std::string genvar = opt(a, "--gen");
    if (!genvar.empty()) {
        if (fr.has(genvar)) throw EngineError("bad_input", "recode: variable '" + genvar + "' already exists");
        fr.set_f64_column(genvar, std::move(result));
    } else {
        fr.set_f64_column(var, std::move(result));  // in-place replace (always f64, matches recode.py)
    }
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "recode: wrote %s (%s)\n", out.c_str(),
                    genvar.empty() ? ("replaced " + var).c_str() : ("new var " + genvar).c_str());
    return 0;
}

// cobra encode --data f.hbin --var SRC --gen NEW --out new.hbin
// Stata `encode SRC, generate(NEW)` — SRC must already be a factor/string column;
// NEW = f64 codes assigned by ALPHABETICAL level order, 1-based (missing/empty stays
// missing). Output = input columns + NEW appended. A JSON envelope with the
// alphabetically-ordered `levels` array is ALSO printed to stdout (§22 extension —
// unlike the other data verbs' plain stderr notice) so a caller (PIET) can attach
// value labels without re-deriving the alphabetical order itself.
int cmd_encode(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var"), gen = opt(a, "--gen"), out = opt(a, "--out");
    if (data_missing(data) || var.empty() || gen.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra encode --data f.hbin --var SRC --gen NEW "
                             "[--keepif EXPR] --out new.hbin\n"
                             "  NEW = f64 codes assigned by ALPHABETICAL level order (Stata encode), "
                             "1-based; missing/empty stays missing.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    std::vector<std::string> levels = fr.encode(var, gen);
    // capture before the move into commit_frame (staged/write may leave fr's lifetime
    // ended in resident mode, but we already have everything we need above).
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "encode: wrote %s (%s -> %s, %d levels)\n",
                    out.c_str(), var.c_str(), gen.c_str(), static_cast<int>(levels.size()));
    // BUG-1372: substr() counts bytes, so a level can end inside a character; it
    // travels escaped, as in the .hbin header, and PIET's runEncode turns it back.
    std::vector<std::string> sent;
    for (const auto& l : levels) sent.push_back(hbin_level_json_text(l));
    json result = {{"command", "encode"}, {"var", var}, {"gen", gen},
                   {"n_levels", static_cast<int>(levels.size())}, {"levels", sent}};
    emit(result, "");  // stdout, regardless of --out (which is the .hbin target here)
    return 0;
}

// cobra setcol --data f.hbin --var NAME --values FILE.csv --dtype f64|factor [--new] --out new.hbin
// Same class as rename/clonevar/encode above (whole-frame CSV round-trip retirement —
// destring/tostring/single-column predict-style writes): patches (default) or appends
// (--new) exactly ONE column from a HEADERLESS single-column CSV payload (one raw
// value per row; row count must equal the dataset's n_rows exactly). Every OTHER
// column/dictionary byte is preserved. --var existing + no --new = in-place replace
// (dtype MAY change, e.g. factor->f64 = destring); --var new + --new = append at the
// END; a mismatched NAME/--new combination is bad_input (Frame::setcol/read_csv_column
// own every validation rule — see frame.h doc comments). --store-type T (f64 only)
// stores every value as Stata's type T holds it (BUG-924: PIET's predict writes its
// natively computed statistics here, float by default).
int cmd_setcol(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var"), values = opt(a, "--values"),
               dtype = opt(a, "--dtype"), out = opt(a, "--out");
    bool is_new = has(a, "--new");
    if (data_missing(data) || var.empty() || values.empty() || dtype.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra setcol --data f.hbin --var NAME --values FILE.csv "
                             "--dtype f64|factor [--new] [--ones-as V] [--store-type T] "
                             "[--keepif EXPR] --out new.hbin\n"
                             "  FILE.csv: single column, ONE raw value per row, NO header; row count\n"
                             "  must equal the dataset's n_rows exactly. --new appends NAME as a\n"
                             "  brand-new column (NAME must not already exist); without --new, NAME is\n"
                             "  replaced in place (NAME must already exist; dtype may change, e.g.\n"
                             "  factor->f64 = destring). --ones-as V: FILE is a 0/1 indicator and\n"
                             "  each 1 is stored as V (f64 only). --store-type byte|int|long|float|double\n"
                             "  stores each value as that Stata type holds it (f64 only).\n");
        return 2;
    }
    const StoreType storeType = parse_store_type(a, "setcol");
    if (storeType != StoreType::Double && dtype != "f64")
        throw EngineError("bad_input", "setcol: --store-type requires --dtype f64");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    // `--ones-as V` (e(sample) T2): the values file is the 0/1 indicator a fit
    // wrote (`--esample-out`), and every 1 is stored as V -- PIET's per-install
    // serial for the estimation-sample mask. Parsed here line by line ("0" or "1",
    // nothing else, one per row): the general CSV reader paid a string per row,
    // and PIET used to read and rewrite the whole file per fit first. Any other
    // line is refused, so a file that is not that indicator is never stored.
    FrameColumn newcol;
    const std::string ones_as = opt(a, "--ones-as");
    if (!ones_as.empty()) {
        double v = 0.0;
        try {
            size_t used = 0;
            v = std::stod(ones_as, &used);
            if (used != ones_as.size()) throw std::invalid_argument(ones_as);
        } catch (const std::exception&) {
            throw EngineError("bad_input", "setcol: --ones-as must be a number (got '" + ones_as + "')");
        }
        if (dtype != "f64")
            throw EngineError("bad_input", "setcol: --ones-as requires --dtype f64");
        std::ifstream in(to_native_path(values), std::ios::binary | std::ios::ate);
        if (!in) throw EngineError("data_io", "setcol: cannot open --values file: " + values, values);
        const std::streamoff size = in.tellg();
        std::string body(static_cast<size_t>(size > 0 ? size : 0), '\0');
        in.seekg(0);
        if (size > 0 && !in.read(&body[0], size))
            throw EngineError("data_io", "setcol: cannot read --values file: " + values, values);
        const int64_t n_rows = fr.n_rows();
        std::vector<double> cells(static_cast<size_t>(n_rows), 0.0);
        auto refuse = [&](int64_t row) {
            throw EngineError("bad_input",
                "setcol: --ones-as needs a 0/1 values file; row " + std::to_string(row) +
                " is not 0 or 1", values);
        };
        if (body.size() == 2 * static_cast<size_t>(n_rows)) {
            // The engine's own layout: exactly "0\n" or "1\n" per row.
            for (int64_t i = 0; i < n_rows; ++i) {
                const char c = body[2 * static_cast<size_t>(i)];
                if ((c != '0' && c != '1') || body[2 * static_cast<size_t>(i) + 1] != '\n')
                    refuse(i + 1);
                if (c == '1') cells[static_cast<size_t>(i)] = v;
            }
        } else {
            // Any other layout (CRLF, a missing final newline, a wrong row count).
            int64_t row = 0;
            size_t pos = 0;
            while (pos < body.size()) {
                size_t eol = body.find('\n', pos);
                if (eol == std::string::npos) eol = body.size();
                size_t len = eol - pos;
                if (len > 0 && body[pos + len - 1] == '\r') --len;
                ++row;
                if (len != 1 || (body[pos] != '0' && body[pos] != '1')) refuse(row);
                if (row > n_rows) break;
                if (body[pos] == '1') cells[static_cast<size_t>(row - 1)] = v;
                pos = eol + 1;
            }
            if (row != n_rows)
                throw EngineError("bad_input", "setcol: --values has " + std::to_string(row) +
                    " row(s), the dataset has " + std::to_string(n_rows) +
                    " — row counts must match exactly", values);
        }
        newcol.is_factor = false;
        newcol.f64 = std::move(cells);
    } else {
        newcol = Frame::read_csv_column(values, dtype, fr.n_rows());
    }
    if (storeType != StoreType::Double)
        round_column_to_store(newcol.f64.data(), newcol.f64.size(), storeType);
    // e(sample) T2: a hidden `__piet_*` column (see drop_piet_hidden_columns)
    // is PIET's own, and PIET cannot see whether the working frame still carries
    // one (the user-facing card hides it). `--new` on such a name REPLACES it, so
    // installing the estimation-sample mask after every fit is one call whether
    // or not an earlier fit's mask is still on the rows. User names keep the
    // strict contract above.
    if (is_new && var.rfind("__piet_", 0) == 0 && fr.has(var)) is_new = false;
    fr.setcol(var, std::move(newcol), is_new);
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "setcol: wrote %s (%s '%s', dtype %s)\n",
                    out.c_str(), is_new ? "appended" : "replaced", var.c_str(), dtype.c_str());
    return 0;
}

// cobra reshape long|wide --data f.hbin --stubs s1,s2 --i id1,id2 --j year --out new.hbin
int cmd_reshape(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::fprintf(stderr, "usage: cobra reshape long|wide --data f.hbin --stubs s1,s2 --i id1,id2 --j year "
                             "[--keepif EXPR] [--stata [--string]] --out new.hbin\n");
        return 2;
    }
    std::string direction = a[1];
    std::string data = opt(a, "--data"), out = opt(a, "--out");
    std::vector<std::string> stubs = split_csv(opt(a, "--stubs"));
    std::vector<std::string> ivars = split_csv(opt(a, "--i"));
    std::string jvar = opt(a, "--j");
    if (data_missing(data) || out_missing(out) || stubs.empty() || ivars.empty() || jvar.empty()) {
        std::fprintf(stderr, "reshape: --data --stubs --i --j and --out are all required\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);

    // --stata: reshape.ado's own semantics (Frame::reshape_long_stata /
    // reshape_wide_stata); --string: its `string` option (j holds text).
    const bool stata = has(a, "--stata"), jstring = has(a, "--string");
    // e(sample) T2: PIET's `__piet_*` columns are no Stata variable; reshape.ado
    // would test them for constancy within i (w2_esample.log E9 `reshape wide y`
    // rc 0), and the observations they marked do not survive a reshape anyway.
    if (stata) drop_piet_hidden_columns(fr);
    Frame::ReshapeResult r;
    if (direction == "long") r = stata ? fr.reshape_long_stata(stubs, ivars, jvar, jstring)
                                       : fr.reshape_long(stubs, ivars, jvar);
    else if (direction == "wide") r = stata ? fr.reshape_wide_stata(stubs, ivars, jvar, jstring)
                                            : fr.reshape_wide(stubs, ivars, jvar);
    else { std::fprintf(stderr, "reshape: direction must be 'long' or 'wide', got '%s'\n", direction.c_str()); return 2; }

    int64_t nrows = r.frame.n_rows();  // capture before the move into commit_frame
    if (!commit_frame(std::move(r.frame), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "reshape %s: wrote %s (%lld rows, %zu j-values)\n",
                    direction.c_str(), out.c_str(), static_cast<long long>(nrows), r.j_values.size());
    return 0;
}

// cobra convert in.csv out.hbin — native CSV/TSV -> .hbin, no Python/pyarrow.
// Type inference per column (all-numeric -> f64, else factor); see Frame::from_csv.
int cmd_convert(const std::vector<std::string>& a) {
    std::string in = opt(a, "--data"), out = opt(a, "--out");
    // positional fallback: cobra convert IN OUT
    if (in.empty() && a.size() >= 2 && a[1].rfind("--", 0) != 0) in = a[1];
    if (out.empty() && a.size() >= 3 && a[2].rfind("--", 0) != 0) out = a[2];
    if (in.empty() || out.empty()) {
        std::fprintf(stderr,
            "usage: cobra convert <in.csv> <out.hbin> [--delim ,|tab|;] [--factor c1,c2]\n"
            "   or: cobra convert --data in.csv --out out.hbin [--delim ...] [--factor ...]\n"
            "  Reads a delimited text file natively (no Python/pyarrow). Columns whose every\n"
            "  non-missing cell is a number become f64; the rest become factors. Use --factor\n"
            "  to force string-coded FE/cluster keys to factor. Default delim: tab for .tsv, else ','.\n");
        return 2;
    }
    std::string ds = opt(a, "--delim");
    char delim;
    if (!ds.empty()) delim = (ds == "tab" || ds == "\\t" || ds == "\t") ? '\t' : ds[0];
    else delim = (in.size() >= 4 && in.substr(in.size() - 4) == ".tsv") ? '\t' : ',';
    Frame f = Frame::from_csv(in, delim, split_csv(opt(a, "--factor")));
    f.write_hbin_reserved(out);  // conversion output: reserve so the first gen can patch
    int nfac = 0;
    for (const auto& c : f.columns()) if (c.is_factor) ++nfac;
    std::fprintf(stderr, "convert: %s -> %s  (%lld rows, %d cols, %d factor)\n",
                 in.c_str(), out.c_str(), static_cast<long long>(f.n_rows()),
                 f.n_cols(), nfac);
    return 0;
}

// ===========================================================================
// MI-066: native .dta reader + data verbs (contract 1.8.0). All verbs read one
// (or two, for merge/append) .hbin/.dta and write a new .hbin, in the same
// style as collapse/tsfill/convert above. See src/data/frame.h for the Stata-
// parity semantics documented next to each Frame method / merge_frames().
// ===========================================================================

// cobra import-dta IN.dta OUT.hbin [--out-meta meta.json]
// Native Stata .dta reader (formats 115/117/118) -> .hbin. Optionally also
// writes a JSON metadata sidecar (variable labels, value labels, storage
// types, display formats) mirroring what PIET's Python loader currently
// derives from 3 separate pandas reads (_apply_dta_var_labels) — piet/native
// can populate SessionState.varMeta / valueLabels / labelDefs from this one
// native call instead.
int cmd_import_dta(const std::vector<std::string>& a) {
    std::string in = opt(a, "--data"), out = opt(a, "--out");
    if (in.empty() && a.size() >= 2 && a[1].rfind("--", 0) != 0) in = a[1];
    if (out.empty() && a.size() >= 3 && a[2].rfind("--", 0) != 0) out = a[2];
    if (in.empty() || out.empty()) {
        std::fprintf(stderr,
            "usage: cobra import-dta <in.dta> <out.hbin> [--out-meta meta.json]\n"
            "   or: cobra import-dta --data in.dta --out out.hbin [--out-meta meta.json]\n"
            "  Native Stata .dta reader (formats 115/117/118), no Python/pandas dependency.\n"
            "  Value-labeled numeric columns stay f64 (raw codes); genuine strings become\n"
            "  factor columns. --out-meta optionally writes variable/value-label metadata.\n");
        return 2;
    }
    DtaResult r = read_dta(in);
    r.frame.write_hbin_reserved(out);  // conversion output: reserve so the first gen can patch

    int nfac = 0;
    for (const auto& c : r.frame.columns()) if (c.is_factor) ++nfac;
    std::fprintf(stderr, "import-dta: %s -> %s  (format %d, %lld rows, %d cols, %d factor)\n",
                 in.c_str(), out.c_str(), r.format_version,
                 static_cast<long long>(r.frame.n_rows()), r.frame.n_cols(), nfac);

    std::string meta_path = opt(a, "--out-meta");
    if (!meta_path.empty()) {
        // BUG-1372: a label, value label, note or data label that is not UTF-8 (a
        // byte substr() made it, or the file's own) travels escaped as a .hbin
        // level does; PIET's applyDtaMeta and export-dta's --in-meta turn it back.
        json vars = json::array();
        for (const auto& vm : r.var_meta) {
            vars.push_back({{"name", vm.name}, {"label", hbin_level_json_text(vm.label)},
                            {"format", vm.format}, {"stata_type", vm.stata_type},
                            {"value_label_name", vm.value_label_name}});
        }
        json vlabs = json::object();
        for (const auto& vl : r.value_labels) {
            json m = json::object();
            for (const auto& kv : vl.map) m[std::to_string(kv.first)] = hbin_level_json_text(kv.second);
            vlabs[vl.name] = m;
        }
        json chars = json::array();
        for (const auto& ch : r.characteristics) {
            chars.push_back({{"varname", ch.varname}, {"charname", ch.charname},
                             {"value", hbin_level_json_text(ch.value)}});
        }
        json meta = {{"format_version", r.format_version},
                     {"data_label", hbin_level_json_text(r.data_label)},
                    {"n_rows", r.frame.n_rows()}, {"variables", vars}, {"value_labels", vlabs},
                    {"characteristics", chars}};
        // "timestamp" (BUG-199): the source .dta's own <timestamp> header field,
        // round-tripped through this sidecar the same append-only-when-non-empty
        // discipline as the .hbin header's "label" key — emitted only when the
        // source file actually carried one, so every existing --out-meta fixture
        // (which has no source timestamp) stays byte-identical.
        if (!r.timestamp.empty()) meta["timestamp"] = r.timestamp;
        // binary: keep the sidecar's newlines LF on every platform (the freeze
        // matrix byte-compares this fixture).
        std::ofstream mf(cecon::to_native_path(meta_path), std::ios::binary);
        mf << meta.dump(2);
    }
    return 0;
}

// cobra export-dta IN.hbin OUT.dta [--in-meta meta.json]
//
// P1-A3-WRITER-NUMERIC + P1-A3-WRITER-STRINGS-IMPL — the write direction of
// import-dta above: a Stata format-118 emitter (src/io/dta_writer.h carries the
// layout, the type-narrowing invariant and the Q-rulings). Numeric types, str#
// columns, strL GSOs, variable labels, value labels and <characteristics>. Long
// and binary strings use genuine strL records, never truncation.
//
// `--in-meta` is the symmetric counterpart of `import-dta --out-meta` and the ONLY
// way a source file's declared Stata storage types can reach the writer: the Frame
// carries f64/factor and nothing else, and the hbin header has no `stata_type` key,
// so the declared types die at the hbin boundary one command earlier
// (internal note P1-A3-WRITER-IMPL-brief.md §1). With the sidecar the round trip is a true
// type round trip; without it the writer scans and picks the narrowest LOSSLESS type,
// which changes no value either way — the guard `(double)(float)v == v` is exactly
// the condition under which `datasignature` is preserved, so gate ①
// (`import-dta(export-dta(X)) == X`) mechanically enforces the policy.
//
// Verb name is `export-dta`, not `save` (spec §8 Q10 / brief §5): the batch CLI is
// file-in/file-out with no resident dataset, so a `save` would imply an in-place
// session write the CLI does not honour. A PIET-side `save` surface is a separate
// question with its own 4-face coupled delta and is NOT part of this arc.
int cmd_export_dta(const std::vector<std::string>& a) {
    std::string in = opt(a, "--data"), out = opt(a, "--out");
    // positional fallback: cobra export-dta IN OUT (same shape as import-dta/convert)
    if (in.empty() && a.size() >= 2 && a[1].rfind("--", 0) != 0) in = a[1];
    if (out.empty() && a.size() >= 3 && a[2].rfind("--", 0) != 0) out = a[2];
    if (data_missing(in) || out.empty()) {
        std::fprintf(stderr,
            "usage: cobra export-dta <in.hbin> <out.dta> [--in-meta meta.json]\n"
            "                        [--strict-string-missing]\n"
            "   or: cobra export-dta --data in.hbin --out out.dta [--in-meta meta.json]\n"
            "  A missing string cell is written as \"\", which is what Stata stores (a string\n"
            "  variable's missing IS \"\" there); each such column is named on stderr because\n"
            "  the collapse is not recoverable on read-back. --strict-string-missing refuses\n"
            "  to write instead.\n"
            "  Writes Stata format 118, no Python dependency. Each numeric column is stored\n"
            "  in the narrowest type that holds every value LOSSLESSLY (byte/int/long/float\n"
            "  with the f32 round-trip guard, else double) — values never change. String\n"
            "  columns use str# or strL for long/binary values. --in-meta takes\n"
            "  `import-dta --out-meta` JSON back, so declared Stata types, display formats,\n"
            "  variable labels and value labels round-trip too.\n");
        return 2;
    }
    const Dataset& ds = acquire_data(in);
    Frame fr = frame_from_acquired(ds);
    drop_piet_hidden_columns(fr);

    DtaWriteOptions wo;
    std::string meta_path = opt(a, "--in-meta");
    if (!meta_path.empty()) {
        std::ifstream mf(cecon::to_native_path(meta_path), std::ios::binary);
        if (!mf) {
            std::fprintf(stderr, "export-dta: cannot open --in-meta %s\n", meta_path.c_str());
            return 1;
        }
        json meta;
        try {
            mf >> meta;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "export-dta: --in-meta %s is not valid JSON: %s\n",
                         meta_path.c_str(), e.what());
            return 1;
        }
        // BUG-1372: label text arrives escaped when it is not UTF-8 (import-dta's
        // --out-meta and PIET's save write it so); each unescape is a no-op otherwise.
        if (meta.contains("data_label") && meta["data_label"].is_string()) {
            wo.data_label = meta["data_label"].get<std::string>();
            hbin_level_unescape(wo.data_label);
        }
        // "timestamp" (BUG-199): symmetric with "data_label" above; absent key or
        // non-string value leaves wo.timestamp at its "" (Q8 byte-deterministic)
        // default, so an --in-meta from before this fix (or with no source
        // timestamp) behaves exactly as it did before.
        if (meta.contains("timestamp") && meta["timestamp"].is_string())
            wo.timestamp = meta["timestamp"].get<std::string>();
        if (meta.contains("variables") && meta["variables"].is_array()) {
            for (const auto& v : meta["variables"]) {
                if (!v.is_object() || !v.contains("name") || !v["name"].is_string()) continue;
                DtaWriteVarMeta m;
                m.name = v["name"].get<std::string>();
                if (v.contains("format") && v["format"].is_string())
                    m.format = v["format"].get<std::string>();
                if (v.contains("stata_type") && v["stata_type"].is_string())
                    m.stata_type = v["stata_type"].get<std::string>();
                if (v.contains("label") && v["label"].is_string()) {
                    m.label = v["label"].get<std::string>();
                    hbin_level_unescape(m.label);
                }
                if (v.contains("value_label_name") && v["value_label_name"].is_string())
                    m.value_label_name = v["value_label_name"].get<std::string>();
                wo.var_meta.push_back(std::move(m));
            }
        }
        // value_labels: {set_name: {"code": "text", …}} — exactly `--out-meta`'s shape,
        // codes stringified because JSON object keys are strings (see cmd_import_dta).
        if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
            for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
                if (!it.value().is_object()) continue;
                DtaWriteValueLabels vl;
                vl.name = it.key();
                for (auto e = it.value().begin(); e != it.value().end(); ++e) {
                    if (!e.value().is_string()) continue;
                    try {
                        std::string& text = vl.map[std::stoll(e.key())];
                        text = e.value().get<std::string>();
                        hbin_level_unescape(text);
                    } catch (const std::exception&) {
                        std::fprintf(stderr,
                                     "export-dta: --in-meta value_labels['%s'] has non-numeric "
                                     "code '%s'\n", vl.name.c_str(), e.key().c_str());
                        return 1;
                    }
                }
                wo.value_labels.push_back(std::move(vl));
            }
        }
        // characteristics: [{varname, charname, value}, …] — exactly `--out-meta`'s
        // shape (FIX:DTA-READER-CHARS-DROP closed the reader §5.2 gap), so this is now
        // a true round trip; a caller-authored array (not sourced from import-dta) is
        // still accepted verbatim, same as before.
        if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
            for (const auto& ch : meta["characteristics"]) {
                if (!ch.is_object()) continue;
                DtaWriteChar c;
                if (ch.contains("varname") && ch["varname"].is_string())
                    c.varname = ch["varname"].get<std::string>();
                if (ch.contains("charname") && ch["charname"].is_string())
                    c.charname = ch["charname"].get<std::string>();
                if (ch.contains("value") && ch["value"].is_string()) {
                    c.value = ch["value"].get<std::string>();
                    hbin_level_unescape(c.value);
                }
                if (c.varname.empty() || c.charname.empty()) continue;
                wo.characteristics.push_back(std::move(c));
            }
        }
    }

    wo.strict_string_missing = has(a, "--strict-string-missing");
    // --release 115|117|118 (default 118): `saveold`'s version() target.
    const std::string rel = opt(a, "--release", "118");
    if (rel == "115") wo.release = 115;
    else if (rel == "117") wo.release = 117;
    else if (rel == "118") wo.release = 118;
    else throw EngineError("bad_input", "export-dta: --release must be 115, 117 or 118");
    write_dta(fr, out, wo);
    std::fprintf(stderr, "export-dta: %s -> %s  (format 118, %lld rows, %d cols%s)\n",
                 in.empty() ? "<resident>" : in.c_str(), out.c_str(),
                 static_cast<long long>(fr.n_rows()), fr.n_cols(),
                 meta_path.empty() ? "" : ", typed from --in-meta");
    return 0;
}

int cmd_run(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::fprintf(stderr, "usage: cobra run <spec.json> [--out f.json]\n");
        return 2;
    }
    std::ifstream f(cecon::to_native_path(a[1]), std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open spec: %s\n", a[1].c_str());
        return 1;
    }
    json spec;
    f >> spec;
    const json& m = spec.at("model");
    std::string type = m.value("type", "feols");
    std::string data = spec.at("data").at("path").get<std::string>();
    std::string y = m.at("depvar").get<std::string>();
    std::vector<std::string> xs = m.value("exog", std::vector<std::string>{});
    std::vector<std::string> fes = m.value("fixed_effects", std::vector<std::string>{});
    std::string vcov_type = "iid";
    std::vector<std::string> clusters;
    if (spec.contains("vcov")) {
        vcov_type = spec["vcov"].value("type", "iid");
        clusters = spec["vcov"].value("cluster", std::vector<std::string>{});
    }
    json out;
    if (type == "iv2sls" || type == "iv") {
        std::vector<std::string> endog = m.value("endog", std::vector<std::string>{});
        std::vector<std::string> inst = m.value("instruments", std::vector<std::string>{});
        out = run_iv(data, y, xs, endog, inst, fes, vcov_type, clusters);
    } else if (type == "fepois" || type == "poisson") {
        out = run_fepois(data, y, xs, fes, m.value("offset", std::string("")), vcov_type,
                         clusters);
    } else {
        out = run_feols(data, y, xs, fes, vcov_type, clusters);
    }
    out["spec_file"] = a[1];
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// Run one model group (a set of outcomes that share an identical design) via the
// appropriate estimator, using the multi-outcome batch when there is >1 outcome
// so the FE absorption / first stage is computed once. Only used by cmd_grid()
// below (same TU) — no external linkage needed.
json grid_run_group(const std::string& data, const json& m, const std::vector<std::string>& deps) {
    std::string type = m.value("type", "feols");
    std::vector<std::string> xs = m.value("exog", std::vector<std::string>{});
    std::vector<std::string> fes = m.value("fixed_effects", std::vector<std::string>{});
    std::string vcov_type = "iid";
    std::vector<std::string> clusters;
    if (m.contains("vcov")) {
        vcov_type = m["vcov"].value("type", "iid");
        clusters = m["vcov"].value("cluster", std::vector<std::string>{});
    }
    std::string dataf = data;
    if (m.contains("keepif")) dataf = maybe_keepif(data, m["keepif"].get<std::string>());
    const bool multi = deps.size() > 1;
    json out;
    if (type == "iv2sls" || type == "iv" || type == "ivgmm") {
        std::vector<std::string> endog = m.value("endog", std::vector<std::string>{});
        std::vector<std::string> inst = m.value("instruments", std::vector<std::string>{});
        bool gmm = (type == "ivgmm") || m.value("gmm", false);
        if (gmm && !m.contains("vcov")) vcov_type = "hc1";  // Stata gmm default = robust
        bool ds_ = !m.value("keep_singletons", false);  // default drop, matches feols/ivreghdfe
        out = multi ? run_iv_multi(dataf, deps, xs, endog, inst, fes, vcov_type, clusters, gmm, nullptr, ds_)
                    : run_iv(dataf, deps[0], xs, endog, inst, fes, vcov_type, clusters, gmm, nullptr, ds_);
    } else if (type == "fepois" || type == "poisson") {
        std::string off = m.value("offset", std::string(""));
        bool dz = !m.value("keep_zero_fe", false);
        bool sx2 = m.value("scale", std::string("")) == "x2";
        out = multi ? run_fepois_multi(dataf, deps, xs, fes, off, vcov_type, clusters, dz, sx2)
                    : run_fepois(dataf, deps[0], xs, fes, off, vcov_type, clusters, dz, sx2);
    } else {
        bool ds_ = !m.value("keep_singletons", false);
        out = multi ? run_feols_multi(dataf, deps, xs, fes, vcov_type, clusters, ds_)
                    : run_feols(dataf, deps[0], xs, fes, vcov_type, clusters, ds_);
    }
    return out;
}

}  // namespace

// cobra grid <spec.json> — run many models from one spec. Each model inherits the
// top-level "defaults"; models that share an identical design (everything except
// the outcome) are auto-merged into a single multi-outcome batch so the FE
// absorption is done once. Output: {"estimator":"grid", results:[...]}.
int cmd_grid(const std::vector<std::string>& a) {
    if (a.size() < 2 || a[1].rfind("--", 0) == 0) {
        std::fprintf(stderr,
            "usage: cobra grid <spec.json> [--out f.json]\n"
            "  spec: {\"data\":\"f.hbin\", \"defaults\":{...}, \"models\":[{...},...]}\n"
            "  Each model: type(feols|iv|fepois), depvar(string or [list]), exog[], fixed_effects[],\n"
            "  endog[], instruments[], offset, vcov{type,cluster[]}, keepif. Models sharing every\n"
            "  field except depvar are batched together (one FE absorption).\n");
        return 2;
    }
    std::ifstream f(cecon::to_native_path(a[1]), std::ios::binary);
    if (!f) throw EngineError("data_io", "cannot open spec: " + a[1], a[1]);
    json spec; f >> spec;
    std::string top_data = spec.contains("data")
        ? (spec["data"].is_string() ? spec["data"].get<std::string>()
                                    : spec["data"].at("path").get<std::string>())
        : std::string();
    json defaults = spec.value("defaults", json::object());
    const json& models = spec.at("models");

    // Group models by their design signature (the merged spec minus depvar/data).
    std::vector<std::string> order;                         // signatures in first-seen order
    std::unordered_map<std::string, json> sig_spec;         // signature -> merged spec
    std::unordered_map<std::string, std::vector<std::string>> sig_deps;
    std::unordered_map<std::string, std::string> sig_data;
    for (const auto& mdl : models) {
        json merged = defaults;
        merged.update(mdl);                                 // model keys override defaults
        std::string data = merged.contains("data") ? merged["data"].get<std::string>() : top_data;
        // collect this entry's outcome(s)
        std::vector<std::string> deps;
        const json& dv = merged.at("depvar");
        if (dv.is_array()) for (const auto& d : dv) deps.push_back(d.get<std::string>());
        else deps.push_back(dv.get<std::string>());
        json sigj = merged; sigj.erase("depvar"); sigj.erase("data");
        std::string sig = data + "\n" + sigj.dump();
        if (!sig_spec.count(sig)) { order.push_back(sig); sig_spec[sig] = sigj; sig_data[sig] = data; }
        auto& acc = sig_deps[sig];
        for (auto& d : deps) acc.push_back(d);
    }

    json results = json::array();
    int n_models = 0;
    for (const std::string& sig : order) {
        const std::vector<std::string>& deps = sig_deps[sig];
        n_models += static_cast<int>(deps.size());
        json r = grid_run_group(sig_data[sig], sig_spec[sig], deps);
        if (g_format_tidy) apply_tidy(r);
        results.push_back(r);
    }
    json out = {{"estimator", "grid"}, {"spec_file", a[1]},
                {"n_groups", static_cast<int>(order.size())}, {"n_models", n_models},
                {"results", results}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
