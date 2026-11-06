// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Full Callaway-Sant'Anna csdid validated against the Stata golden.
//   csdid_attgt.csv: 361 ATT(g,t) rows (then 361 weight rows) in csdid's iteration
//                    order — outer loop cohort i (sorted gvar>0), inner loop j
//                    (sorted time minus first), matching our driver exactly.
//   csdid_event.csv: simple|event aggregates (Pre_avg, Post_avg, Tm5..Tp5).
//   csdid_golden.log: SIMPLE_ATT=3.18097568770293, SE=1.22088641062688.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "estimators/csdid.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

namespace {

double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

// Read CSV with header "name,att,se", return rows in file order.
std::vector<std::tuple<std::string, double, double>> read_csv(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    std::vector<std::tuple<std::string, double, double>> out;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string nm, a, s;
        std::getline(ss, nm, ',');
        std::getline(ss, a, ',');
        std::getline(ss, s, ',');
        out.emplace_back(nm, parse_stata(a), parse_stata(s));
    }
    return out;
}

// A long panel built in memory from Stata `generate` formulas (row r = _n - 1).
struct InlinePanel {
    std::vector<double> id, t, g, y, x1;
};

// tests/eval/golden/aer_w2/w2l_vce_shape_det.do:11-22: 180 rows, 30 ids x 6
// periods, cohorts 3/5 and never-treated.
InlinePanel vce_shape_det_panel() {
    InlinePanel P;
    for (int r = 0; r < 180; ++r) {
        const double n = r + 1;
        const double id = std::ceil(n / 6), t = std::fmod(n - 1, 6) + 1;
        const double x1 = std::fmod(n * 7, 11) + std::fmod(n, 5) / 8;
        const double z1 = std::fmod(n * 5, 13) + x1 / 2 + std::fmod(n * 17, 19) / 16;
        const double x2 = 0.5 * z1 + std::fmod(n * 3, 5) + std::fmod(n * 13, 7) / 8 +
                          std::fmod(n * 23, 29) / 32;
        const double y = 1 + 2 * x1 - x2 + id / 4 + std::fmod(n * 11, 17) / 4 +
                         (std::fmod(n * 37, 41) / 64 + std::fmod(n * 53, 59) / 64 +
                          std::fmod(n * 29, 31) / 64 - 1) * 3;
        const double im = std::fmod(id, 3);
        P.id.push_back(id); P.t.push_back(t); P.x1.push_back(x1); P.y.push_back(y);
        P.g.push_back(im == 0 ? 0 : (im == 1 ? 3 : 5));
    }
    return P;
}

// tests/eval/golden/aer_w2/w2d_uncovered.do:66-76 (w2d_mkdid): 30 ids x 6
// periods, cohorts 3/5 and never-treated, no covariate. `generate y` there has
// Stata's default storage type, float, so y is rounded to float here.
InlinePanel w2d_mkdid_panel() {
    InlinePanel P;
    for (int r = 0; r < 180; ++r) {
        const double n = r + 1;
        const double id = std::ceil(n / 6), t = std::fmod(n - 1, 6) + 1;
        const double gv = id <= 10 ? 0 : (id <= 20 ? 3 : 5);
        const double y = static_cast<double>(static_cast<float>(
            id / 10 + t / 5 + ((gv > 0 && t >= gv) ? 1.0 : 0.0) +
            std::fmod(n * n * 8931 + n * 5959, 10007) / 10007));
        P.id.push_back(id); P.t.push_back(t); P.g.push_back(gv); P.y.push_back(y);
        P.x1.push_back(0);
    }
    return P;
}

// The w2ag_csdid_drdid oracle's fixture (internal capture
// w2ag_csdid_drdid.do): 60 ids x 5 periods, cohorts 3/4/5 and
// never-treated, time-invariant x1.
InlinePanel w2ag_panel() {
    InlinePanel P;
    for (int r = 0; r < 300; ++r) {
        const double n = r + 1;
        const double id = std::ceil(n / 5), t = std::fmod(n - 1, 5) + 1;
        const double im = std::fmod(id, 5);
        const double gv = im == 1 ? 3 : (im == 2 ? 4 : (im == 3 ? 5 : 0));
        const double x1 = std::fmod(id * 7, 11) / 4 + std::fmod(id * 3, 5) / 8;
        const double y = 1 + x1 / 2 + std::fmod(id * 13, 17) / 8 + t / 4 +
                         std::fmod(id * t * 7 + id, 23) / 16 + std::fmod(n * 37, 41) / 64 +
                         ((gv > 0 && t >= gv) ? 1 + std::fmod(id, 3) / 4 : 0.0);
        P.id.push_back(id); P.t.push_back(t); P.g.push_back(gv); P.y.push_back(y);
        P.x1.push_back(x1);
    }
    return P;
}

