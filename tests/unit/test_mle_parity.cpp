// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// logit / probit iterate parity with Stata's ml (FU-138, FU-210, FU-205).
//
// Stata reports the iterate at which ml's stop rule fires, not the fully
// converged MLE; binary_ml_fit (src/estimators/logit.h) reproduces that iterate:
// the constant-only start, Newton steps with halving / forward extension, the
// (tolerance OR ltolerance) AND nrtolerance stop, and the completely determined
// rows dropped from the objective. Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2b_estim.log
//     :625-630 logit d x1 x2 trace, :648 logit_ctl _se[x1] .8658455933085595
//     :619 logit_vce_robust _b[x1] 2.41264546148092 _se[x1] .730288375126513
//     :679 logit_vce_cluster _se[x1] .7409539084217277 (10 clusters)
//     :738-743 probit d x1 x2 trace, :762 probit_ctl _b[x1] 1.355178493014946
//              _se[x1] .4459396018157188
//   tests/eval/golden/aer_w2/w2e_probes.log (logit/probit d x1 x2 i.c)
//     :141 lp_default ic 8, :249 lp_tight ic 10, :304 lp_tight_difficult == :249,
//     :358 lp_probit_ctl ic 8, :412 lp_probit_tight_ctl ic 9 (_b[x1], e(ll) in %21x)
//   tests/eval/golden/aer_w2/w2c_ml_getmata.log
//     :246 probit_ctl e(ll) -90.32310551180336 _b[x1] 1.359804997021133
//          _se[x1] .197548178672465 ic 4
//   internal capture w2u_binary_ml_edges.log
//     (double fixture, METHOD.md section 1): the asis fits bm_l_xq_asis,
//     bm_p_dz1_asis, bm_h_d1_probit, bm_h_d2_probit pin the completely-determined
//     cutoffs (logit 18, probit 6); bm_l_xs_asis the all-determined fit (ic 2,
//     ll 0, V 0, e(rank) 0, e(df_m) -1); bm_[lp]_cg3_vce the model test after a
//     3-cluster VCE (Stata's `test, min`: e(df_m) 1, e(rank) 2, e(chi2) .);
//     bm_e_probit_str a string outcome, "no observations" r(2000).
//
// Fixtures = the do-files': every `generate` is default-typed (FLOAT), so x1/x2/y
// are rebuilt the way Stata stores them (double expression rounded to float; y
// and d read the float x1/x2). The engine's own data is double (BUG-471 /
// FU-136), so these arms run on the float replica.
//
// Tier (tests/eval/TOLERANCE.md T3): b, se, ll at rtol 1e-9; ic and the printed
// trace exact.
//
// Fails on the pre-change engine: logit_fit / probit_fit ran IRLS from 0 to a
// deviance change of 1e-14 (w2b logit 9 iterations, _b[x1] 2.4126457093 = 1.0e-7
// off), emitted no `ic`, refused --tolerance as an unknown flag, and probit
// fitted `--vce bogus` as iid at exit 0. The kernel arms do not compile there
// (no binary_ml_fit); the CLI arms (argv[1] = cobra) fail on the frozen binary.
// logit_fit / probit_fit are the refit entry points (cmds_postest, heckman,
// ivprobit); the refit arms pin them to the posted fit bit for bit.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/logit.h"
#include "estimators/probit.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cecon;

static int g_fail = 0;
static std::string g_cobra;
static fs::path g_dir;

