// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Generic LF falsification: likelihood identity comes from the evaluator,
// including nonlinear means, multiple equations, sample/weight and refusals.
#include "selftest_w2.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "io/win_compat.h"
#include "core/expr_eval.h"
#include "lapack_dsyevr.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {
std::string run(piet::SessionState& s, const std::string& cmd) {
  for (const auto& b : piet::dispatch(cmd,s))
    if (b.is_object() && b.value("type",std::string()) == "error") return piet::eval::coarseRc(b);
  return "0";
}
double scalar(const piet::SessionState& s, const std::string& name) {
  const double* p = s.eval.eScalars.find(name);
  return p ? *p : std::nan("");
}
double coef(const piet::SessionState& s, size_t j) {
  auto b = s.eval.resolveEMatrix("b");
  return b && !b->data.empty() && j < b->data[0].size() ? b->data[0][j] : std::nan("");
}
}

void testW2Ml() {
  cecon::expr::Options expressionOptions;
  auto lazy = cecon::expr::Program::compile("cond(0,sqrt(-1),3)+cond(1,4,ln(-1))",expressionOptions);
  w2check(lazy.eval(nullptr,cecon::expr::VarSource{},nullptr)==7,
          "shared LF evaluator conditionals do not evaluate a domain-invalid unselected branch");
  auto rounded = cecon::expr::Program::compile("float(1/3)",expressionOptions);
  w2check(rounded.eval(nullptr,cecon::expr::VarSource{},nullptr)==static_cast<double>(static_cast<float>(1.0/3)),
          "shared LF evaluator float() preserves the generated intermediate storage boundary");
  // StataBE 19.5 %21x bits captured 2026-10-27 (internal notes remaining150-
  // 20261027/codex-recovery/invlogit-grid.log, normal-grid.log): invlogit() of a
  // negative argument is exp(x)/(1+exp(x)) (1/(1+exp(-x)) is 1 ulp off at both
  // points), and normal() is Cody's ANORM (0.5*erfc(-x/sqrt 2) is 2, 1 and 29
  // ulp off at these three points, one per branch of the approximation).
  auto pinned = [&](const char* text) {
    return cecon::expr::Program::compile(text, expressionOptions).eval(nullptr, cecon::expr::VarSource{}, nullptr);
  };
  w2check(pinned("invlogit(-0.0078125)") == 0x1.fe0000aaaa667p-2 &&
              pinned("invlogit(-7.94)") == 0x1.756066911d360p-12,
          "LF evaluator invlogit() of a negative argument reproduces StataBE's bits (exp(x)/(1+exp(x)))");
  w2check(pinned("normal(-0.664)") == 0x1.036ce996a11c0p-2 && pinned("normal(0.688)") == 0x1.823026887eedap-1 &&
              pinned("normal(-8)") == 0x1.669d2c90d55cfp-51 &&
              cecon::stata_normal(-0.664) == 0x1.036ce996a11c0p-2 && cecon::stata_normal(-8) == 0x1.669d2c90d55cfp-51,
          "normal() reproduces StataBE's bits on all three branches of Cody's ANORM");
  // exp() is 0 for an argument below -709 and libm's subnormal from -709 up, and
  // invlogit() inherits it (StataBE 19.5 %21x, 2026-10-27: exp(-709) =
  // +0.8bfe55de02338X-3ff, exp(-709.0000000000001) = invlogit(-717.5) = 0;
  // scratch-r150/tail_fixes/stata_probe/ex3.do; core/stata_arith.h).
  w2check(pinned("exp(-709)") == 0x0.8bfe55de02338p-1022 && pinned("invlogit(-709)") == 0x0.8bfe55de02338p-1022 &&
              pinned("exp(-709.0000000000001)") == 0 && pinned("invlogit(-717.5)") == 0 &&
              pinned("invlogit(-740.5)") == 0 && pinned("exp(-708.5)") == 0x0.e6cf6d08897acp-1022,
          "LF evaluator exp()/invlogit() underflow edge reproduces StataBE's bits (0 below -709)");
  // `^` is Stata's power, not libm pow (StataBE 19.5 %21x, 2026-10-27, 20,000
  // random doubles per exponent, scratch-r150/tail_fixes/stata_probe/pw.do,
  // pw2.do; core/stata_arith.h): |n| <= 64 a left-to-right product (of 1/x for
  // n < 0), otherwise exp(y*ln(x)). At each point libm's pow is 1-2 ulp off.
  w2check(pinned("4.241756170799917^2") == 0x1.1fe142dea332cp+4 &&
              pinned("5.556503909095005^4") == 0x1.dc9fe931d87efp+9 &&
              pinned("2.78361166961121^-2") == 0x1.084f36b13ee24p-3 &&
              pinned("9.283069191108344^0.5") == 0x1.85fdf78a52137p+1 &&
              pinned("(-2)^65") == -0x1.0000000000002p+65 && pinned("2^-1022") == 0x1.000000000007cp-1022,
          "LF evaluator `^` reproduces StataBE's bits (x*x, ((x*x)*x)*x, (1/x)*(1/x), exp(y*ln x))");
  // The overflow edge (StataBE 19.5 %21x, 2026-10-27, scratch-r150/tail_fixes/
  // stata_probe2): exp(709) = +1.d422d2be5dc9bX+3fe is finite, an argument above
  // 709 is `.`; `^` of 2^1023 or more is `.` (9.5e153^2) while 9.4e153^2 is
  // +1.f750b55516b59X+3fe and a negative result is not clamped.
  w2check(pinned("exp(709)") == 0x1.d422d2be5dc9bp+1022 && std::isnan(pinned("exp(709.0000000000001)")) &&
              std::isnan(pinned("exp(709.5)")) && pinned("9.4e153^2") == 0x1.f750b55516b59p+1022 &&
              std::isnan(pinned("9.5e153^2")) && pinned("(-4.5e102)^3") == -0x1.038859727d162p+1023,
          "LF evaluator exp()/`^` overflow edge reproduces StataBE (`.` above 709 / from 2^1023)");
  namespace fs = std::filesystem;
  const std::string dir = (fs::temp_directory_path()/"piet_selftest_ml").u8string();
  std::error_code ec;
  fs::create_directories(cecon::to_native_path(dir),ec);
  piet::SessionState s(dir);
  bool setup = true;
  for (const char* c : {"clear all","set obs 60","generate double x = mod(_n,5)",
        "generate double y = 1+mod(_n,3)","generate double w = 1+mod(_n,2)"})
    setup = run(s,c) == "0" && setup;
  w2check(setup,"ml generic fixture loads");
  w2check(run(s, "scalar __tl_e = exp(-717.5)") == "0" && s.eval.scalars.get("__tl_e", "<unset>") == "0" &&
              run(s, "scalar drop __tl_e") == "0",
          "display/scalar exp(-717.5) is 0 as in StataBE (argument below -709), not a subnormal");
  w2check(run(s, "scalar __tl_p = 4.241756170799917^2") == "0" &&
              run(s, "assert __tl_p == 17.992495412519176") == "0" &&
              run(s, "local __tl_q = 5.556503909095005^4") == "0" &&
              run(s, "assert `__tl_q' == 953.249304037772") == "0" &&
              run(s, "assert 2.78361166961121^-2 == 0.12905733803187147") == "0" &&
              run(s, "count if (x*0 + 4.241756170799917)^2 == 17.992495412519176") == "0" &&
              run(s, "assert r(N) == 60") == "0" && run(s, "scalar drop __tl_p") == "0",
          "display/scalar/local/assert/count-if `^` is Stata's power (x*x), not libm pow (1 ulp off here)");
  s.programs["lf_generic"] = {"lf_generic",false,{
    "args lnf a", "quietly replace `lnf' = -0.5*($ML_y1-exp(`a'))^2"}};
  w2check(run(s,"ml model lf lf_generic (mu:y=)") == "0" &&
          run(s,"ml maximize, difficult tolerance(1e-10) ltolerance(1e-12) nrtolerance(1e-12)") == "0" &&
          std::fabs(coef(s,0)-std::log(2.0)) < 1e-7,
          "ml evaluates an arbitrary nonlinear mean likelihood (not a binary-model mapping)");
  const auto V = s.eval.resolveEMatrix("V");
  w2check(V && V->data.size()==1 && std::fabs(V->data[0][0]-1.0/240) < 1e-8 && scalar(s,"converged")==1,
          "ml publishes full observed covariance for nonlinear LF");
  s.programs["lf_normal"] = {"lf_normal",false,{
    "args lnf mu sig", "tempvar tmp", "local twopi = \"(2*_pi)\"",
    "quietly gen double `tmp' = -.5*ln(`twopi')-.5*(($ML_y1-`mu')/exp(`sig'))^2-`sig'",
    "quietly replace `lnf' = `tmp'"}};
  w2check(run(s,"ml model lf lf_normal (y=x) (lnsigma:), technique(bhhh 5 nr 5)")=="0" &&
          run(s,"ml init 0 2 0, copy")=="0" && run(s,"ml maximize, difficult iterate(100)")=="0" &&
          scalar(s,"k_eq")==2 && scalar(s,"N")==60 && std::fabs(coef(s,0))<1e-5 && std::fabs(coef(s,1)-2)<1e-5 &&
          std::fabs(coef(s,2)-std::log(std::sqrt(2.0/3)))<1e-5,
          "ml compiles text locals/tempvars and fits a multi-equation normal LF");
  w2check(run(s,"ml init 1 2, copy")=="503", "ml rejects initial-vector dimension mismatch");
  w2check(run(s,"display \"`: colnames e(b)'|`: coleq e(b)'\"")=="0",
          "ML equation names expand inline through the same stripe resolver as local assignments");
  s.programs["lf_hazard"] = {"lf_hazard",false,{"args lnf a",
    "replace `lnf' = $ML_y1*ln(1-exp(-`a'))-(1-$ML_y1)*`a'"}};
  w2check(run(s,"gen byte event = mod(_n,4)==0")=="0" &&
          run(s,"ml model lf lf_hazard (rate:event=)")=="0" && run(s,"ml init .2, copy")=="0" &&
          run(s,"ml maximize, tolerance(1e-11) ltolerance(1e-13) nrtolerance(1e-13)")=="0" &&
          std::fabs(coef(s,0)+std::log(.75))<1e-7 && scalar(s,"ic")==4,
          "ML recovers the constant-hazard optimum to deriv()'s step bias in Stata's 4 iterations "
          "(StataBE 19.5 on this fixture: _b[_cons]+ln(.75) = 3.5e-9, e(ic) 4; the former 1e-11 "
          "bound pinned PIET's own stencil, which Stata's lf numerics do not reach)");
  s.programs["lf_split"] = {"lf_split",false,{"args lnf a",
    "replace `lnf' = -.5*($ML_y1-`a')^2 if $ML_y1<=2",
    "replace `lnf' = -.5*($ML_y1-`a')^2 if $ML_y1>2"}};
  w2check(run(s,"ml model lf lf_split (y=)")=="0" && run(s,"ml maximize")=="0" &&
          std::fabs(coef(s,0)-2)<1e-7,
          "ML composes sequential data-qualified likelihood assignments without estimator-specific mappings");
  s.programs["lf_float"] = {"lf_float",false,{"args lnf a","tempvar t",
    "gen `t' = -.5*($ML_y1-`a')^2-.1","replace `lnf' = `t'"}};
  const double floatLl = 20*(static_cast<double>(static_cast<float>(-.6))+
      static_cast<double>(static_cast<float>(-2.1))+static_cast<double>(static_cast<float>(-4.6)));
  w2check(run(s,"ml model lf lf_float (y=)")=="0" && run(s,"ml maximize, iterate(0)")=="0" &&
          std::fabs(scalar(s,"ll")-floatLl)<1e-10,
          "ML objective includes actual default-float generated tempvar values");
  w2check(run(s,"ml model lf lf_generic (mu:y=) if x<2 [pw=w]")=="0" &&
          run(s,"ml maximize, difficult")=="0" && scalar(s,"N")==24 &&
          s.eval.eStrings.find("vce") && *s.eval.eStrings.find("vce")=="robust",
          "ml restricts sample and defaults pweights to robust VCE");
  s.programs["lf_bad"] = {"lf_bad",false,{"args lnf a",
    "replace `lnf' = -.5*($ML_y1-`a')^2", "display 1"}};
  w2check(run(s,"ml model lf lf_bad (y=)")=="198" && run(s,"ml maximize")=="198",
          "ml refuses arbitrary evaluator side effects and cannot replay the previous pending model");
  w2check(run(s,"ml model lf absent_evaluator (y=)")=="199", "ml names absent evaluators");
  w2check(run(s,"ml model lf lf_generic (mu:y=), technique(bfgs)")=="198", "ml refuses unsupported techniques");
  w2check(run(s,"ml model lf lf_generic (mu:y=)")=="0" && run(s,"ml init .5, copy")=="0" &&
          run(s,"ml maximize, iterate(0)")=="0" &&
          scalar(s,"converged")==0 && scalar(s,"ic")==0,
          "ml iteration budget publishes a nonconverged result honestly");
  w2check(run(s,"ml clear")=="0" && run(s,"ml maximize")=="198", "ml clear resets pending state");
  w2check(run(s,"mata: M_ml=(1,2\\3,4)")=="0" &&
          run(s,"mata: st_numscalar(\"ml_rows\",rows(M_ml))")=="0" &&
          (s.eval.scalars.get("ml_rows")=="2.0" || s.eval.scalars.get("ml_rows")=="2"),
          "Mata st_numscalar publishes a real scalar from a matrix shape expression");
  w2check(run(s,"pca x y")=="0" && run(s,"local pca_matrices : e(matrices)")=="0" &&
          s.eval.locals.get("pca_matrices")=="sds means components C Psi Ev L",
          "PCA reports the complete ordered namespace of matrices it actually posts");
  w2check(run(s,"regress y x")=="0" && s.eval.findClassMatrix('e',"components")==nullptr,
          "a later regression cannot read stale PCA matrices from the previous fit");
  w2check(run(s,"ml model lf lf_generic (mu:y=)")=="0" && run(s,"clear all")=="0" &&
          !s.eval.mlState, "clear all resets the pending ML model");
  // moptimize lf arithmetic, bit for bit: the w2c_ml_getmata.do probit fixture,
  // `ml maximize, iterate(2)`. StataBE 19.5's e(b) after two NR iterations
  // (moptimize_result_coefs, %21x, captured 2026-10-27, codex-recovery/
  // initial-trace.log P2) requires Stata's normal(), the plain-sum gradient and
  // Hessian, Stata's invsym sweep and unfused arithmetic all at once.
  bool probit = true;
  for (const char* c : {"clear all","set obs 200","generate x1 = mod(_n*7,11)/5 + mod(_n,5)/10",
        "generate x2 = mod(_n*3,5)/2 + mod(_n*13,7)/10",
        "generate byte d = (-0.5 + 0.8*x1 - 0.6*x2 + mod(_n*11,17)/8 - 1 > 0)"})
    probit = run(s,c) == "0" && probit;
  s.programs["lf_probit"] = {"lf_probit",false,{
    "args lnf xb", "quietly replace `lnf' = $ML_y1*ln(normal(`xb')) + (1-$ML_y1)*ln(normal(-`xb'))"}};
  w2check(probit && run(s,"ml model lf lf_probit (d = x1 x2)")=="0" && run(s,"ml maximize, iterate(2)")=="0" &&
          coef(s,0)==0x1.5a300c49e29d1p+0 && coef(s,1)==-0x1.e31798083efacp-1 && coef(s,2)==-0x1.dce57f8e34170p-1,
          "ml lf probit iterate(2) reproduces StataBE's e(b) bits");
  // Mata symeigensystem() is LAPACK DSYEVR(JOBZ='V',RANGE='A',UPLO='L'):
  // StataBE 19.5 bundled LAPACK 3.12.0, captured 2026-10-27 by calling
  // StataBE.app/Contents/Frameworks/liblapack.3.12.0.dylib's own dsyevr_
  // (scratch-r150/dsyevr/harness/pins_oracle.txt). W ascending, Z column-major.
  // The order-3 matrix is lf_052's -H at ml iteration 0, whose eigenvectors
  // StataBE's Mata symeigensystem() returns in these exact bits (reversed).
  {
    struct EigPin { int n; std::vector<double> a, w, z; };
    const std::vector<EigPin> pins = {
        {2, {2, -1.5, -1.5, 0.25},
         {-0x1.391dcd650cb73p-1, 0x1.6e477359432ddp+1},
         {-0x1.fe03a18e3414bp-2, -0x1.bbfa0e6488b52p-1, -0x1.bbfa0e6488b52p-1, 0x1.fe03a18e3414bp-2}},
        {3, {0x1.222d1945e139cp+9, 0x1.5dcc8758e5ec7p+8, 0x1.171bf39eef43cp+0, 0x1.5dcc8758e5ec7p+8,
             0x1.909a7db07e8dap+7, 0x1.1a77f05c3f3b6p+6, 0x1.171bf39eef43cp+0, 0x1.1a77f05c3f3b6p+6,
             0x1.32c3693ea25f5p+7},
         {-0x1.be3fc0e2e13aap+4, 0x1.56b2ff3fd269ap+7, 0x1.8b49d138cbe42p+9},
         {0x1.e3a66310b99fdp-2, -0x1.a500249de95e9p-1, 0x1.451ab7570086cp-2, -0x1.b0afd13ed3237p-3,
          0x1.f3e0bc277e7bap-3, 0x1.e49686401cfb7p-1, 0x1.b622efa6ced56p-1, 0x1.073882e104972p-1,
          0x1.debcd9d6cb4f1p-5}},
        {4, {4, 1, -2, 0.5, 1, 3, 0, -1, -2, 0, 2.5, 1.5, 0.5, -1, 1.5, 1},
         {-0x1.96045cf563568p-1, 0x1.3ea2c7d62be24p+1, 0x1.85fe245bef741p+1, 0x1.707015859ec12p+2},
         {0x1.6e6195a48729ep-2, -0x1.1fc83b7b496b4p-2, 0x1.146c7c0916f48p-1, -0x1.6a96abe43b8e6p-1,
          0x1.c48b36868bff8p-2, 0x1.1e912d06bf7a2p-2, 0x1.3d695e907cd4ap-1, 0x1.2b70d14dec9d1p-1,
          0x1.4941784f72c14p-2, -0x1.b60a0e833ba8cp-1, -0x1.791a15c0aa39dp-3, 0x1.7245b236319c7p-2,
          0x1.83aa27e151a8dp-1, 0x1.54c4b318d1d3ap-2, -0x1.13dad922d38c7p-1, -0x1.483acf2613ec2p-3}}};
    bool eig = true;
    for (const auto& p : pins) {
      std::vector<double> w, z;
      eig = piet::lapack::dsyevrVAL(p.n, p.a, 0.0, w, z) == 0 && w == p.w && z == p.z && eig;
    }
    w2check(eig, "DSYEVR translation reproduces StataBE's bundled LAPACK 3.12.0 bits (orders 2, 3, 4)");
    // Orders 40 and 150 take ILAENV's blocked DSYTRD (DLATRD + DSYR2K) and, at
    // 150, the blocked DORMQR (DLARFT + DLARFB) under DSYEVR's own optimal
    // workspace, which is what StataBE uses: these are StataBE 19.5's Mata
    // symeigensystem() bits for A[i,j] = (mod(i*j*7+i+3*j,23)-11)/8 (captured
    // 2026-10-27, scratch-r150/dsyevr/stata_probe/eig.do; Mata's L is
    // descending and its X[.,1] is DSYEVR's last column).
    const auto blocked = [](int n, double l1, double ln, double x11, double xn1, double x1n, double x32,
                            double scale = 1.0) {
      std::vector<double> a(static_cast<size_t>(n) * n), w, z;
      for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= i; ++j)
          a[(i - 1) + static_cast<size_t>(j - 1) * n] = a[(j - 1) + static_cast<size_t>(i - 1) * n] =
              scale == 1.0 ? ((i * j * 7 + i + 3 * j) % 23 - 11) / 8.0
                           : scale * (((i * j * 7 + i + 3 * j) % 23 - 11) / 8.0);
      if (piet::lapack::dsyevrVAL(n, a, 0.0, w, z) != 0) return false;
      const auto X = [&](int r, int c) { return z[(r - 1) + static_cast<size_t>(n - c) * n]; };
      return w[n - 1] == l1 && w[0] == ln && X(1, 1) == x11 && X(n, 1) == xn1 && X(1, n) == x1n &&
             X(3, 2) == x32;
    };
    w2check(blocked(40, 0x1.2f48e62cb5347p+3, -0x1.2607be7ae5c39p+3, -0x1.5953119087b48p-2,
                    0x1.3e1d802736a9fp-3, -0x1.96c8b87d5d852p-5, -0x1.afc5703397c80p-4) &&
                blocked(150, 0x1.f3636ad3feeb3p+4, -0x1.ee9d1f4a3459ap+4, -0x1.5b320fd572fb6p-3,
                        0x1.6480303056457p-4, -0x1.ea987bf976a7fp-5, 0x1.6b91f3cec6e2cp-4),
            "DSYEVR translation's blocked DSYTRD/DORMQR paths reproduce StataBE's symeigensystem() bits (orders 40, 150)");
    // The same matrix times 1e160 at order 72: DSYEVR rescales it, DSTEMR
    // fails (INFO=22, in StataBE's liblapack as here) and DSYEVR falls back to
    // DSTEBZ + DSTEIN; StataBE 19.5's Mata symeigensystem() bits (captured
    // 2026-10-27, scratch-r150/dsyevr/stata_probe/eigfb.do).
    w2check(blocked(72, 0x1.5de990252f051p+535, -0x1.630d74cd84d59p+535, -0x1.6854b6149115bp-3,
                    0x1.89290b204c299p-3, -0x1.f0be3beb63023p-5, 0x1.0ba7d6eb38041p-3, 1e160),
            "DSYEVR translation's DSTEBZ/DSTEIN fallback reproduces StataBE's symeigensystem() bits (order 72, scale 1e160)");
  }
  // lf_052's `difficult` fit (w2c_ml_getmata.do:170-178): with the DSYEVR
  // eigenvectors in the difficult step, ml reaches StataBE 19.5's converged
  // e(b) bit for bit in Stata's 6 iterations (moptimize_result_coefs %21x,
  // captured 2026-10-27, scratch-r150/ml_tail/stata_probe/traj.log Q6/Q7).
  bool shape = true;
  for (const char* c : {"clear all","set obs 200","generate x1  = mod(_n*7,11)/5 + mod(_n,5)/10",
        "generate x2  = mod(_n*3,5)/2 + mod(_n*13,7)/10",
        "generate yb  = invlogit(-0.3 + 0.9*x1) + mod(_n*11,17)/170 - 0.05"})
    shape = run(s,c) == "0" && shape;
  s.programs["lf_052"] = {"lf_052",false,{
    "args lnf a0 a1 lnsigma",
    "local inv_c = \"ln( exp(x2/x1) + exp(mc))\"",
    "local gamma = \"(invlogit(`a0' + ln(1+exp(`a1'))*x1))\"",
    "local gamma_prime = \"(ln(1+exp(`a1'))*(`gamma')*(1-`gamma'))\"",
    "quietly replace `lnf' = -0.5*ln(2*_pi) - 0.5*($ML_y1 - `gamma' - `gamma_prime')^2/(exp(`lnsigma'))^2 - `lnsigma'"}};
  w2check(shape && run(s,"ml model lf lf_052 (a0: yb =) (a1:) (lnsigma:), technique(nr 30 bhhh 10) vce(oim)")=="0" &&
          run(s,"set seed 20260924")=="0" && run(s,"ml search, repeat(50)")=="0" &&
          run(s,"ml maximize, difficult iter(3000)")=="0" && scalar(s,"ic")==6 &&
          coef(s,0)==-0x1.fa64e92d4eca0p-1 && coef(s,1)==0x1.54aa2a94d324dp-3 && coef(s,2)==-0x1.c651f06eae1d2p+1,
          "ml lf_052 difficult fit reproduces StataBE's e(b) bits and e(ic)");
}