int test_csdid_inline_fixtures() {
    int fail = 0;
    // (0) long2 posting (LAYOUT.md 1.4): per cohort every grid period except
    // g - 1, named t_<lo>_<hi>, then the wgt columns named over periods 2..5
    // positionally; and estat event's window is read on the positional event
    // list, where a pre-period cell sits one step later than its period.
    {
        InlinePanel P = w2ag_panel();
        CsdidConfig cfg;
        cfg.long2 = true;
        CsdidRunResult R = csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                           P.x1.data(), 1, 300, cfg);
        const std::vector<std::string> want = {
            "g3:t_1_2", "g3:t_2_3", "g3:t_2_4", "g3:t_2_5", "g4:t_1_3", "g4:t_2_3",
            "g4:t_3_4", "g4:t_3_5", "g5:t_1_4", "g5:t_2_4", "g5:t_3_4", "g5:t_4_5",
            "wgt:w3_2", "wgt:w3_3", "wgt:w3_4", "wgt:w3_5", "wgt:w4_2", "wgt:w4_3",
            "wgt:w4_4", "wgt:w4_5", "wgt:w5_2", "wgt:w5_3", "wgt:w5_4", "wgt:w5_5"};
        const bool names_ok = R.attgt.names == want;
        CsdidRunResult W = csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                           P.x1.data(), 1, 300, cfg, -2, 2);
        std::vector<int> es;
        for (const auto& eb : W.events) es.push_back(eb.e);
        const bool win_ok = es == std::vector<int>{-3, -2, 0, 1, 2};
        std::printf("  long2 posting names %s; window(-2 2) bins:", names_ok ? "ok" : "WRONG");
        for (int e : es) std::printf(" %d", e);
        std::printf("\n");
        if (!names_ok || !win_ok) { std::printf("    <<FAIL\n"); fail = 1; }
    }
    // (1) w2l_vce_shape_det.log:2114 csdid_ctl_vcecluster_se (and the seven other
    // rc-0 csdid fits, :2162 .. :2474): `csdid y x1, ivar(id) time(t) gvar(g)`,
    // se11 = sqrt(el(e(V),1,1)) = 3.104395981639785 (vce()/robust spellings are
    // ignored by csdid, so every one is the default fit).
    {
        InlinePanel P = vce_shape_det_panel();
        CsdidConfig cfg;
        CsdidRunResult R = csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                           P.x1.data(), 1, 180, cfg);
        const CsdidPosting& A = R.attgt;
        const bool shape = A.b.size() == 20 && A.names.size() == 20 && A.V.size() == 400 &&
                           A.names[0] == "g3:t_1_2" && A.names[4] == "g3:t_2_6" &&
                           A.names[9] == "g5:t_4_6" && A.names[10] == "wgt:w3_2" &&
                           A.names[19] == "wgt:w5_6";
        const double se11 = shape ? std::sqrt(A.V[0]) : std::nan("");
        const double rel = std::fabs(se11 - 3.104395981639785) / 3.104395981639785;
        std::printf("  w2l_vce_shape_det se11: shape %d se11 %.16g (Stata 3.104395981639785) rel %.2e\n",
                    shape ? 1 : 0, se11, rel);
        if (!shape || !(rel <= 1e-12)) { std::printf("    <<FAIL\n"); fail = 1; }
    }
    // (2) w2d_uncovered.log:4660-4703: `csdid y, ivar(id) time(t) gvar(gv)
    // method(reg)` (the 027 form) and its `estat event` table, printed to 7
    // significant digits. The no-covariate reg cell is also the dripw cell with
    // a constant-only propensity and outcome model (LAYOUT.md 1.5).
    {
        InlinePanel P = w2d_mkdid_panel();
        CsdidConfig creg;
        creg.method = CsdidMethod::Reg;
        CsdidRunResult R = csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                           nullptr, 0, 180, creg);
        CsdidConfig cdr;
        CsdidRunResult D = csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                           nullptr, 0, 180, cdr);
        const double att[10] = {-.0029679, .9970321, 1.194064, .8910962, .8881283,
                                .0940641,  -.0059359, .0940642, .994064, .7881283};
        const double se[10] = {.1951234, .1845892, .1994638, .161831,  .2022064,
                               .2016317, .1973485, .1980847, .1793605, .1958366};
        // As printed in the %9.0g table: 7 decimals below 1, 6 from 1 to 10;
        // agreement is half a unit in the last printed place.
        const auto ok7 = [](double got, double want) {
            const double unit = std::fabs(want) < 1 ? 1e-7 : 1e-6;
            return std::fabs(got - want) <= 0.5 * unit + 1e-12;
        };
        int bad = 0;
        double worst_eq = 0;
        if (R.cells.size() != 10 || D.cells.size() != 10) bad = 99;
        for (size_t k = 0; k < R.cells.size() && k < 10 && bad != 99; ++k) {
            if (!ok7(R.cells[k].att, att[k]) || !ok7(R.cells[k].se, se[k])) ++bad;
            worst_eq = std::max(worst_eq, std::fabs(R.cells[k].att - D.cells[k].att) /
                                              std::max(std::fabs(R.cells[k].att), 1e-3));
            worst_eq = std::max(worst_eq, std::fabs(R.cells[k].se - D.cells[k].se) / R.cells[k].se);
        }
        const double ev[9] = {.0445588, .9414672, .0940641, -.0059359, .0455481,
                              .9955481, .9910962, .8910962, .8881283};
        const double evse[9] = {.066973, .1208063, .2016317, .1973485, .1265099,
                                .1180096, .1234893, .161831, .2022064};
        int evbad = R.event_b.size() == 9 ? 0 : 99;
        for (size_t j = 0; j < 9 && evbad != 99; ++j) {
            const double sej = std::sqrt(R.event_V[j * 9 + j]);
            if (!ok7(R.event_b[j], ev[j]) || !ok7(sej, evse[j])) ++evbad;
        }
        std::printf("  w2d_uncovered method(reg): cells off %d, estat event off %d, "
                    "reg vs dripw (no covariate) worst rel %.2e\n", bad, evbad, worst_eq);
        if (bad || evbad || !(worst_eq <= 1e-12)) { std::printf("    <<FAIL\n"); fail = 1; }
    }
    // (3) The w2ag_csdid_drdid capture (internal capture
    // w2ag_csdid_drdid.log, StataBE 19.5, csdid 1.81): e(b)/e(V) elements printed at
    // %21x / %18.0g. Fits without a logit first step (method reg, or no covariate)
    // are held to 1e-9 (TOLERANCE.md T2); dripw with a covariate to 1e-8, since
    // Stata's propensity logit stops at its own convergence tolerance (T3).
    {
        InlinePanel P = w2ag_panel();
        std::vector<double> cl(300);
        for (int r = 0; r < 300; ++r) cl[r] = std::ceil(P.id[r] / 4);
        int bad = 0;
        double worst2 = 0, worst3 = 0;
        const auto pin = [&](const char* what, double got, double want, double rtol) {
            const double d = want == 0 ? std::fabs(got) : std::fabs(got - want) / std::fabs(want);
            // 7-digit table pins (rtol > 1e-8) carry their own half-unit and are not summarized.
            if (rtol <= 1e-9) worst2 = std::max(worst2, d);
            else if (rtol <= 1e-8) worst3 = std::max(worst3, d);
            if (!(d <= rtol)) { ++bad; std::printf("    %s: %.16g vs Stata %.16g (rel %.2e)\n", what, got, want, d); }
        };
        const auto run = [&](bool x, CsdidMethod m, CsdidComparison c, bool l2, bool clus,
                             int wlo = 0, int whi = 0) {
            CsdidConfig cfg;
            cfg.method = m;
            cfg.comparison = c;
            cfg.long2 = l2;
            if (clus) cfg.cluster = cl.data();
            return csdid_dripw_run(P.id.data(), P.t.data(), P.g.data(), P.y.data(),
                                   x ? P.x1.data() : nullptr, x ? 1 : 0, 300, cfg, wlo, whi);
        };
        const auto se = [](const CsdidPosting& A, size_t i) { return std::sqrt(A.V[i * A.b.size() + i]); };
        const auto v = [](const CsdidPosting& A, size_t i, size_t j) { return A.V[i * A.b.size() + j]; };
        const auto col = [](const CsdidPosting& A, const char* nm) {
            return static_cast<size_t>(std::find(A.names.begin(), A.names.end(), nm) - A.names.begin());
        };
        using M = CsdidMethod;
        using C = CsdidComparison;
        const double T2 = 1e-9, T3 = 1e-8;
        // ag_def / ag_reg (log:125-136, :758-769): `csdid y` = method reg.
        {
            CsdidRunResult R = run(false, M::Reg, C::Never, false, false);
            const CsdidPosting& A = R.attgt;
            pin("ag_def b11", A.b[0], 0x1.2aaaaaaaaaaabp-3, T2);
            pin("ag_def se11", se(A, 0), .2625256399721657, T2);
            pin("ag_def v12", v(A, 0, 1), -.0237713566532841, T2);
            const size_t k = col(A, "g4:t_3_4");
            pin("ag_def _b[g4:t_3_4]", A.b[k], 1.294270833333333, T2);
            pin("ag_def _se[g4:t_3_4]", se(A, k), .2109832086214541, T2);
            // estat event, estore(cs) (log:1378-1384): estat's block-diagonal V.
            const size_t E = R.event_b.size();
            pin("ag_evst_cs b1", R.event_b[0], .0163483796296296, T2);
            pin("ag_evst_cs se1", std::sqrt(R.event_V[0]), .1210764045835689, T2);
            pin("ag_evst_cs v13", R.event_V[2], 0.0, 0);
            pin("ag_evst_cs v34", R.event_V[2 * E + 3], .0007849092836733, T2);
            // estat simple, estore(A) post (log:1453): agg_simple.
            pin("ag_spost b1", R.agg_simple.b[0], 1.209852430555556, T2);
            pin("ag_spost se1", se(R.agg_simple, 0), .1181341402362786, T2);
        }
        // ag_evwin_post (log:1697-1703): estat event, window(-2 2) post.
        {
            CsdidRunResult R = run(false, M::Reg, C::Never, false, false, -2, 2);
            const size_t E = R.event_b.size();
            if (E != 7) ++bad;
            pin("ag_evwin_post b1", R.event_b[0], .0199652777777778, T2);
            pin("ag_evwin_post se1", std::sqrt(R.event_V[0]), .0843905925176183, T2);
            pin("ag_evwin_post v12", R.event_V[1], -.0054059248838351, T2);
            pin("ag_evwin_post v34", R.event_V[2 * E + 3], -.0046102971206477, T2);
        }
        // ag_l2ny (044 form, log:572-689): `long2 notyet`, method reg. The
        // not-yet comparison of cohort 4/5 pre-period cells excludes earlier
        // cohorts (BUG-956): g4:t_1_3 .046441, not .3815104 as before.
        {
            CsdidRunResult R = run(false, M::Reg, C::Notyet, true, false);
            const CsdidPosting& A = R.attgt;
            pin("ag_l2ny b11", A.b[0], -.1608072916666666, T2);
            pin("ag_l2ny se11", se(A, 0), .2271892701028092, T2);
            pin("ag_l2ny v12", v(A, 0, 1), .0181495392764056, T2);
            pin("ag_l2ny g4:t_1_3 (log:590)", A.b[col(A, "g4:t_1_3")], .046441, 5e-7 / .046441);
            pin("ag_l2ny g5:t_3_4 (log:597)", A.b[col(A, "g5:t_3_4")], .0507813, 5e-7 / .0507813);
            CsdidRunResult W = run(false, M::Reg, C::Notyet, true, false, -2, 2);
            pin("ag_l2ny_ev Pre_avg", W.event_b[0], -0x1.92f684bda12f3p-7, T2);
            pin("ag_l2ny_ev se1", std::sqrt(W.event_V[0]), .0876630998275162, T2);
        }
        // ag_ny (log:412 table): notyet with x1, cohort 4/5 pre-period cells.
        {
            CsdidRunResult R = run(true, M::Dripw, C::Notyet, false, false);
            const CsdidPosting& A = R.attgt;
            pin("ag_ny g4:t_1_2", A.b[col(A, "g4:t_1_2")], -.0810175, 5e-7 / .0810175);
            pin("ag_ny g5:t_1_2", A.b[col(A, "g5:t_1_2")], .1250347, 5e-7 / .1250347);
            pin("ag_ny g5:t_2_3", A.b[col(A, "g5:t_2_3")], .0154976, 5e-7 / .0154976);
            pin("ag_ny se11", se(A, 0), .2312465176209391, T3);
            pin("ag_ny _se[g4:t_3_4]", se(A, col(A, "g4:t_3_4")), .1956944375392162, T3);
        }
        // ag_regx (log:838-849): method reg with x1 (OLS first step, no logit).
        {
            CsdidRunResult R = run(true, M::Reg, C::Never, false, false);
            const CsdidPosting& A = R.attgt;
            pin("ag_regx b11", A.b[0], 0x1.aafa5dd0342eap-3, T2);
            pin("ag_regx se11", se(A, 0), .2857580071400538, T2);
            pin("ag_regx v12", v(A, 0, 1), -.0243546968513277, T2);
            const size_t k = col(A, "g4:t_3_4");
            pin("ag_regx _b[g4:t_3_4]", A.b[k], 1.293316213516331, T2);
            pin("ag_regx _se[g4:t_3_4]", se(A, k), .210477055174628, T2);
        }
        // ag_defx and agg() (log:236-254, :1056-1272): dripw with x1.
        {
            CsdidRunResult R = run(true, M::Dripw, C::Never, false, false);
            const CsdidPosting& A = R.attgt;
            pin("ag_defx se11", se(A, 0), .2876269602514325, T3);
            pin("ag_defx v12", v(A, 0, 1), -.0248898385851957, T3);
            pin("ag_defx vww", v(A, 12, 13), .000462962962963, T3);
            pin("ag_asimple b", R.agg_simple.b[0], 1.221991318428428, T3);
            pin("ag_asimple se", se(R.agg_simple, 0), .1204291794161211, T3);
            const CsdidPosting& Ev = R.agg_event;
            pin("ag_aevent _b[Tp0]", Ev.b[col(Ev, "Tp0")], 1.271554451486864, T3);
            pin("ag_aevent _se[Tp0]", se(Ev, col(Ev, "Tp0")), .1051500222014393, T3);
            pin("ag_aevent v12", v(Ev, 0, 1), -.0098124968866761, T3);
            const CsdidPosting& Gr = R.agg_group;
            pin("ag_agroup _b[G4]", Gr.b[col(Gr, "G4")], 1.253774832857531, T3);
            pin("ag_agroup _se[G4]", se(Gr, col(Gr, "G4")), .2003108772849304, T3);
            pin("ag_agroup v12", v(Gr, 0, 1), .0110030069967278, T3);
            const CsdidPosting& Ca = R.agg_calendar;
            pin("ag_acal _b[T4]", Ca.b[col(Ca, "T4")], 1.231701038328268, T3);
            pin("ag_acal _se[T4]", se(Ca, col(Ca, "T4")), .1708997339500578, T3);
            pin("ag_acal v12", v(Ca, 0, 1), .013107907535651, T3);
        }
        // ag_cl (log:361-373): cluster(cl), cl = ceil(id/4).
        {
            CsdidRunResult R = run(true, M::Dripw, C::Never, false, true);
            const CsdidPosting& A = R.attgt;
            pin("ag_cl se11", se(A, 0), .3479060037560119, T3);
            pin("ag_cl vw1", v(A, 12, 0), -.0011630151742087, T3);
            pin("ag_cl vww", v(A, 12, 13), .0001157407407407, T3);
            pin("ag_cl _se[g4:t_3_4]", se(A, col(A, "g4:t_3_4")), .2127962940083689, T3);
        }
        std::printf("  w2ag capture pins: off %d, worst rel %.2e (T2 pins) / %.2e (T3 pins)\n", bad,
                    worst2, worst3);
        if (bad) { std::printf("    <<FAIL\n"); fail = 1; }
    }
    return fail;
}

}  // namespace