static void close_to(double got, double want, double rtol, const std::string& what) {
    const double rel = std::fabs(got - want) / std::fabs(want);
    const bool ok = rel <= rtol;
    std::printf("  %-46s got %.16g want %.16g rel %.2e %s\n", what.c_str(), got, want, rel,
                ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static void equal_int(long long got, long long want, const std::string& what) {
    const bool ok = got == want;
    std::printf("  %-46s got %lld want %lld %s\n", what.c_str(), got, want, ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static void check(bool cond, const std::string& what) {
    std::printf("  %-46s %s\n", what.c_str(), cond ? "ok" : "<<FAIL");
    if (!cond) g_fail = 1;
}

// Stata's iteration log prints the log likelihood with 8 significant digits.
static void trace_is(const std::vector<double>& ilog, const std::vector<std::string>& want,
                     const std::string& what) {
    std::string got_s, want_s;
    bool ok = ilog.size() == want.size();
    for (size_t i = 0; i < ilog.size(); ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.8g", ilog[i]);
        got_s += std::string(i ? " " : "") + buf;
        if (i < want.size() && want[i] != buf) ok = false;
    }
    for (size_t i = 0; i < want.size(); ++i) want_s += (i ? " " : "") + want[i];
    std::printf("  %-46s got [%s]\n  %-46s want [%s] %s\n", what.c_str(), got_s.c_str(), "",
                want_s.c_str(), ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static double f32(double v) { return static_cast<double>(static_cast<float>(v)); }

// ---- fixtures -------------------------------------------------------------------

struct Fixture {
    int64_t n = 0;
    std::vector<std::string> cols;      // regressor names (constant appended last)
    std::vector<double> X;              // column-major n * (cols + 1), constant last
    std::vector<double> y;
    std::vector<int> id;
    int k() const { return static_cast<int>(cols.size()) + 1; }
};

static void finish(Fixture& f, const std::vector<std::vector<double>>& xs) {
    f.X.clear();
    for (const auto& c : xs) f.X.insert(f.X.end(), c.begin(), c.end());
    f.X.insert(f.X.end(), static_cast<size_t>(f.n), 1.0);
}

// w2b_estim.log:32-49: x1, x2, y float; d = (y > 12).
static Fixture w2b() {
    Fixture f;
    f.n = 60;
    f.cols = {"x1", "x2"};
    std::vector<double> x1, x2;
    for (int n = 1; n <= 60; ++n) {
        const int id = (n + 5) / 6;
        const double a = f32((n * 7) % 11 + (n % 5) / 10.0);
        const double b = f32((n * 3) % 5 + ((n * 13) % 7) / 10.0);
        const double yy = f32(1 + 2 * a - b + id / 3.0 + ((n * 11) % 17) / 5.0);
        x1.push_back(a);
        x2.push_back(b);
        f.y.push_back(yy > 12 ? 1.0 : 0.0);
        f.id.push_back(id);
    }
    finish(f, {x1, x2});
    return f;
}

// w2e_probes.log:65-84: as w2b plus byte c = mod(_n,4), g = mod(_n,5) in y;
// regressors x1 x2 i.c (base level 0 omitted).
static Fixture w2e() {
    Fixture f;
    f.n = 60;
    f.cols = {"x1", "x2", "c1", "c2", "c3"};
    std::vector<double> x1, x2, c1, c2, c3;
    for (int n = 1; n <= 60; ++n) {
        const int id = (n + 5) / 6, c = n % 4, g = n % 5;
        const double a = f32((n * 7) % 11 + (n % 5) / 10.0);
        const double b = f32((n * 3) % 5 + ((n * 13) % 7) / 10.0);
        const double yy = f32(1 + 2 * a - b + 0.7 * c + 0.4 * g + id / 3.0 + ((n * 11) % 17) / 5.0);
        x1.push_back(a);
        x2.push_back(b);
        c1.push_back(c == 1);
        c2.push_back(c == 2);
        c3.push_back(c == 3);
        f.y.push_back(yy > 12 ? 1.0 : 0.0);
        f.id.push_back(id);
    }
    finish(f, {x1, x2, c1, c2, c3});
    return f;
}

// w2c_ml_getmata.log:37-54: 200 rows, x1/x2 float, d from the float x1/x2.
static Fixture w2c() {
    Fixture f;
    f.n = 200;
    f.cols = {"x1", "x2"};
    std::vector<double> x1, x2;
    for (int n = 1; n <= 200; ++n) {
        const double a = f32(((n * 7) % 11) / 5.0 + (n % 5) / 10.0);
        const double b = f32(((n * 3) % 5) / 2.0 + ((n * 13) % 7) / 10.0);
        x1.push_back(a);
        x2.push_back(b);
        f.y.push_back((-0.5 + 0.8 * a - 0.6 * b + ((n * 11) % 17) / 8.0 - 1 > 0) ? 1.0 : 0.0);
        f.id.push_back((n + 9) / 10);
    }
    finish(f, {x1, x2});
    return f;
}

// w2u_binary_ml_edges.do (METHOD.md section 1): 200 rows, closed forms of _n,
// every continuous variable double (the engine's own type), so no float replica.
struct W2u {
    int64_t n = 200;
    std::vector<std::string> names;
    std::vector<std::vector<double>> cols;
    const std::vector<double>& col(const std::string& nm) const {
        for (size_t j = 0; j < names.size(); ++j)
            if (names[j] == nm) return cols[j];
        std::printf("  FAIL: no w2u column %s\n", nm.c_str());
        std::abort();
    }
    void add(const std::string& nm, std::vector<double> v) { names.push_back(nm); cols.push_back(std::move(v)); }
};

static W2u w2u() {
    W2u u;
    std::vector<double> x1, x2, y, dz0, dz1, xq, xs, z, s, zd, zp, g3;
    for (int n = 1; n <= 200; ++n) {
        const double a = ((n * 7) % 23) / 4.0 - 2.75 + (n % 3) / 8.0;
        const double b = ((n * 11) % 17) / 8.0 + ((n * 5) % 9) / 16.0;
        const double c = (n * 3) % 4;
        const double eps = (((n * 37) % 41) / 41.0 + ((n * 53) % 59) / 59.0 + ((n * 29) % 31) / 31.0 - 1.5) * 2.2;
        const double yy = (0.9 * a - 0.5 * b + 0.3 * c + eps + 0.4 > 0) ? 1.0 : 0.0;
        const double uu = (((n * 17) % 19) / 19.0 + ((n * 43) % 47) / 47.0 - 1) * 2;
        const double zz = ((n * 13) % 29) / 7.0 - 2;
        const double ss = (0.7 * a + 0.9 * zz + 0.6 * uu + 0.2 > 0) ? 1.0 : 0.0;
        x1.push_back(a);
        x2.push_back(b);
        y.push_back(yy);
        dz0.push_back((n % 9 == 0 && yy == 0.0) ? 1.0 : 0.0);
        dz1.push_back((n % 7 == 0 && yy == 1.0) ? 1.0 : 0.0);
        const double sep = (yy == 1.0) ? 1 + n % 4 : -1 - n % 4;
        xq.push_back(n % 5 == 0 ? 0.0 : sep);
        xs.push_back(sep);
        z.push_back(zz);
        s.push_back(ss);
        zd.push_back(zz + (n % 8 == 0 ? 40 * (2 * ss - 1) : 0));
        zp.push_back((n % 6 == 0 && ss == 0.0) ? 1.0 : 0.0);
        g3.push_back(((n + 4) / 5) % 3 + 1);
    }
    u.add("x1", x1); u.add("x2", x2); u.add("y", y); u.add("dz0", dz0); u.add("dz1", dz1);
    u.add("xq", xq); u.add("xs", xs); u.add("z", z); u.add("s", s); u.add("zd", zd); u.add("zp", zp);
    u.add("g3", g3);
    return u;
}

// Column-major design of the named regressors with the constant last.
static std::vector<double> w2u_design(const W2u& u, const std::vector<std::string>& xs) {
    std::vector<double> X;
    for (const auto& nm : xs) X.insert(X.end(), u.col(nm).begin(), u.col(nm).end());
    X.insert(X.end(), static_cast<size_t>(u.n), 1.0);
    return X;
}

static MlMaxOptions tight() {
    MlMaxOptions o;   // tolerance(1e-12) ltolerance(1e-15) nrtolerance(1e-12) iterate(300)
    o.tol = 1e-12;
    o.ltol = 1e-15;
    o.nrtol = 1e-12;
    o.maxiter = 300;
    return o;
}

static double hexd(const char* s) { return std::strtod(s, nullptr); }

// ---- CLI ------------------------------------------------------------------------

static std::string q(const std::string& s) { return "\"" + s + "\""; }

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string write_hbin(const Fixture& f, const std::string& name) {
    const std::string csv = (g_dir / (name + ".csv")).string(), hbin = (g_dir / (name + ".hbin")).string();
    std::ofstream o(csv, std::ios::binary);
    o << "id,d";
    for (const auto& c : f.cols) o << "," << c;
    o << "\n";
    for (int64_t i = 0; i < f.n; ++i) {
        char buf[64];
        o << f.id[static_cast<size_t>(i)] << "," << f.y[static_cast<size_t>(i)];
        for (size_t c = 0; c < f.cols.size(); ++c) {
            std::snprintf(buf, sizeof buf, "%.17g", f.X[c * static_cast<size_t>(f.n) + static_cast<size_t>(i)]);
            o << "," << buf;
        }
        o << "\n";
    }
    o.close();
    std::string cmd = q(g_cobra) + " convert " + q(csv) + " " + q(hbin) + " > " +
                      q((g_dir / (name + ".cvt")).string()) + " 2>&1";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) {
        std::printf("  FAIL: convert %s\n", name.c_str());
        g_fail = 1;
    }
    return hbin;
}

struct Run {
    int rc = -1;
    json out;
    std::string err;
};

static Run cobra(const std::string& args, const std::string& tag) {
    const std::string outp = (g_dir / (tag + ".json")).string(), errp = (g_dir / (tag + ".err")).string();
    std::error_code ec;
    fs::remove(outp, ec);
    std::string cmd = q(g_cobra) + " " + args + " --no-provenance --out " + q(outp) + " 2> " + q(errp);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    Run r;
    r.rc = std::system(cmd.c_str());
    r.err = slurp(errp);
    if (fs::exists(outp)) {
        try { r.out = json::parse(slurp(outp)); } catch (const std::exception&) { r.out = json(); }
    }
    return r;
}

// json::value deduces its return type from the default: NAN alone is a float.
static const double kNaN = std::numeric_limits<double>::quiet_NaN();

static double coef(const Run& r, const char* field) {
    if (!r.out.is_object() || !r.out.contains("coefs") || r.out["coefs"].empty()) return kNaN;
    return r.out["coefs"][0].value(field, kNaN);
}

// Top-level reply fields; a failed run (no reply) reads as absent, never throws
// (json::value on a null reply is type_error.306).
static long long ival(const Run& r, const char* key) {
    return (r.out.is_object() && r.out.contains(key) && r.out[key].is_number())
               ? r.out[key].get<long long>() : -1;
}
static double dval(const Run& r, const char* key) {
    return (r.out.is_object() && r.out.contains(key) && r.out[key].is_number())
               ? r.out[key].get<double>() : kNaN;
}
static std::string sval(const Run& r, const char* key) {
    return (r.out.is_object() && r.out.contains(key) && r.out[key].is_string())
               ? r.out[key].get<std::string>() : std::string();
}
static json jval(const Run& r, const char* key) {
    return (r.out.is_object() && r.out.contains(key)) ? r.out[key] : json();
}

static int ic_of(const Run& r) {
    return (r.out.is_object() && r.out.contains("ic") && r.out["ic"].is_number_integer())
               ? r.out["ic"].get<int>() : -1;
}

int main(int argc, char** argv) {
    const std::vector<std::string> logit_trace = {"-41.053906", "-9.6978756", "-7.702004",
                                                  "-7.4358736", "-7.4350072", "-7.4350069"};
    const std::vector<std::string> probit_trace = {"-41.053906", "-8.8919159", "-7.434731",
                                                   "-7.3213867", "-7.3209379", "-7.3209378"};

    std::printf("w2b logit d x1 x2 (w2b_estim.log:625-648)\n");
    const Fixture b = w2b();
    {
        LogitResult r = logit_fit_ml(b.X.data(), b.n, b.k(), b.y.data(), MlMaxOptions{});
        trace_is(r.ilog, logit_trace, "trace :625-630");
        equal_int(r.ic, 5, "e(ic)");
        check(r.converged, "e(converged) 1");
        close_to(r.beta[0], 2.41264546148092, 1e-9, "_b[x1] :619");
        close_to(std::sqrt(r.bread[0]), .8658455933085595, 1e-9, "_se[x1] iid :648");
    }
    std::printf("w2b probit d x1 x2 (w2b_estim.log:738-762)\n");
    {
        ProbitResult r = probit_fit_ml(b.X.data(), b.n, b.k(), b.y.data(), MlMaxOptions{});
        trace_is(r.ilog, probit_trace, "trace :738-743");
        equal_int(r.ic, 5, "e(ic)");
        close_to(r.beta[0], 1.355178493014946, 1e-9, "_b[x1] :762");
        close_to(std::sqrt(r.bread_oim[0]), .4459396018157188, 1e-9, "_se[x1] :762");
    }
    std::printf("w2b probit d x1 x2, noconstant (w2b_estim.log:712-716, :732)\n");
    {
        std::vector<double> X(b.X.begin(), b.X.begin() + 2 * b.n);
        ProbitResult r = probit_fit_ml(X.data(), b.n, 2, b.y.data(), MlMaxOptions{}, nullptr, false);
        trace_is(r.ilog, {"-41.588831", "-19.841011", "-19.788245", "-19.788119", "-19.788119"},
                 "trace :712-716");
        close_to(r.beta[0], .4484790120538529, 1e-9, "_b[x1] :732");
        close_to(std::sqrt(r.bread_oim[0]), .1036892933730802, 1e-9, "_se[x1] :732");
    }

    std::printf("w2e logit/probit d x1 x2 i.c (w2e_probes.log)\n");
    const Fixture e = w2e();
    {
        LogitResult r = logit_fit_ml(e.X.data(), e.n, e.k(), e.y.data(), MlMaxOptions{});
        equal_int(r.ic, 8, "lp_default e(ic) :141");
        close_to(r.beta[0], hexd("0x1.0c02ff9f2d8bep+3"), 1e-9, "lp_default _b[x1] :141");
        close_to(r.ll, -hexd("0x1.77239f16196fap+1"), 1e-9, "lp_default e(ll) :141");
        LogitResult t = logit_fit_ml(e.X.data(), e.n, e.k(), e.y.data(), tight());
        equal_int(t.ic, 10, "lp_tight e(ic) :249");
        close_to(t.beta[0], hexd("0x1.0c0301ffafbfep+3"), 1e-9, "lp_tight _b[x1] :249");
        close_to(t.ll, -hexd("0x1.77239f16196c7p+1"), 1e-9, "lp_tight e(ll) :249");
        MlMaxOptions td = tight();
        td.difficult = true;
        LogitResult d = logit_fit_ml(e.X.data(), e.n, e.k(), e.y.data(), td);
        check(d.ic == t.ic && d.beta == t.beta && d.ll == t.ll && d.bread == t.bread,
              "lp_tight_difficult == lp_tight :304");
        // logit_fit is the refit entry (cmds_postest's lincom/test/margins after
        // logit, selftest BUG-203): it must reproduce the posted fit bit for bit.
        LogitResult rf = logit_fit(e.X.data(), e.n, e.k(), e.y.data());
        check(rf.beta == r.beta && rf.bread == r.bread && rf.ic == r.ic,
              "logit_fit refit == logit_fit_ml default (bitwise)");
    }
    {
        ProbitResult r = probit_fit_ml(e.X.data(), e.n, e.k(), e.y.data(), MlMaxOptions{});
        equal_int(r.ic, 8, "lp_probit_ctl e(ic) :358");
        close_to(r.beta[0], hexd("0x1.2a415c0edc28ep+2"), 1e-9, "lp_probit_ctl _b[x1] :358");
        close_to(r.ll, -hexd("0x1.6df18b771fe0fp+1"), 1e-9, "lp_probit_ctl e(ll) :358");
        ProbitResult rf = probit_fit(e.X.data(), e.n, e.k(), e.y.data());
        check(rf.beta == r.beta && rf.bread_oim == r.bread_oim && rf.ic == r.ic,
              "probit_fit refit == probit_fit_ml default (bitwise)");
        ProbitObservedInfo oi = probit_observed_info(rf, e.X.data(), e.n, e.k(), e.y.data());
        check(oi.bread == r.bread_oim, "probit_observed_info bread == the ml bread_oim");
        ProbitResult t = probit_fit_ml(e.X.data(), e.n, e.k(), e.y.data(), tight());
        equal_int(t.ic, 9, "lp_probit_tight_ctl e(ic) :412");
        close_to(t.beta[0], hexd("0x1.2a415c2c689b2p+2"), 1e-9, "lp_probit_tight_ctl _b[x1] :412");
        close_to(t.ll, -hexd("0x1.6df18b771fe08p+1"), 1e-9, "lp_probit_tight_ctl e(ll) :412");
    }

    std::printf("w2c probit d x1 x2 (w2c_ml_getmata.log:246)\n");
    const Fixture c = w2c();
    {
        ProbitResult r = probit_fit_ml(c.X.data(), c.n, c.k(), c.y.data(), MlMaxOptions{});
        equal_int(r.ic, 4, "probit_ctl e(ic)");
        close_to(r.ll, -90.32310551180336, 1e-9, "probit_ctl e(ll)");
        close_to(r.beta[0], 1.359804997021133, 1e-9, "probit_ctl _b[x1]");
        close_to(r.beta[1], -.9509914208292267, 1e-9, "probit_ctl _b[x2]");
        close_to(r.beta[2], -.9355516247928987, 1e-9, "probit_ctl _b[_cons]");
        close_to(std::sqrt(r.bread_oim[0]), .197548178672465, 1e-9, "probit_ctl _se[x1]");
    }

    std::printf("w2u completely determined rows: logit t > 18, probit t > 6 (w2u_binary_ml_edges.log)\n");
    const W2u u = w2u();
    {
        // bm_l_xq_asis: xq separates y off its 40 tie rows, so _b[xq] diverges and
        // moves with every row that crosses the cutoff (ln 2^26 = 18.0218 gave
        // 21.99052175, 3.3e-8 off).
        const std::vector<double> X = w2u_design(u, {"x1", "xq"});
        LogitResult r = logit_fit_ml(X.data(), u.n, 3, u.col("y").data(), MlMaxOptions{});
        equal_int(r.ic, 12, "bm_l_xq_asis e(ic)");
        close_to(r.beta[1], hexd("0x1.5fd92c9487bfdp+4"), 1e-9, "bm_l_xq_asis_b2 _b[xq]");
        close_to(std::sqrt(r.bread[4]), hexd("0x1.957068abcd43ap+11"), 1e-9, "bm_l_xq_asis_b2 _se[xq]");
        equal_int(r.n_cds, 87, "bm_l_xq_asis e(N_cds)");
        equal_int(r.n_cdf, 71, "bm_l_xq_asis e(N_cdf)");
    }
    {
        // bm_p_dz1_asis: -invnormal(2^-31) = 6.1208 gave _b[dz1] 5.96740758 (3.7e-6
        // off) and e(N_cds) 9.
        const std::vector<double> X = w2u_design(u, {"x1", "dz1"});
        ProbitResult r = probit_fit_ml(X.data(), u.n, 3, u.col("y").data(), MlMaxOptions{});
        equal_int(r.ic, 9, "bm_p_dz1_asis e(ic)");
        close_to(r.beta[1], hexd("0x1.7dea5d3dc4caap+2"), 1e-9, "bm_p_dz1_asis_b2 _b[dz1]");
        close_to(std::sqrt(r.bread_oim[4]), hexd("0x1.fb24361254ed4p+7"), 1e-9, "bm_p_dz1_asis_b2 _se[dz1]");
        equal_int(r.n_cds, 10, "bm_p_dz1_asis e(N_cds)");
        equal_int(r.n_cdf, 0, "bm_p_dz1_asis e(N_cdf)");
    }
    {
        // bm_h_d1_probit / bm_h_d2_probit: heckman's first steps (probit ..., asis).
        const std::vector<double> X1 = w2u_design(u, {"x1", "zd"});
        ProbitResult d1 = probit_fit_ml(X1.data(), u.n, 3, u.col("s").data(), MlMaxOptions{});
        equal_int(d1.ic, 7, "bm_h_d1_probit e(ic)");
        equal_int(d1.n_cds, 20, "bm_h_d1_probit e(N_cds)");
        equal_int(d1.n_cdf, 8, "bm_h_d1_probit e(N_cdf)");
        close_to(d1.beta[0], hexd("0x1.3a8af17a8ec3ep+0"), 1e-9, "bm_h_d1_probit _b[x1]");
        close_to(std::sqrt(d1.bread_oim[0]), hexd("0x1.8a9af4f126748p-3"), 1e-9, "bm_h_d1_probit _se[x1]");
        const std::vector<double> X2 = w2u_design(u, {"x1", "z", "zp"});
        ProbitResult d2 = probit_fit_ml(X2.data(), u.n, 4, u.col("s").data(), MlMaxOptions{});
        equal_int(d2.ic, 9, "bm_h_d2_probit e(ic)");
        equal_int(d2.n_cds, 3, "bm_h_d2_probit e(N_cds)");
        equal_int(d2.n_cdf, 13, "bm_h_d2_probit e(N_cdf)");
        close_to(d2.beta[2], -hexd("0x1.79d4e1e4f1dbbp+2"), 1e-9, "bm_h_d2_probit _b[zp]");
        close_to(std::sqrt(d2.bread_oim[10]), hexd("0x1.c937ca5cc262ap+8"), 1e-9, "bm_h_d2_probit _se[zp]");
    }
    {
        // bm_l_xs_asis: xs separates y completely. One Newton step, extended until
        // every row is determined (ll 0); the zero Hessian's generalized inverse is
        // 0, so iteration 2 stops (ltolerance) with V = 0. The engine used to stop
        // on the singular Hessian (separated, not converged).
        const std::vector<double> X = w2u_design(u, {"x1", "xs"});
        LogitResult r = logit_fit_ml(X.data(), u.n, 3, u.col("y").data(), MlMaxOptions{});
        equal_int(r.ic, 2, "bm_l_xs_asis e(ic)");
        check(r.converged && !r.separated, "bm_l_xs_asis e(converged) 1");
        check(r.ll == 0.0, "bm_l_xs_asis_ll e(ll) 0");
        close_to(r.beta[0], hexd("0x1.741e8f04ccc23p+4"), 1e-9, "bm_l_xs_asis_b1 _b[x1]");
        close_to(r.beta[1], hexd("0x1.3b0109fc2caeap+6"), 1e-9, "bm_l_xs_asis_b2 _b[xs]");
        close_to(r.beta[2], -hexd("0x1.499d6c59d6fdcp+4"), 1e-9, "bm_l_xs_asis_b1 _b[_cons]");
        bool zero = r.bread.size() == 9;
        for (double v : r.bread) zero = zero && v == 0.0;
        check(zero, "bm_l_xs_asis V = 0 (every _se 0)");
        equal_int(r.n_cds, 107, "bm_l_xs_asis e(N_cds)");
        equal_int(r.n_cdf, 93, "bm_l_xs_asis e(N_cdf)");
    }

    if (argc >= 2) {
        g_cobra = argv[1];
        // Every flag the arms drive must be in apps/cmds/options_table.cpp.
#if defined(_WIN32)
        _putenv_s("COBRA_OPTIONS_STRICT", "1");
#else
        setenv("COBRA_OPTIONS_STRICT", "1", 1);
#endif
        std::error_code ec;
        g_dir = fs::temp_directory_path(ec) / ("cobra_mle_parity_" + std::to_string(cecon::native_process_id()));
        fs::create_directories(g_dir, ec);
        const std::string D = "--data " + q(write_hbin(b, "w2b"));
        const std::string E = "--data " + q(write_hbin(e, "w2e"));

        std::printf("CLI: cobra logit/probit on the w2b float replica\n");
        Run li = cobra("logit " + D + " --y d --x x1,x2", "logit_iid");
        equal_int(li.rc, 0, "logit rc");
        equal_int(ic_of(li), 5, "logit `ic` key");
        close_to(coef(li, "beta"), 2.41264546148092, 1e-9, "logit _b[x1] :619");
        close_to(coef(li, "se"), .8658455933085595, 1e-9, "logit _se[x1] iid :648");
        {
            std::vector<double> il;
            if (li.out.is_object() && li.out.contains("ilog"))
                for (const auto& v : li.out["ilog"]) il.push_back(v.get<double>());
            trace_is(il, logit_trace, "logit `ilog` key :625-630");
        }
        Run lr = cobra("logit " + D + " --y d --x x1,x2 --vce robust", "logit_robust");
        close_to(coef(lr, "se"), .730288375126513, 1e-9, "logit _se[x1] robust :619");
        Run lc = cobra("logit " + D + " --y d --x x1,x2 --vce cluster --cluster id", "logit_cluster");
        close_to(coef(lc, "se"), .7409539084217277, 1e-9, "logit _se[x1] cluster :679");
        Run pr = cobra("probit " + D + " --y d --x x1,x2", "probit");
        equal_int(ic_of(pr), 5, "probit `ic` key");
        close_to(coef(pr, "beta"), 1.355178493014946, 1e-9, "probit _b[x1] :762");
        close_to(coef(pr, "se"), .4459396018157188, 1e-9, "probit _se[x1] :762");
        // FU-205: an unsupported --vce is refused by name, never fitted as iid.
        Run pb = cobra("probit " + D + " --y d --x x1,x2 --vce bogus", "probit_bogus");
        check(pb.rc != 0 && pb.err.find("probit: --vce must be one of") != std::string::npos,
              "probit --vce bogus refused");
        // FU-464: vce(cluster) is served now; without --cluster it is refused by
        // name exactly as cmd_logit refuses it (the old pin: "no clustered probit").
        Run pc = cobra("probit " + D + " --y d --x x1,x2 --vce cluster", "probit_cluster");
        check(pc.rc != 0 && pc.err.find("probit: --vce cluster requires --cluster C") != std::string::npos,
              "probit --vce cluster without --cluster refused");

        std::printf("CLI: Wald chi2 after vce(robust), determined-row counts (FU-463, FU-471)\n");
        // w2b_estim.log:603 `logit d x1 x2, vce(robust)` prints Wald chi2(2) = 11.06;
        // the pre-change engine posted the OIM LR chi2 (67.24, :633).
        close_to(dval(lr, "chi2"), 11.06, 0.005 / 11.06, "logit robust Wald chi2 :603 (2 dp)");
        check(sval(lr, "chi2type") == "Wald", "logit robust chi2type Wald");
        close_to(dval(lc, "chi2"), 10.69, 0.005 / 10.69, "logit cluster Wald chi2 :662 (2 dp)");
        check(sval(li, "chi2type") == "LR", "logit oim chi2type LR");
        Run pnr = cobra("probit " + D + " --y d --x x1,x2 --vce robust --no-intercept", "probit_r_nocons");
        close_to(coef(pnr, "se"), .1031705727505295, 1e-9, "probit_r_nocons _se[x1] :706");
        close_to(dval(pnr, "chi2"), 19.17, 0.005 / 19.17, "probit_r_nocons Wald chi2 :692 (2 dp)");
        // "Note: N failures and M successes completely determined" = e(N_cdf), e(N_cds).
        equal_int(ival(li, "N_cdf"), 0, "logit d x1 x2 N_cdf (no note :623-648)");
        equal_int(ival(li, "N_cds"), 0, "logit d x1 x2 N_cds (no note :623-648)");
        equal_int(ival(pr, "N_cdf"), 2, "probit d x1 x2 N_cdf :757");
        equal_int(ival(pr, "N_cds"), 10, "probit d x1 x2 N_cds :757");
        equal_int(ival(pr, "df_m"), 2, "probit d x1 x2 df_m");
        equal_int(ival(pr, "k_vars"), 3, "probit d x1 x2 rank (k_vars)");

        std::printf("CLI: probit vce(cluster id) (FU-464)\n");
        {
            // The clustered score sandwich of g3_logit_probit_vce.log:197-234 (probit
            // vce(cluster rep78), factor G/(G-1)), recomputed here from the fit.
            Run pcl = cobra("probit " + D + " --y d --x x1,x2 --vce cluster --cluster id", "probit_cl");
            equal_int(pcl.rc, 0, "probit vce(cluster id) rc");
            equal_int(ival(pcl, "n_clusters"), 10, "probit n_clusters");
            check(sval(pcl, "chi2type") == "Wald", "probit cluster chi2type Wald");
            ProbitResult r = probit_fit_ml(b.X.data(), b.n, b.k(), b.y.data(), MlMaxOptions{});
            ProbitObservedInfo oi = probit_observed_info(r, b.X.data(), b.n, b.k(), b.y.data());
            const int K = b.k();
            std::vector<double> meat(static_cast<size_t>(K * K), 0.0);
            for (int g = 1; g <= 10; ++g) {
                std::vector<double> sg(static_cast<size_t>(K), 0.0);
                for (int64_t i = 0; i < b.n; ++i)
                    if (b.id[static_cast<size_t>(i)] == g)
                        for (int c = 0; c < K; ++c)
                            sg[static_cast<size_t>(c)] += oi.score[static_cast<size_t>(i)] *
                                b.X[static_cast<size_t>(c) * static_cast<size_t>(b.n) + static_cast<size_t>(i)];
                for (int c = 0; c < K; ++c)
                    for (int d = 0; d < K; ++d)
                        meat[static_cast<size_t>(c * K + d)] += sg[static_cast<size_t>(c)] * sg[static_cast<size_t>(d)];
            }
            double v00 = 0.0;   // (B M B)[0][0] * G/(G-1)
            for (int c = 0; c < K; ++c)
                for (int d = 0; d < K; ++d)
                    v00 += oi.bread[static_cast<size_t>(c)] * meat[static_cast<size_t>(c * K + d)] *
                           oi.bread[static_cast<size_t>(d)];
            close_to(coef(pcl, "se"), std::sqrt(v00 * 10.0 / 9.0), 1e-10, "probit _se[x1] cluster sandwich");
        }

        std::printf("CLI: perfect predictor dropped with its rows (FU-462/FU-467)\n");
        {
            // dz0 = (mod(_n,9)==0 & d==0) implies failure: Stata's perfect-predictor
            // rule 1 (x != 0 predicts failure). Stata omits dz0 and drops its rows,
            // so the fit is the fit on dz0 == 0 without dz0 (the w2u *_ctl
            // equality control) and e(rules) is (1, 0, 0, #dropped).
            Fixture pp, ctl;
            pp.n = b.n;
            pp.cols = {"x1", "dz0"};
            ctl.cols = {"x1"};
            std::vector<double> x1, dz0, cx1;
            long long ndrop = 0;
            for (int64_t i = 0; i < b.n; ++i) {
                const double xi = b.X[static_cast<size_t>(i)];
                const double di = ((i + 1) % 9 == 0 && b.y[static_cast<size_t>(i)] == 0.0) ? 1.0 : 0.0;
                x1.push_back(xi);
                dz0.push_back(di);
                pp.y.push_back(b.y[static_cast<size_t>(i)]);
                pp.id.push_back(b.id[static_cast<size_t>(i)]);
                if (di != 0.0) { ++ndrop; continue; }
                cx1.push_back(xi);
                ctl.y.push_back(b.y[static_cast<size_t>(i)]);
                ctl.id.push_back(b.id[static_cast<size_t>(i)]);
            }
            ctl.n = static_cast<int64_t>(cx1.size());
            finish(pp, {x1, dz0});
            finish(ctl, {cx1});
            check(ndrop > 0, "fixture has dz0 rows");
            const std::string P = "--data " + q(write_hbin(pp, "pp")), C = "--data " + q(write_hbin(ctl, "ppctl"));
            for (const char* cmd : {"logit", "probit"}) {
                const std::string c = cmd;
                Run f = cobra(c + " " + P + " --y d --x x1,dz0", c + "_dz0");
                Run k = cobra(c + " " + C + " --y d --x x1", c + "_dz0ctl");
                equal_int(f.rc, 0, c + " dz0 rc 0");
                equal_int(ival(f, "n_obs"), ctl.n, c + " dz0 N = rows with dz0 == 0");
                check(f.out.is_object() && f.out.contains("coefs") && f.out["coefs"].size() == 3 &&
                          f.out["coefs"][1].value("name", std::string()) == "dz0" &&
                          f.out["coefs"][1].value("beta", kNaN) == 0.0 &&
                          f.out["coefs"][1].value("se", kNaN) == 0.0,
                      c + " dz0 posted omitted: _b 0, _se 0");
                check(coef(f, "beta") == coef(k, "beta") && coef(f, "se") == coef(k, "se"),
                      c + " dz0 _b/_se[x1] == the dz0 == 0 control (bitwise)");
                for (const char* key : {"ll", "ll_0", "chi2"})
                    check(dval(f, key) == dval(k, key),
                          c + " dz0 e(" + key + ") == the control (bitwise)");
                check(jval(f, "omitted") == json::array({"dz0"}), c + " dz0 omitted list");
                check(jval(f, "rules") == json::array({json::array({1, 0.0, 0, ndrop})}),
                      c + " dz0 e(rules) (1, 0, 0, #dropped)");
                equal_int(ival(f, "df_m"), 1, c + " dz0 e(df_m)");
                equal_int(ival(f, "k_vars"), 2, c + " dz0 e(rank)");
                check(jval(k, "rules") == json::array({json::array({0, 0, 0, 0})}),
                      c + " control e(rules) J(1,4,0)");
                Run as = cobra(c + " " + P + " --y d --x x1,dz0 --asis", c + "_dz0_asis");
                equal_int(ival(as, "n_obs"), pp.n, c + " dz0 asis keeps every row");
                check(as.out.is_object() && !as.out.contains("omitted"), c + " dz0 asis keeps dz0");
            }
        }

        std::printf("CLI: w2u_binary_ml_edges (cg3 model test, xs asis, string outcome, refits)\n");
        {
            const std::string csv = (g_dir / "w2u.csv").string(), hbin = (g_dir / "w2u.hbin").string();
            {
                std::ofstream o(csv, std::ios::binary);
                // dz2 = (mod(_n,18)==0 & y==0) is nonzero only on dz0 rows, and w = x1 + 5*dz0
                // equals x1 there: after the dz0 rule both are constant / collinear on the rows left.
                o << "y,x1,x2,dz0,xs,g3,ystr,dz2,w\n";
                char buf[240];
                for (int64_t i = 0; i < u.n; ++i) {
                    const size_t r = static_cast<size_t>(i);
                    const double dz2 = ((i + 1) % 18 == 0 && u.col("y")[r] == 0.0) ? 1.0 : 0.0;
                    std::snprintf(buf, sizeof buf, "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.17g,%.17g\n",
                                  u.col("y")[r], u.col("x1")[r], u.col("x2")[r], u.col("dz0")[r], u.col("xs")[r],
                                  u.col("g3")[r], u.col("y")[r] == 1.0 ? "yes" : "no", dz2,
                                  u.col("x1")[r] + 5.0 * u.col("dz0")[r]);
                    o << buf;
                }
            }
            std::string cmd = q(g_cobra) + " convert " + q(csv) + " " + q(hbin) + " > " +
                              q((g_dir / "w2u.cvt").string()) + " 2>&1";
#if defined(_WIN32)
            cmd = "\"" + cmd + "\"";
#endif
            check(std::system(cmd.c_str()) == 0, "convert w2u");
            const std::string U = "--data " + q(hbin);
            for (const char* cmd1 : {"logit", "probit"}) {
                const std::string c = cmd1;
                // bm_[lp]_cg3_vce: 3 clusters, so V has rank 2 and `test, min` keeps one
                // slope: e(df_m) 1, e(rank) 2, e(chi2) missing (Wald chi2(1) = .).
                Run cg = cobra(c + " " + U + " --y y --x x1,x2 --vce cluster --cluster g3", c + "_cg3");
                equal_int(cg.rc, 0, c + " cg3 rc");
                equal_int(ival(cg, "n_clusters"), 3, c + " cg3 e(N_clust)");
                equal_int(ival(cg, "df_m"), 1, c + " bm_cg3_vce e(df_m)");
                equal_int(ival(cg, "k_vars"), 2, c + " bm_cg3_vce e(rank)");
                check(jval(cg, "chi2").is_null(), c + " bm_cg3_se e(chi2) missing");
                check(sval(cg, "chi2type") == "Wald", c + " bm_cg3_vce e(chi2type) Wald");
                // bm_[lp]_xs_asis: rc 0, every row determined, V = 0.
                Run xa = cobra(c + " " + U + " --y y --x x1,xs --asis", c + "_xs_asis");
                equal_int(xa.rc, 0, c + " bm_xs_asis rc 0");
                equal_int(ival(xa, "N_cds"), 107, c + " bm_xs_asis_fit e(N_cds)");
                equal_int(ival(xa, "N_cdf"), 93, c + " bm_xs_asis_fit e(N_cdf)");
                equal_int(ival(xa, "df_m"), -1, c + " bm_xs_asis_fit e(df_m)");
                equal_int(ival(xa, "k_vars"), 0, c + " bm_xs_asis_fit e(rank)");
                check(dval(xa, "ll") == 0.0, c + " bm_xs_asis_ll e(ll) 0");
                close_to(dval(xa, "chi2"), hexd("0x1.14472f9dc933cp+8"), 1e-9, c + " bm_xs_asis_ll e(chi2) LR");
                Run xn = cobra(c + " " + U + " --y y --x x1,xs", c + "_xs");
                check(xn.rc != 0 && xn.err.find("predicts data perfectly") != std::string::npos,
                      c + " bm_xs_fit (no asis) r(2000)");
                // bm_e_probit_str: a string variable marks every row out (mark.sthlp rule 6).
                Run st = cobra(c + " " + U + " --y ystr --x x1", c + "_str");
                check(st.rc != 0 && st.err.find("no_obs") != std::string::npos, c + " bm_e_str no_obs (r(2000))");
                // FU-541: the refit after a dropped perfect predictor is the dropped fit
                // (bm_l_dz0_pred row 5 is the dz0 == 0 fit); with --asis, the asis fit.
                Run fd = cobra(c + " " + U + " --y y --x x1,dz0", c + "_w2u_dz0");
                Run ld = cobra("lincom " + U + " --y y --x x1,dz0 --model " + c + " --exp x1", c + "_lincom_dz0");
                check(ld.rc == 0 && coef(ld, "b") == coef(fd, "beta") && coef(ld, "se") == coef(fd, "se"),
                      c + " lincom x1 after the dz0 drop == the posted _b/_se[x1]");
                equal_int(ival(ld, "n_obs"), 190, c + " lincom refit N 190 (bm_dz0_fit)");
                Run fa = cobra(c + " " + U + " --y y --x x1,dz0 --asis", c + "_w2u_dz0_asis");
                Run la = cobra("lincom " + U + " --y y --x x1,dz0 --model " + c + " --exp x1 --asis",
                               c + "_lincom_dz0_asis");
                check(la.rc == 0 && coef(la, "b") == coef(fa, "beta"), c + " lincom --asis == the asis fit");
                check(coef(la, "b") != coef(ld, "b"), c + " the asis and the dropped refits differ");
                // A regressor that turns constant (dz2) or collinear (w) only on the rows the
                // dz0 rule leaves is omitted too: the fit is the dz0 == 0 control
                // (bm_[lp]_dz0_ctl, w2u_binary_ml_edges.log:1096/:2124), never a singular
                // design stopped at iteration 0 with _b[x1] 0 (review of 45717e00).
                const double ctl_b = c == "logit" ? hexd("0x1.0365f05af90abp+0") : hexd("0x1.365f43f21a6e6p-1");
                for (const char* xs2 : {"x1,dz0,dz2", "x1,dz2,dz0", "x1,dz0,w"}) {
                    const std::string tag = c + " " + xs2;
                    Run f2 = cobra(c + " " + U + " --y y --x " + xs2, c + "_nested");
                    equal_int(f2.rc, 0, tag + " rc 0");
                    equal_int(ival(f2, "n_obs"), 190, tag + " N 190 (bm_dz0_ctl)");
                    check(f2.out.is_object() && f2.out.value("converged", false), tag + " converged");
                    close_to(coef(f2, "beta"), ctl_b, 1e-9, tag + " _b[x1] == bm_dz0_ctl");
                    check(coef(f2, "beta") == coef(fd, "beta") && coef(f2, "se") == coef(fd, "se"),
                          tag + " _b/_se[x1] == the x1,dz0 fit (bitwise)");
                    check(jval(f2, "omitted").is_array() && jval(f2, "omitted").size() == 2,
                          tag + " two omitted terms");
                    equal_int(ival(f2, "df_m"), 1, tag + " e(df_m)");
                    equal_int(ival(f2, "k_vars"), 2, tag + " e(rank)");
                    Run l2 = cobra("lincom " + U + " --y y --x " + xs2 + " --model " + c + " --exp x1",
                                   c + "_lincom_nested");
                    check(l2.rc == 0 && coef(l2, "b") == coef(fd, "beta") && coef(l2, "se") == coef(fd, "se"),
                          tag + " lincom x1 refit == the posted _b/_se[x1]");
                }
                Run r2 = cobra(c + " " + U + " --y y --x x1,dz0,dz2", c + "_nested_rules");
                check(jval(r2, "rules") == json::array({json::array({1, 0.0, 0, 10}), json::array({4, 0.0, 0, 0})}),
                      c + " x1,dz0,dz2 e(rules): dz0 (1,0,0,10), dz2 omitted for collinearity (4,0,0,0), measured on StataBE 19.5");
            }
        }

        std::printf("CLI: maximize options on the w2e float replica\n");
        const std::string tf = " --tolerance 1e-12 --ltolerance 1e-15 --nrtolerance 1e-12 --iterate 300";
        Run ld = cobra("logit " + E + " --y d --x x1,x2,c1,c2,c3", "lp_default");
        equal_int(ic_of(ld), 8, "lp_default ic :141");
        // BUG-1199: a fit stopped by iterate() before it converges is still posted,
        // at rc 0 (StataBE 19.5 `logit|probit y x, iterate(1)`: "Warning: Convergence
        // not achieved.", e(converged) 0); the CLI exited 1 with the whole reply.
        for (const std::string c : {"logit", "probit"}) {
            Run nc = cobra(c + " " + E + " --y d --x x1,x2,c1,c2,c3 --iterate 1", c + "_iter1");
            equal_int(nc.rc, 0, c + " --iterate 1 exit 0 (BUG-1199)");
            check(nc.out.is_object() && nc.out.contains("converged") && !nc.out.value("converged", true),
                  c + " --iterate 1 e(converged) 0");
        }
        close_to(ld.out.value("ll", kNaN), -hexd("0x1.77239f16196fap+1"), 1e-9, "lp_default ll :141");
        Run lt = cobra("logit " + E + " --y d --x x1,x2,c1,c2,c3" + tf, "lp_tight");
        equal_int(lt.rc, 0, "lp_tight rc (flags in the vocabulary)");
        equal_int(ic_of(lt), 10, "lp_tight ic :249");
        close_to(coef(lt, "beta"), hexd("0x1.0c0301ffafbfep+3"), 1e-9, "lp_tight _b[x1] :249");
        Run ltd = cobra("logit " + E + " --y d --x x1,x2,c1,c2,c3" + tf + " --difficult", "lp_tight_d");
        check(ltd.rc == 0 && ltd.out == lt.out, "lp_tight_difficult reply == lp_tight :304");
        equal_int(ival(ld, "N_cdf"), 9, "lp_default N_cdf :127");
        equal_int(ival(ld, "N_cds"), 27, "lp_default N_cds :127");
        Run pd = cobra("probit " + E + " --y d --x x1,x2,c1,c2,c3", "lp_probit_ctl");
        equal_int(ival(pd, "N_cdf"), 13, "lp_probit_ctl N_cdf :344");
        equal_int(ival(pd, "N_cds"), 32, "lp_probit_ctl N_cds :344");
        Run pt = cobra("probit " + E + " --y d --x x1,x2,c1,c2,c3" + tf, "lp_probit_tight");
        equal_int(ic_of(pt), 9, "lp_probit_tight_ctl ic :412");
        close_to(coef(pt, "beta"), hexd("0x1.2a415c2c689b2p+2"), 1e-9, "lp_probit_tight_ctl _b[x1] :412");
        Run bad = cobra("logit " + D + " --y d --x x1,x2 --tolerance -1", "bad_tol");
        check(bad.rc != 0 && bad.err.find("--tolerance must be") != std::string::npos,
              "--tolerance -1 refused");
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("(CLI arms skipped: no cobra binary given)\n");
    }

    std::printf(g_fail ? "test_mle_parity: FAIL\n" : "test_mle_parity: ok\n");
    return g_fail;
}