int test_csdid_main() {
    std::printf("test_csdid:\n");
    const std::string hbin_path = cobra_test::validate_path("csdid_input.hbin");
    Dataset ds = Dataset::open(hbin_path);
    std::printf("  data: %lld rows, %d cols\n",
                static_cast<long long>(ds.n_rows()), ds.n_cols());

    const double* h_pid = ds.col("h_pid").f64();
    const double* wv    = ds.col("wv").f64();
    const double* gvar  = ds.col("gvar").f64();
    const double* y     = ds.col("visits_w").f64();
    const double* female= ds.col("female").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi= ds.col("edu_hi").f64();
    int64_t n_obs = ds.n_rows();

    std::vector<double> X(static_cast<size_t>(n_obs) * 3);
    for (int64_t i = 0; i < n_obs; ++i) {
        X[i] = female[i];
        X[n_obs + i] = age_z[i];
        X[2 * n_obs + i] = edu_hi[i];
    }

    CsdidConfig cfg;
    cfg.comparison = CsdidComparison::Never;
    auto t0 = std::chrono::steady_clock::now();
    auto R = csdid_dripw_run(h_pid, wv, gvar, y, X.data(), 3, n_obs, cfg, -5, 5);
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("  csdid_dripw_run: ok=%d N_units=%lld G=%zu T-1=%zu cells=%zu in %.2fs\n",
                R.ok ? 1 : 0,
                static_cast<long long>(R.n_units),
                R.glev.size(), R.tlev.size(), R.cells.size(), secs);

    // ---- Validate per-cell ATTs against csdid_attgt.csv (positional) ----
    auto golden = read_csv(cobra_test::validate_path("csdid_attgt.csv"));
    // First R.cells.size() rows are the ATTs (rest are weights).
    int n_checked = 0, n_fail_att = 0, n_fail_se = 0;
    double worst_att = 0, worst_se = 0;
    std::string worst_name;
    for (size_t k = 0; k < R.cells.size() && k < golden.size(); ++k) {
        const auto& ce = R.cells[k];
        if (!ce.ok) continue;
        const auto& [nm, gatt, gse] = golden[k];
        // Sanity: name agrees with our (base, t).
        char buf[64];
        std::snprintf(buf, sizeof(buf), "t_%g_%g", ce.base, ce.t);
        if (buf != nm) {
            std::printf("  ! name mismatch at k=%zu: cecon=%s golden=%s\n",
                        k, buf, nm.c_str());
        }
        double ra = std::fabs(ce.att - gatt) / (std::fabs(gatt) + 1e-10);
        double rs = std::fabs(ce.se  - gse ) / (std::fabs(gse ) + 1e-10);
        ++n_checked;
        if (ra > worst_att) { worst_att = ra; worst_name = nm + "(idx" + std::to_string(k) + ")"; }
        if (rs > worst_se ) { worst_se  = rs; }
        if (ra > 5e-6) ++n_fail_att;
        if (rs > 5e-4) ++n_fail_se;
    }
    std::printf("  per-cell ATT: checked=%d worst_att_rel=%.2e (%s) worst_se_rel=%.2e att_fail=%d se_fail=%d\n",
                n_checked, worst_att, worst_name.c_str(), worst_se, n_fail_att, n_fail_se);

    // ---- Simple ATT ----
    double exp_simple_att = 3.18097568770293;
    double exp_simple_se  = 1.22088641062688;
    double rs_att = std::fabs(R.simple_att - exp_simple_att) / std::fabs(exp_simple_att);
    double rs_se  = std::fabs(R.simple_se  - exp_simple_se ) / std::fabs(exp_simple_se);
    std::printf("  simple: cecon att=%.12g se=%.12g  stata att=%.12g se=%.12g  rel_att=%.2e rel_se=%.2e\n",
                R.simple_att, R.simple_se, exp_simple_att, exp_simple_se, rs_att, rs_se);

    // ---- Event ----
    auto egold = read_csv(cobra_test::validate_path("csdid_event.csv"));
    std::unordered_map<std::string, std::pair<double, double>> emap;
    for (const auto& [nm, a, s] : egold) emap[nm] = {a, s};
    auto get = [&](const std::string& nm) {
        auto it = emap.find(nm);
        if (it == emap.end()) return std::pair<double,double>{std::nan(""), std::nan("")};
        return it->second;
    };
    auto preg  = get("Pre_avg");
    auto postg = get("Post_avg");
    double r_pre   = std::fabs(R.pre_avg  - preg.first ) / std::fabs(preg.first );
    double r_post  = std::fabs(R.post_avg - postg.first) / std::fabs(postg.first);
    std::printf("  Pre_avg : cecon=%.12g (se %.12g) stata=%.12g (se %.12g) rel_att=%.2e\n",
                R.pre_avg, R.pre_avg_se, preg.first, preg.second, r_pre);
    std::printf("  Post_avg: cecon=%.12g (se %.12g) stata=%.12g (se %.12g) rel_att=%.2e\n",
                R.post_avg, R.post_avg_se, postg.first, postg.second, r_post);

    double max_event_rel = 0;
    for (const auto& eb : R.events) {
        std::string nm = (eb.e < 0)
            ? "Tm" + std::to_string(-eb.e)
            : "Tp" + std::to_string(eb.e);
        auto g = get(nm);
        if (std::isnan(g.first)) continue;
        double r = std::fabs(eb.att - g.first) / std::fabs(g.first);
        if (r > max_event_rel) max_event_rel = r;
        std::printf("  %-4s: cecon att=%.10g (se %.10g) stata att=%.10g (se %.10g) rel=%.2e\n",
                    nm.c_str(), eb.att, eb.se, g.first, g.second, r);
    }
    std::printf("  max event-bin rel = %.2e\n", max_event_rel);

    int fail = 0;

    // ---- csdid's own e(b)/e(V) (csdid-drdid-attgt-ematrix; LAYOUT.md 1.1).
    // csdid_attgt.csv IS Stata's e(b) after this fit, column by column: the 361
    // cells and then the 361 wgt columns, each with sqrt(e(V)[j,j]). The
    // posting must have the same 722 names in the same order, and the wgt
    // columns' values and SEs (exact functions of the sample counts) must
    // agree to the csv's printed precision.
    {
        const CsdidPosting& A = R.attgt;
        const size_t m = A.b.size();
        int name_bad = 0, wgt_bad = 0, sym_bad = 0, diag_bad = 0;
        double worst_wb = 0, worst_wse = 0;
        if (m != golden.size() || A.names.size() != m || A.V.size() != m * m) {
            std::printf("  attgt posting: %zu columns (%zu names, V %zu) vs Stata %zu  <<FAIL\n",
                        m, A.names.size(), A.V.size(), golden.size());
            fail = 1;
        } else {
            const size_t K = m / 2;
            for (size_t j = 0; j < m; ++j) {
                const auto& [nm, gb, gse] = golden[j];
                const std::string& full = A.names[j];
                const std::string want_eq = j < K ? "g" : "wgt";
                const size_t colon = full.find(':');
                if (colon == std::string::npos || full.substr(colon + 1) != nm ||
                    full.compare(0, want_eq.size(), want_eq) != 0)
                    ++name_bad;
                const double se = std::sqrt(A.V[j * m + j]);
                if (j >= K) {
                    const double rb = std::fabs(A.b[j] - gb) / std::fabs(gb);
                    const double rs = std::fabs(se - gse) / std::fabs(gse);
                    worst_wb = std::max(worst_wb, rb);
                    worst_wse = std::max(worst_wse, rs);
                    if (rb > 1e-11 || rs > 1e-11) ++wgt_bad;
                } else if (R.cells[j].ok &&
                           std::fabs(se - R.cells[j].se) > 1e-13 * R.cells[j].se) {
                    ++diag_bad;  // the cell block's diagonal IS the per-cell SE
                }
                for (size_t c = 0; c < j; ++c)
                    if (A.V[j * m + c] != A.V[c * m + j]) ++sym_bad;
            }
            std::printf("  attgt posting: %zu columns, name mismatches %d, wgt b/se worst rel "
                        "%.2e/%.2e (bad %d), cell diag != se %d, asymmetric %d\n",
                        m, name_bad, worst_wb, worst_wse, wgt_bad, diag_bad, sym_bad);
            if (name_bad || wgt_bad || diag_bad || sym_bad) fail = 1;
            if (A.names[0] != "g2:t_1_2" || A.names[K] != "wgt:w2_2") {
                std::printf("  attgt first names %s / %s  <<FAIL\n", A.names[0].c_str(),
                            A.names[K].c_str());
                fail = 1;
            }
        }
    }

    if (n_fail_att > 0) fail = 1;
    if (rs_att > 5e-6) fail = 1;
    if (rs_se  > 1e-2) fail = 1;     // cluster SE — slacker tolerance
    if (r_pre  > 5e-5) fail = 1;
    if (r_post > 5e-5) fail = 1;
    if (max_event_rel > 5e-5) fail = 1;
    fail |= test_csdid_inline_fixtures();
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
