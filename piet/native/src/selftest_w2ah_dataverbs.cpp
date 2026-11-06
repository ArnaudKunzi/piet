// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data-verbs-effects-rename-recode-label (2026-10-21) -- the renaming,
// recoding, labelling and de-duplicating data verbs as StataBE 19.5 leaves them.
//
// Oracle: internal notes w2ah_data_verbs (private coordination tree; landing as
// tests/eval/golden/aer_w2/w2ah_data_verbs.{do,log}). Each section below is the
// oracle .do's own section, verbatim, run in a fresh session; each arm compares
// the PREC record PIET prints with the oracle's record (cited by log line). The
// arms are exactly the 52 records frozen main 8b4fda1e got wrong (internal
// note w2ah-arms.md, RED rows), so every one fails
// on main: FU-243's implicit-float carry through clonevar and rename (BUG-964),
// `: label lblname #' (FU-1111), recode's rule grammar and gen() (BUG-965),
// encode/decode/isid/clonevar refusals and labels (BUG-966) and duplicates'
// r() and force rules (BUG-967). The FU-243 equalities are also asserted
// directly: a replace on the clone or the renamed variable stores what an
// untyped generate stores.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gAhChecks = 0;
void ahCheck(bool cond, const std::string& label) {
  ++gAhChecks;
  w2check(cond, label);
}

struct AhSection {
  const char* id;
  const char* src;
};

struct AhArm {
  const char* section;
  const char* arm;
  int logLine;
  const char* oracle;
};

// (a) rename and ren: names, position, type, format, labels kept; rc for an existing or absent name
const char* const kAhSection_a = R"PIETDO(set linesize 255
clear
quietly set obs 4
generate byte id = _n
generate int a = 10*_n
label variable a "A label"
label define al 10 "ten" 20 "twenty"
label values a al
format a %6.0f
generate double d = _n/3
generate str4 s = "s" + string(_n)
label variable s "S label"
generate b = _n/7
capture noisily rename a cpf
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cpf
local __m2 "absent"
capture local __m2 : format cpf
local __m3 "absent"
capture local __m3 : variable label cpf
local __m4 "absent"
capture local __m4 : value label cpf
local __m5 "absent"
capture local __m5 : label al 10
local __m6 "absent"
capture local __m6 : label al 20
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_lab|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__m6']|" "[`__ds']|" %21x cpf[1] "|" %21x cpf[2] "|" %21x cpf[_N] "|" %21x _N "|"
if _rc != 0 display "PRECFAIL|ah_rename_lab|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename s s_new
local __rc = _rc
local __m1 "absent"
capture local __m1 : type s_new
local __m2 "absent"
capture local __m2 : format s_new
local __m3 "absent"
capture local __m3 : variable label s_new
local __m4 "absent"
capture local __m4 : value label s_new
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_str|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__ds']|" "[" s_new[1] "]|" "[" s_new[2] "]|" "[" s_new[_N] "]|"
if _rc != 0 display "PRECFAIL|ah_rename_str|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename b bb
local __rc = _rc
local __m1 "absent"
capture local __m1 : type bb
local __m2 "absent"
capture local __m2 : format bb
local __m3 "absent"
capture local __m3 : variable label bb
local __m4 "absent"
capture local __m4 : value label bb
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_float|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__ds']|" %21x bb[1] "|" %21x bb[2] "|" %21x bb[_N] "|"
if _rc != 0 display "PRECFAIL|ah_rename_float|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename id ID
local __rc = _rc
local __m1 "absent"
capture local __m1 : type ID
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_case|" `__rc' "|" "`__m1'|" "[`__ds']|" %21x ID[1] "|" %21x ID[_N] "|"
if _rc != 0 display "PRECFAIL|ah_rename_case|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename cpf d
local __rc = _rc
local __m1 "absent"
capture local __m1 : type d
local __m2 "absent"
capture local __m2 : type cpf
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_exist|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__ds']|" %21x cpf[1] "|" %21x d[1] "|"
if _rc != 0 display "PRECFAIL|ah_rename_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename zz q
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_rename_absent|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_rename_absent|rc=`__rc'|display_rc=" _rc "|"
capture noisily ren cpf a2
local __rc = _rc
local __m1 "absent"
capture local __m1 : type a2
local __m2 "absent"
capture local __m2 : format a2
local __m3 "absent"
capture local __m3 : variable label a2
local __m4 "absent"
capture local __m4 : value label a2
local __m5 "absent"
capture local __m5 : label al 10
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_ren_lab|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__ds']|" %21x a2[1] "|" %21x a2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_ren_lab|rc=`__rc'|display_rc=" _rc "|"
global ind "d"
capture noisily ren ${ind} ${ind}_i
local __rc = _rc
local __m1 "absent"
capture local __m1 : type d_i
local __m2 "absent"
capture local __m2 : format d_i
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_ren_global|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__ds']|" %21x d_i[1] "|" %21x d_i[_N] "|"
if _rc != 0 display "PRECFAIL|ah_ren_global|rc=`__rc'|display_rc=" _rc "|"
capture noisily ren a2 bb
local __rc = _rc
local __m1 "absent"
capture local __m1 : type a2
local __m2 "absent"
capture local __m2 : type bb
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_ren_exist|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_ren_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily ren nosuch q2
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_ren_absent|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_ren_absent|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (b) FU-243: the implicit float of an untyped generate survives clonevar and rename
const char* const kAhSection_b = R"PIETDO(set linesize 255
clear
quietly set obs 3
generate x = 1/7
generate x2 = 1/7
generate double xd = 1/7
capture noisily clonevar y = x
local __rc = _rc
capture noisily replace y = 2/7
local __m1 "absent"
capture local __m1 : type y
local __m2 "absent"
capture local __m2 : format y
capture noisily display "PREC|ah_fu243_clone|" `__rc' "|" "`__m1'|" "`__m2'|" %21x y[1] "|" %21x y[2] "|" %21x y[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_clone|rc=`__rc'|display_rc=" _rc "|"
capture noisily rename x w
local __rc = _rc
capture noisily replace w = 2/7
local __m1 "absent"
capture local __m1 : type w
local __m2 "absent"
capture local __m2 : format w
capture noisily display "PREC|ah_fu243_rename|" `__rc' "|" "`__m1'|" "`__m2'|" %21x w[1] "|" %21x w[2] "|" %21x w[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_rename|rc=`__rc'|display_rc=" _rc "|"
capture noisily generate z = 2/7
local __rc = _rc
local __m1 "absent"
capture local __m1 : type z
local __m2 "absent"
capture local __m2 : format z
capture noisily display "PREC|ah_fu243_twin|" `__rc' "|" "`__m1'|" "`__m2'|" %21x z[1] "|" %21x z[2] "|" %21x z[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_twin|rc=`__rc'|display_rc=" _rc "|"
capture noisily ren x2 w2
local __rc = _rc
capture noisily replace w2 = 3/7
local __m1 "absent"
capture local __m1 : type w2
local __m2 "absent"
capture local __m2 : format w2
capture noisily display "PREC|ah_fu243_ren|" `__rc' "|" "`__m1'|" "`__m2'|" %21x w2[1] "|" %21x w2[2] "|" %21x w2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_ren|rc=`__rc'|display_rc=" _rc "|"
capture noisily generate z3 = 3/7
local __rc = _rc
local __m1 "absent"
capture local __m1 : type z3
local __m2 "absent"
capture local __m2 : format z3
capture noisily display "PREC|ah_fu243_twin3|" `__rc' "|" "`__m1'|" "`__m2'|" %21x z3[1] "|" %21x z3[2] "|" %21x z3[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_twin3|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar y2 = w
local __rc = _rc
capture noisily replace y2 = 16777217
local __m1 "absent"
capture local __m1 : type y2
local __m2 "absent"
capture local __m2 : format y2
capture noisily display "PREC|ah_fu243_bigint|" `__rc' "|" "`__m1'|" "`__m2'|" %21x y2[1] "|" %21x y2[2] "|" %21x y2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_bigint|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar yd = xd
local __rc = _rc
capture noisily replace yd = 2/7
local __m1 "absent"
capture local __m1 : type yd
local __m2 "absent"
capture local __m2 : format yd
capture noisily display "PREC|ah_fu243_clone_dbl|" `__rc' "|" "`__m1'|" "`__m2'|" %21x yd[1] "|" %21x yd[2] "|" %21x yd[_N] "|"
if _rc != 0 display "PRECFAIL|ah_fu243_clone_dbl|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (e) recode: rule grammar in place, gen()/generate()/g(), rule labels, refusals
const char* const kAhSection_e = R"PIETDO(set linesize 255
clear
quietly set obs 8
generate byte r0 = cond(_n < 8, _n, .)
label variable r0 "raw code"
generate byte age = 10 + 5*_n
generate double dd = _n/4
generate str2 rs = "a"
generate byte r1 = r0
capture noisily recode r1 (1=10) (2=20)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r1
capture noisily display "PREC|ah_recode_single|" `__rc' "|" "`__m1'|" %21x r1[1] "|" %21x r1[2] "|" %21x r1[3] "|" %21x r1[4] "|" %21x r1[5] "|" %21x r1[6] "|" %21x r1[7] "|" %21x r1[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_single|rc=`__rc'|display_rc=" _rc "|"
generate byte r2 = r0
capture noisily recode r2 (1/3=1) (4/6=2)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r2
capture noisily display "PREC|ah_recode_range|" `__rc' "|" "`__m1'|" %21x r2[1] "|" %21x r2[2] "|" %21x r2[3] "|" %21x r2[4] "|" %21x r2[5] "|" %21x r2[6] "|" %21x r2[7] "|" %21x r2[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_range|rc=`__rc'|display_rc=" _rc "|"
generate byte r3 = r0
capture noisily recode r3 (1 2 3=1) (4 5=2)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r3
capture noisily display "PREC|ah_recode_numlist|" `__rc' "|" "`__m1'|" %21x r3[1] "|" %21x r3[2] "|" %21x r3[3] "|" %21x r3[4] "|" %21x r3[5] "|" %21x r3[6] "|" %21x r3[7] "|" %21x r3[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_numlist|rc=`__rc'|display_rc=" _rc "|"
generate byte r4 = r0
capture noisily recode r4 (1/3=1) (else=0)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r4
capture noisily display "PREC|ah_recode_else|" `__rc' "|" "`__m1'|" %21x r4[1] "|" %21x r4[2] "|" %21x r4[3] "|" %21x r4[4] "|" %21x r4[5] "|" %21x r4[6] "|" %21x r4[7] "|" %21x r4[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_else|rc=`__rc'|display_rc=" _rc "|"
generate byte r5 = r0
capture noisily recode r5 (missing=0)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r5
capture noisily display "PREC|ah_recode_missing|" `__rc' "|" "`__m1'|" %21x r5[1] "|" %21x r5[2] "|" %21x r5[3] "|" %21x r5[4] "|" %21x r5[5] "|" %21x r5[6] "|" %21x r5[7] "|" %21x r5[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_missing|rc=`__rc'|display_rc=" _rc "|"
generate byte r6 = r0
capture noisily recode r6 (1=2) (2=1) (.=9)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r6
capture noisily display "PREC|ah_recode_dotrule|" `__rc' "|" "`__m1'|" %21x r6[1] "|" %21x r6[2] "|" %21x r6[3] "|" %21x r6[4] "|" %21x r6[5] "|" %21x r6[6] "|" %21x r6[7] "|" %21x r6[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_dotrule|rc=`__rc'|display_rc=" _rc "|"
generate byte r7 = r0
capture noisily recode r7 (1 = 1) (2 = 2) (nonmissing = .)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r7
capture noisily display "PREC|ah_recode_nonmissing|" `__rc' "|" "`__m1'|" %21x r7[1] "|" %21x r7[2] "|" %21x r7[3] "|" %21x r7[4] "|" %21x r7[5] "|" %21x r7[6] "|" %21x r7[7] "|" %21x r7[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_nonmissing|rc=`__rc'|display_rc=" _rc "|"
generate byte r8 = r0
capture noisily recode r8 (min/2=1) (6/max=3)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r8
capture noisily display "PREC|ah_recode_minmax|" `__rc' "|" "`__m1'|" %21x r8[1] "|" %21x r8[2] "|" %21x r8[3] "|" %21x r8[4] "|" %21x r8[5] "|" %21x r8[6] "|" %21x r8[7] "|" %21x r8[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_minmax|rc=`__rc'|display_rc=" _rc "|"
generate byte r9 = r0
capture noisily recode r9 (1/4=1) (3/6=2)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r9
capture noisily display "PREC|ah_recode_overlap|" `__rc' "|" "`__m1'|" %21x r9[1] "|" %21x r9[2] "|" %21x r9[3] "|" %21x r9[4] "|" %21x r9[5] "|" %21x r9[6] "|" %21x r9[7] "|" %21x r9[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_overlap|rc=`__rc'|display_rc=" _rc "|"
generate byte r10 = r0
capture noisily recode r10 (1=1000) (2=0.5)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r10
capture noisily display "PREC|ah_recode_promote|" `__rc' "|" "`__m1'|" %21x r10[1] "|" %21x r10[2] "|" %21x r10[3] "|" %21x r10[4] "|" %21x r10[5] "|" %21x r10[6] "|" %21x r10[7] "|" %21x r10[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_promote|rc=`__rc'|display_rc=" _rc "|"
generate byte m1 = r0
generate double m2 = r0/2
capture noisily recode m1 m2 (.=0)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type m1
local __m2 "absent"
capture local __m2 : type m2
capture noisily display "PREC|ah_recode_multi|" `__rc' "|" "`__m1'|" "`__m2'|" %21x m1[7] "|" %21x m1[8] "|" %21x m2[7] "|" %21x m2[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_multi|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode r0 (1=0) (8=1) (7=1), gen(newunit)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type newunit
local __m2 "absent"
capture local __m2 : format newunit
local __m3 "absent"
capture local __m3 : variable label newunit
local __m4 "absent"
capture local __m4 : value label newunit
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_recode_gen|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__ds']|" %21x newunit[1] "|" %21x newunit[2] "|" %21x newunit[7] "|" %21x newunit[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_gen|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode age (-1/15=1) (16/30=2) (31/1000=3), gen(agebin)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type agebin
local __m2 "absent"
capture local __m2 : format agebin
local __m3 "absent"
capture local __m3 : variable label agebin
local __m4 "absent"
capture local __m4 : value label agebin
capture noisily display "PREC|ah_recode_gen_range|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x agebin[1] "|" %21x agebin[2] "|" %21x agebin[5] "|" %21x agebin[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_gen_range|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode r0 (1 2 3=1) (4 5=2) (nonmissing=.), generate(edu_cat)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type edu_cat
local __m2 "absent"
capture local __m2 : format edu_cat
local __m3 "absent"
capture local __m3 : variable label edu_cat
local __m4 "absent"
capture local __m4 : value label edu_cat
capture noisily display "PREC|ah_recode_generate|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x edu_cat[1] "|" %21x edu_cat[4] "|" %21x edu_cat[6] "|" %21x edu_cat[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_generate|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode age (min/17=1 "<18 years")(18/24=2 "18-24 years")(25/34=3 "25-34 years")(35/40=4 "35 and older"), g(agegroup)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type agegroup
local __m2 "absent"
capture local __m2 : format agegroup
local __m3 "absent"
capture local __m3 : variable label agegroup
local __m4 "absent"
capture local __m4 : value label agegroup
local __m5 "absent"
capture local __m5 : label agegroup 1
local __m6 "absent"
capture local __m6 : label agegroup 4
capture noisily display "PREC|ah_recode_g_lab|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__m6']|" %21x agegroup[1] "|" %21x agegroup[2] "|" %21x agegroup[3] "|" %21x agegroup[5] "|" %21x agegroup[7] "|"
if _rc != 0 display "PRECFAIL|ah_recode_g_lab|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode dd (0/1=0) (1/2=1), gen(ddc)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type ddc
local __m2 "absent"
capture local __m2 : format ddc
capture noisily display "PREC|ah_recode_gen_double|" `__rc' "|" "`__m1'|" "`__m2'|" %21x ddc[3] "|" %21x ddc[4] "|" %21x ddc[5] "|" %21x ddc[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_gen_double|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode r0 (1=2), gen(age)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type age
capture noisily display "PREC|ah_recode_gen_exist|" `__rc' "|" "`__m1'|" %21x age[1] "|"
if _rc != 0 display "PRECFAIL|ah_recode_gen_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode rs (1=2)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type rs
capture noisily display "PREC|ah_recode_str|" `__rc' "|" "`__m1'|" "[" rs[1] "]|"
if _rc != 0 display "PRECFAIL|ah_recode_str|rc=`__rc'|display_rc=" _rc "|"
capture noisily recode r0 (1=2) (else=0) (missing=9)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type r0
capture noisily display "PREC|ah_recode_else_missing|" `__rc' "|" "`__m1'|" %21x r0[1] "|" %21x r0[8] "|"
if _rc != 0 display "PRECFAIL|ah_recode_else_missing|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (f) decode, gen(): label text, empty string for an unlabelled value, str# and format
const char* const kAhSection_f = R"PIETDO(set linesize 255
clear
quietly set obs 5
generate byte region = cond(_n <= 4, mod(_n - 1, 3) + 1, .)
label define rl 1 "North" 2 "South-East region"
label values region rl
label variable region "Region code"
generate byte nolab = _n
generate byte sh = mod(_n, 2) + 1
label define shl 1 "a" 2 "bb"
label values sh shl
capture noisily decode region, gen(region_str)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type region_str
local __m2 "absent"
capture local __m2 : format region_str
local __m3 "absent"
capture local __m3 : variable label region_str
local __m4 "absent"
capture local __m4 : value label region_str
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_decode_gen|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__ds']|" "[" region_str[1] "]|" "[" region_str[2] "]|" "[" region_str[3] "]|" "[" region_str[4] "]|" "[" region_str[5] "]|"
if _rc != 0 display "PRECFAIL|ah_decode_gen|rc=`__rc'|display_rc=" _rc "|"
capture noisily decode sh, gen(sh_s)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type sh_s
local __m2 "absent"
capture local __m2 : format sh_s
local __m3 "absent"
capture local __m3 : variable label sh_s
local __m4 "absent"
capture local __m4 : value label sh_s
capture noisily display "PREC|ah_decode_short|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[" sh_s[1] "]|" "[" sh_s[2] "]|"
if _rc != 0 display "PRECFAIL|ah_decode_short|rc=`__rc'|display_rc=" _rc "|"
capture noisily decode nolab, gen(nl_str)
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_decode_nolab|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_decode_nolab|rc=`__rc'|display_rc=" _rc "|"
capture noisily decode region, gen(nolab)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type nolab
capture noisily display "PREC|ah_decode_exist|" `__rc' "|" "`__m1'|" %21x nolab[1] "|"
if _rc != 0 display "PRECFAIL|ah_decode_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily decode region_str, gen(q)
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_decode_string|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_decode_string|rc=`__rc'|display_rc=" _rc "|"
capture noisily decode region
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_decode_nogen|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_decode_nogen|rc=`__rc'|display_rc=" _rc "|"
tempvar t
capture noisily decode region, gen(`t')
local __rc = _rc
local __m1 "absent"
capture local __m1 : type `t'
local __m2 "absent"
capture local __m2 : format `t'
capture noisily display "PREC|ah_decode_tempvar|" `__rc' "|" "`__m1'|" "`__m2'|" "[" `t'[1] "]|" "[" `t'[3] "]|"
if _rc != 0 display "PRECFAIL|ah_decode_tempvar|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (g) encode gen()/g()/generate()/label(): sorted codes, blanks to missing, label reuse
const char* const kAhSection_g = R"PIETDO(set linesize 255
clear
quietly set obs 6
generate str8 es = ""
quietly replace es = "pear" in 1
quietly replace es = "apple" in 2
quietly replace es = "fig" in 4
quietly replace es = "apple" in 5
quietly replace es = "Pear" in 6
label variable es "Fruit name"
generate byte id = _n
generate str3 eb = ""
generate str3 ew = cond(_n == 1, "  ", "x")
capture noisily encode es, gen(e_es)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type e_es
local __m2 "absent"
capture local __m2 : format e_es
local __m3 "absent"
capture local __m3 : variable label e_es
local __m4 "absent"
capture local __m4 : value label e_es
local __m5 "absent"
capture local __m5 : label e_es 1
local __m6 "absent"
capture local __m6 : label e_es 2
local __m7 "absent"
capture local __m7 : label e_es 3
local __m8 "absent"
capture local __m8 : label e_es 4
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_encode_gen|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__m6']|" "[`__m7']|" "[`__m8']|" "[`__ds']|" %21x e_es[1] "|" %21x e_es[3] "|" %21x e_es[5] "|" %21x e_es[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_gen|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es, g(sector)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type sector
local __m2 "absent"
capture local __m2 : format sector
local __m3 "absent"
capture local __m3 : value label sector
local __m4 "absent"
capture local __m4 : label sector 1
capture noisily display "PREC|ah_encode_g|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x sector[1] "|" %21x sector[3] "|" %21x sector[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_g|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es, generate(occ)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type occ
local __m2 "absent"
capture local __m2 : format occ
local __m3 "absent"
capture local __m3 : value label occ
local __m4 "absent"
capture local __m4 : label occ 4
capture noisily display "PREC|ah_encode_generate|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x occ[1] "|" %21x occ[3] "|" %21x occ[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_generate|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es, g(newcode) label(intervention)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type newcode
local __m2 "absent"
capture local __m2 : value label newcode
local __m3 "absent"
capture local __m3 : label intervention 1
local __m4 "absent"
capture local __m4 : label intervention 4
capture noisily display "PREC|ah_encode_label_new|" `__rc' "|" "`__m1'|" "[`__m2']|" "[`__m3']|" "[`__m4']|" %21x newcode[1] "|" %21x newcode[2] "|" %21x newcode[3] "|" %21x newcode[4] "|" %21x newcode[5] "|" %21x newcode[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_label_new|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es, gen(nc2) label(intervention)
local __rc = _rc
local __m1 "absent"
capture local __m1 : value label nc2
local __m2 "absent"
capture local __m2 : label intervention 2
capture noisily display "PREC|ah_encode_label_again|" `__rc' "|" "[`__m1']|" "[`__m2']|" %21x nc2[1] "|" %21x nc2[2] "|" %21x nc2[3] "|" %21x nc2[4] "|" %21x nc2[5] "|" %21x nc2[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_label_again|rc=`__rc'|display_rc=" _rc "|"
label define ordl 1 "pear" 5 "kiwi"
capture noisily encode es, gen(es_bin) label(ordl)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type es_bin
local __m2 "absent"
capture local __m2 : value label es_bin
local __m3 "absent"
capture local __m3 : label ordl 1
local __m4 "absent"
capture local __m4 : label ordl 5
local __m5 "absent"
capture local __m5 : label ordl 6
local __m6 "absent"
capture local __m6 : label ordl 7
local __m7 "absent"
capture local __m7 : label ordl 8
capture noisily display "PREC|ah_encode_label_exist|" `__rc' "|" "`__m1'|" "[`__m2']|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__m6']|" "[`__m7']|" %21x es_bin[1] "|" %21x es_bin[2] "|" %21x es_bin[3] "|" %21x es_bin[4] "|" %21x es_bin[6] "|"
if _rc != 0 display "PRECFAIL|ah_encode_label_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode eb, gen(eb_c)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type eb_c
local __m2 "absent"
capture local __m2 : value label eb_c
local __m3 "absent"
capture local __m3 : display %21x eb_c[1]
local __m4 "absent"
capture local __m4 : display %21x eb_c[_N]
capture noisily display "PREC|ah_encode_blank_all|" `__rc' "|" "`__m1'|" "[`__m2']|" "`__m3'|" "`__m4'|"
if _rc != 0 display "PRECFAIL|ah_encode_blank_all|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode ew, gen(ew_c)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type ew_c
local __m2 "absent"
capture local __m2 : label ew_c 1
local __m3 "absent"
capture local __m3 : label ew_c 2
local __m4 "absent"
capture local __m4 : display %21x ew_c[1]
local __m5 "absent"
capture local __m5 : display %21x ew_c[2]
capture noisily display "PREC|ah_encode_spaces|" `__rc' "|" "`__m1'|" "[`__m2']|" "[`__m3']|" "`__m4'|" "`__m5'|"
if _rc != 0 display "PRECFAIL|ah_encode_spaces|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es, gen(e_es)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type e_es
capture noisily display "PREC|ah_encode_exist|" `__rc' "|" "`__m1'|" %21x e_es[1] "|"
if _rc != 0 display "PRECFAIL|ah_encode_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode id, gen(idc)
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_encode_numeric|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_encode_numeric|rc=`__rc'|display_rc=" _rc "|"
capture noisily encode es
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_encode_nogen|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_encode_nogen|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (h) isid: rc 0 on a key (order kept), Stata rc on duplicates and missing keys
const char* const kAhSection_h = R"PIETDO(set linesize 255
clear
quietly set obs 6
generate byte k1 = ceil(_n/2)
generate byte k2 = mod(_n, 2)
generate byte u = 7 - _n
generate byte km = cond(_n == 3, ., _n)
generate str2 ks = "a" + string(_n)
capture noisily isid u
local __rc = _rc
capture noisily display "PREC|ah_isid_one|" `__rc' "|" %21x u[1] "|" %21x u[_N] "|" %21x _N "|"
if _rc != 0 display "PRECFAIL|ah_isid_one|rc=`__rc'|display_rc=" _rc "|"
capture noisily isid k1 k2
local __rc = _rc
capture noisily display "PREC|ah_isid_two|" `__rc' "|" %21x k1[1] "|" %21x k1[_N] "|" %21x k2[1] "|" %21x k2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_isid_two|rc=`__rc'|display_rc=" _rc "|"
capture noisily isid ks
local __rc = _rc
capture noisily display "PREC|ah_isid_str|" `__rc' "|" "[" ks[1] "]|" "[" ks[_N] "]|"
if _rc != 0 display "PRECFAIL|ah_isid_str|rc=`__rc'|display_rc=" _rc "|"
capture noisily isid k1
local __rc = _rc
capture noisily display "PREC|ah_isid_dup|" `__rc' "|" %21x k1[1] "|" %21x k1[_N] "|"
if _rc != 0 display "PRECFAIL|ah_isid_dup|rc=`__rc'|display_rc=" _rc "|"
capture noisily isid km
local __rc = _rc
capture noisily display "PREC|ah_isid_missing|" `__rc' "|" %21x km[1] "|" %21x km[3] "|"
if _rc != 0 display "PRECFAIL|ah_isid_missing|rc=`__rc'|display_rc=" _rc "|"
capture noisily isid nosuch
local __rc = _rc
capture noisily display "PREC|ah_isid_absent|" `__rc' "|" %21x u[1] "|"
if _rc != 0 display "PRECFAIL|ah_isid_absent|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (i) duplicates report/list/tag/drop, with and without force (fixture rebuilt per arm)
const char* const kAhSection_i = R"PIETDO(set linesize 255
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates report
local __rc = _rc
capture noisily display "PREC|ah_dup_report_all|" `__rc' "|" "N=" %21x r(N) "|" "unique_value=" %21x r(unique_value) "|" %21x _N "|"
if _rc != 0 display "PRECFAIL|ah_dup_report_all|rc=`__rc'|display_rc=" _rc "|"
capture noisily duplicates report pis
local __rc = _rc
capture noisily display "PREC|ah_dup_report_var|" `__rc' "|" "N=" %21x r(N) "|" "unique_value=" %21x r(unique_value) "|" %21x _N "|" %21x v[1] "|" %21x v[_N] "|"
if _rc != 0 display "PRECFAIL|ah_dup_report_var|rc=`__rc'|display_rc=" _rc "|"
capture noisily duplicates report pis yr
local __rc = _rc
capture noisily display "PREC|ah_dup_report_two|" `__rc' "|" "N=" %21x r(N) "|" "unique_value=" %21x r(unique_value) "|"
if _rc != 0 display "PRECFAIL|ah_dup_report_two|rc=`__rc'|display_rc=" _rc "|"
capture noisily duplicates list pis yr
local __rc = _rc
capture noisily display "PREC|ah_dup_list|" `__rc' "|" "N=" %21x r(N) "|" %21x _N "|" %21x v[1] "|" %21x v[4] "|" %21x v[_N] "|"
if _rc != 0 display "PRECFAIL|ah_dup_list|rc=`__rc'|display_rc=" _rc "|"
capture noisily duplicates list
local __rc = _rc
capture noisily display "PREC|ah_dup_list_all|" `__rc' "|" "N=" %21x r(N) "|" %21x _N "|" %21x v[1] "|" %21x v[_N] "|"
if _rc != 0 display "PRECFAIL|ah_dup_list_all|rc=`__rc'|display_rc=" _rc "|"
capture noisily duplicates tag pis, gen(dup)
local __rc = _rc
local __m1 "absent"
capture local __m1 : type dup
local __m2 "absent"
capture local __m2 : format dup
capture noisily display "PREC|ah_dup_tag|" `__rc' "|" "N=" %21x r(N) "|" "`__m1'|" "`__m2'|" %21x dup[1] "|" %21x dup[2] "|" %21x dup[3] "|" %21x dup[4] "|" %21x dup[5] "|" %21x dup[6] "|" %21x dup[7] "|" %21x dup[8] "|"
if _rc != 0 display "PRECFAIL|ah_dup_tag|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates drop
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_all|" `__rc' "|" "N=" %21x r(N) "|" "N_drop=" %21x r(N_drop) "|" %21x _N "|" %21x v[1] "|" %21x v[2] "|" %21x v[_N] "|" %21x pis[2] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_all|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates drop pis, force
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_force|" `__rc' "|" "N=" %21x r(N) "|" "N_drop=" %21x r(N_drop) "|" %21x _N "|" %21x pis[1] "|" %21x pis[2] "|" %21x pis[3] "|" %21x pis[4] "|" %21x pis[5] "|" %21x v[1] "|" %21x v[2] "|" %21x v[3] "|" %21x v[4] "|" %21x v[5] "|" %21x yr[3] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_force|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates drop pis yr, force
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_force_two|" `__rc' "|" "N=" %21x r(N) "|" "N_drop=" %21x r(N_drop) "|" %21x _N "|" %21x v[1] "|" %21x v[2] "|" %21x v[3] "|" %21x v[4] "|" %21x v[5] "|" %21x v[6] "|" %21x yr[4] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_force_two|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates drop pis
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_noforce|" `__rc' "|" %21x _N "|" %21x v[2] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_noforce|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
capture noisily duplicates drop, force
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_all_force|" `__rc' "|" %21x _N "|" %21x v[2] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_all_force|rc=`__rc'|display_rc=" _rc "|"
clear
quietly set obs 8
generate byte pis = real(word("1 1 2 3 3 3 4 5", _n))
generate byte yr = real(word("1 1 1 1 2 1 1 1", _n))
generate byte v = real(word("9 9 8 7 7 6 5 4", _n))
drop in 2
capture noisily duplicates drop
local __rc = _rc
capture noisily display "PREC|ah_dup_drop_none|" `__rc' "|" "N=" %21x r(N) "|" "N_drop=" %21x r(N_drop) "|" %21x _N "|" %21x v[1] "|" %21x v[_N] "|"
if _rc != 0 display "PRECFAIL|ah_dup_drop_none|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

// (j) clonevar: type, format, variable label, value label, if-qualifier, refusals
const char* const kAhSection_j = R"PIETDO(set linesize 255
clear
quietly set obs 4
generate byte cb = mod(_n, 3)
label define cbl 0 "zero" 1 "one" 2 "two"
label values cb cbl
label variable cb "Byte src"
format cb %3.0f
generate int ci = 300*_n
label variable ci "Int src"
format ci %7.0fc
generate long cl = 100000*_n
generate float cf = _n/3
format cf %6.2f
generate double cdb = _n/3
format cdb %10.4f
label variable cdb "Double src"
generate str12 Year = "y" + string(_n)
label variable Year "Str src"
capture noisily clonevar county_OG = cb
local __rc = _rc
local __m1 "absent"
capture local __m1 : type county_OG
local __m2 "absent"
capture local __m2 : format county_OG
local __m3 "absent"
capture local __m3 : variable label county_OG
local __m4 "absent"
capture local __m4 : value label county_OG
local __m5 "absent"
capture local __m5 : label cbl 1
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_clone_byte|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__m5']|" "[`__ds']|" %21x county_OG[1] "|" %21x county_OG[2] "|" %21x county_OG[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_byte|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar ci2 = ci
local __rc = _rc
local __m1 "absent"
capture local __m1 : type ci2
local __m2 "absent"
capture local __m2 : format ci2
local __m3 "absent"
capture local __m3 : variable label ci2
local __m4 "absent"
capture local __m4 : value label ci2
capture noisily display "PREC|ah_clone_int|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x ci2[1] "|" %21x ci2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_int|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar cl2 = cl
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cl2
local __m2 "absent"
capture local __m2 : format cl2
local __m3 "absent"
capture local __m3 : variable label cl2
local __m4 "absent"
capture local __m4 : value label cl2
capture noisily display "PREC|ah_clone_long|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x cl2[1] "|" %21x cl2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_long|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar cf2 = cf
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cf2
local __m2 "absent"
capture local __m2 : format cf2
local __m3 "absent"
capture local __m3 : variable label cf2
local __m4 "absent"
capture local __m4 : value label cf2
capture noisily display "PREC|ah_clone_float|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x cf2[1] "|" %21x cf2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_float|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar cd2 = cdb
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cd2
local __m2 "absent"
capture local __m2 : format cd2
local __m3 "absent"
capture local __m3 : variable label cd2
local __m4 "absent"
capture local __m4 : value label cd2
capture noisily display "PREC|ah_clone_double|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x cd2[1] "|" %21x cd2[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_double|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar year = Year
local __rc = _rc
local __m1 "absent"
capture local __m1 : type year
local __m2 "absent"
capture local __m2 : format year
local __m3 "absent"
capture local __m3 : variable label year
local __m4 "absent"
capture local __m4 : value label year
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_clone_str|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" "[`__ds']|" "[" year[1] "]|" "[" year[_N] "]|"
if _rc != 0 display "PRECFAIL|ah_clone_str|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar cif = cdb if cb == 1
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cif
local __m2 "absent"
capture local __m2 : format cif
local __m3 "absent"
capture local __m3 : variable label cif
local __m4 "absent"
capture local __m4 : value label cif
capture noisily display "PREC|ah_clone_if|" `__rc' "|" "`__m1'|" "`__m2'|" "[`__m3']|" "[`__m4']|" %21x cif[1] "|" %21x cif[2] "|" %21x cif[3] "|" %21x cif[_N] "|"
if _rc != 0 display "PRECFAIL|ah_clone_if|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar sif = Year if cb == 1
local __rc = _rc
local __m1 "absent"
capture local __m1 : type sif
local __m2 "absent"
capture local __m2 : format sif
capture noisily display "PREC|ah_clone_str_if|" `__rc' "|" "`__m1'|" "`__m2'|" "[" sif[1] "]|" "[" sif[2] "]|"
if _rc != 0 display "PRECFAIL|ah_clone_str_if|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar cb = ci
local __rc = _rc
local __m1 "absent"
capture local __m1 : type cb
capture noisily display "PREC|ah_clone_exist|" `__rc' "|" "`__m1'|" %21x cb[1] "|"
if _rc != 0 display "PRECFAIL|ah_clone_exist|rc=`__rc'|display_rc=" _rc "|"
capture noisily clonevar q = nosuch
local __rc = _rc
local __ds "absent"
capture quietly ds
if _rc == 0 local __ds "`r(varlist)'"
capture noisily display "PREC|ah_clone_absent|" `__rc' "|" "[`__ds']|"
if _rc != 0 display "PRECFAIL|ah_clone_absent|rc=`__rc'|display_rc=" _rc "|"
)PIETDO";

const AhSection kAhSections[] = {
    {"a", kAhSection_a},
    {"b", kAhSection_b},
    {"e", kAhSection_e},
    {"f", kAhSection_f},
    {"g", kAhSection_g},
    {"h", kAhSection_h},
    {"i", kAhSection_i},
    {"j", kAhSection_j},
};

const AhArm kAhArms[] = {
    {"a", "ah_rename_lab", 101,
     "PREC|ah_rename_lab|0|int|%6.0f|[A label]|[al]|[ten]|[twenty]|[id cpf d s b]|+1.4000000000000X+003|+1.4000000000000X+004|+1.4000000000000X+005|+1.0000000000000X+002|"},
    {"a", "ah_rename_exist", 206,
     "PREC|ah_rename_exist|110|double|int|[ID cpf d s_new bb]|+1.4000000000000X+003|+1.5555555555555X-002|"},
    {"a", "ah_ren_lab", 257,
     "PREC|ah_ren_lab|0|int|%6.0f|[A label]|[al]|[ten]|[ID a2 d s_new bb]|+1.4000000000000X+003|+1.4000000000000X+005|"},
    {"a", "ah_ren_exist", 306,
     "PREC|ah_ren_exist|110|int|float|[ID a2 d_i s_new bb]|"},
    {"b", "ah_fu243_clone", 354,
     "PREC|ah_fu243_clone|0|float|%9.0g|+1.24924a0000000X-002|+1.24924a0000000X-002|+1.24924a0000000X-002|"},
    {"b", "ah_fu243_rename", 374,
     "PREC|ah_fu243_rename|0|float|%9.0g|+1.24924a0000000X-002|+1.24924a0000000X-002|+1.24924a0000000X-002|"},
    {"b", "ah_fu243_ren", 411,
     "PREC|ah_fu243_ren|0|float|%9.0g|+1.b6db6e0000000X-002|+1.b6db6e0000000X-002|+1.b6db6e0000000X-002|"},
    {"b", "ah_fu243_bigint", 448,
     "PREC|ah_fu243_bigint|0|float|%9.0g|+1.0000000000000X+018|+1.0000000000000X+018|+1.0000000000000X+018|"},
    {"e", "ah_recode_else", 775,
     "PREC|ah_recode_else|0|byte|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|"},
    {"e", "ah_recode_nonmissing", 826,
     "PREC|ah_recode_nonmissing|0|byte|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"},
    {"e", "ah_recode_promote", 877,
     "PREC|ah_recode_promote|0|float|+1.f400000000000X+009|+1.0000000000000X-001|+1.8000000000000X+001|+1.0000000000000X+002|+1.4000000000000X+002|+1.8000000000000X+002|+1.c000000000000X+002|+1.0000000000000X+3ff|"},
    {"e", "ah_recode_multi", 902,
     "PREC|ah_recode_multi|0|byte|double|+1.c000000000000X+002|+0.0000000000000X-3ff|+1.c000000000000X+001|+0.0000000000000X-3ff|"},
    {"e", "ah_recode_gen", 934,
     "PREC|ah_recode_gen|0|byte|%9.0g|[RECODE of r0 (raw code)]|[]|[r0 age dd rs r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 m1 m2 newunit]|+0.0000000000000X-3ff|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+3ff|"},
    {"e", "ah_recode_gen_range", 960,
     "PREC|ah_recode_gen_range|0|byte|%9.0g|[RECODE of age]|[]|+1.0000000000000X+000|+1.0000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|"},
    {"e", "ah_recode_generate", 986,
     "PREC|ah_recode_generate|0|byte|%9.0g|[RECODE of r0 (raw code)]|[]|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"},
    {"e", "ah_recode_g_lab", 1020,
     "PREC|ah_recode_g_lab|0|byte|%12.0g|[RECODE of age]|[agegroup]|[<18 years]|[35 and older]|+1.0000000000000X+000|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.6800000000000X+005|"},
    {"e", "ah_recode_gen_double", 1038,
     "PREC|ah_recode_gen_double|0|double|%9.0g|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|"},
    {"e", "ah_recode_gen_exist", 1052,
     "PREC|ah_recode_gen_exist|110|byte|+1.e000000000000X+003|"},
    {"e", "ah_recode_str", 1066,
     "PREC|ah_recode_str|108|str2|[a]|"},
    {"e", "ah_recode_else_missing", 1080,
     "PREC|ah_recode_else_missing|198|byte|+1.0000000000000X+000|+1.0000000000000X+3ff|"},
    {"f", "ah_decode_gen", 1134,
     "PREC|ah_decode_gen|0|str17|%17s|[Region code]|[]|[region nolab sh region_str]|[North]|[South-East region]|[]|[North]|[]|"},
    {"f", "ah_decode_short", 1159,
     "PREC|ah_decode_short|0|str2|%9s|[]|[]|[bb]|[a]|"},
    {"f", "ah_decode_nolab", 1175,
     "PREC|ah_decode_nolab|182|[region nolab sh region_str sh_s]|"},
    {"f", "ah_decode_string", 1205,
     "PREC|ah_decode_string|108|[region nolab sh region_str sh_s]|"},
    {"f", "ah_decode_tempvar", 1240,
     "PREC|ah_decode_tempvar|0|str17|%17s|[North]|[]|"},
    {"g", "ah_encode_gen", 1315,
     "PREC|ah_encode_gen|0|long|%8.0g|[Fruit name]|[e_es]|[Pear]|[apple]|[fig]|[pear]|[es id eb ew e_es]|+1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+000|"},
    {"g", "ah_encode_g", 1340,
     "PREC|ah_encode_g|0|long|%8.0g|[sector]|[Pear]|+1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+000|"},
    {"g", "ah_encode_generate", 1365,
     "PREC|ah_encode_generate|0|long|%8.0g|[occ]|[pear]|+1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+000|"},
    {"g", "ah_encode_label_new", 1390,
     "PREC|ah_encode_label_new|0|long|[intervention]|[Pear]|[pear]|+1.0000000000000X+002|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.8000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|"},
    {"g", "ah_encode_label_again", 1407,
     "PREC|ah_encode_label_again|0|[intervention]|[apple]|+1.0000000000000X+002|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.8000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|"},
    {"g", "ah_encode_label_exist", 1446,
     "PREC|ah_encode_label_exist|0|long|[ordl]|[pear]|[kiwi]|[Pear]|[apple]|[fig]|+1.0000000000000X+000|+1.c000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+003|+1.8000000000000X+002|"},
    {"g", "ah_encode_blank_all", 1471,
     "PREC|ah_encode_blank_all|0|long|[eb_c]|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"},
    {"g", "ah_encode_spaces", 1500,
     "PREC|ah_encode_spaces|0|long|[]|[x]|+1.0000000000000X+000|+1.0000000000000X+001|"},
    {"g", "ah_encode_numeric", 1530,
     "PREC|ah_encode_numeric|107|[es id eb ew e_es sector occ newcode nc2 es_bin eb_c ew_c]|"},
    {"g", "ah_encode_nogen", 1546,
     "PREC|ah_encode_nogen|198|[es id eb ew e_es sector occ newcode nc2 es_bin eb_c ew_c]|"},
    {"h", "ah_isid_missing", 1610,
     "PREC|ah_isid_missing|459|+1.0000000000000X+000|+1.0000000000000X+3ff|"},
    {"i", "ah_dup_report_all", 1650,
     "PREC|ah_dup_report_all|0|N=+1.0000000000000X+003|unique_value=+1.c000000000000X+002|+1.0000000000000X+003|"},
    {"i", "ah_dup_report_var", 1669,
     "PREC|ah_dup_report_var|0|N=+1.0000000000000X+003|unique_value=+1.4000000000000X+002|+1.0000000000000X+003|+1.2000000000000X+003|+1.0000000000000X+002|"},
    {"i", "ah_dup_report_two", 1687,
     "PREC|ah_dup_report_two|0|N=+1.0000000000000X+003|unique_value=+1.8000000000000X+002|"},
    {"i", "ah_dup_list", 1707,
     "PREC|ah_dup_list|0|N=+1.0000000000000X+003|+1.0000000000000X+003|+1.2000000000000X+003|+1.c000000000000X+002|+1.0000000000000X+002|"},
    {"i", "ah_dup_list_all", 1725,
     "PREC|ah_dup_list_all|0|N=+1.0000000000000X+003|+1.0000000000000X+003|+1.2000000000000X+003|+1.0000000000000X+002|"},
    {"i", "ah_dup_tag", 1744,
     "PREC|ah_dup_tag|0|N=+1.0000000000000X+003|byte|%12.0g|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|"},
    {"i", "ah_dup_drop_all", 1767,
     "PREC|ah_dup_drop_all|0|N=+1.0000000000000X+003|N_drop=+1.0000000000000X+000|+1.c000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+003|+1.0000000000000X+002|+1.0000000000000X+001|"},
    {"i", "ah_dup_drop_force", 1791,
     "PREC|ah_dup_drop_force|0|N=+1.0000000000000X+003|N_drop=+1.8000000000000X+001|+1.4000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.4000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+003|+1.c000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+000|"},
    {"i", "ah_dup_drop_force_two", 1815,
     "PREC|ah_dup_drop_force_two|0|N=+1.0000000000000X+003|N_drop=+1.0000000000000X+001|+1.8000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+003|+1.c000000000000X+002|+1.c000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+001|"},
    {"i", "ah_dup_drop_noforce", 1836,
     "PREC|ah_dup_drop_noforce|198|+1.0000000000000X+003|+1.2000000000000X+003|"},
    {"i", "ah_dup_drop_all_force", 1856,
     "PREC|ah_dup_drop_all_force|101|+1.0000000000000X+003|+1.2000000000000X+003|"},
    {"i", "ah_dup_drop_none", 1882,
     "PREC|ah_dup_drop_none|0|N=+1.c000000000000X+002|N_drop=+0.0000000000000X-3ff|+1.c000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+002|"},
    {"j", "ah_clone_byte", 1955,
     "PREC|ah_clone_byte|0|byte|%3.0f|[Byte src]|[cbl]|[one]|[cb ci cl cf cdb Year county_OG]|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+000|"},
    {"j", "ah_clone_str_if", 2130,
     "PREC|ah_clone_str_if|0|str12|%12s|[y1]|[]|"},
    {"j", "ah_clone_exist", 2144,
     "PREC|ah_clone_exist|110|byte|+1.0000000000000X+000|"},
    {"j", "ah_clone_absent", 2160,
     "PREC|ah_clone_absent|111|[cb ci cl cf cdb Year county_OG ci2 cl2 cf2 cd2 year cif sif]|"},
};

// The PREC records a do source prints, by arm id (a missing arm reads "").
std::map<std::string, std::string> precRecords(piet::SessionState& session, const std::string& src) {
  std::map<std::string, std::string> out;
  try {
    const auto blocks = piet::doexec::executeDoSource(src, session, piet::dispatch).blocks;
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "display") continue;
      std::istringstream lines(b.value("text", std::string()));
      std::string line;
      while (std::getline(lines, line)) {
        if (line.rfind("PREC|", 0) != 0) continue;
        const size_t bar = line.find('|', 5);
        if (bar != std::string::npos) out[line.substr(5, bar - 5)] = line;
      }
    }
  } catch (const std::exception& e) {
    out["__exception__"] = e.what();
  }
  return out;
}

// The fields after `PREC|arm|rc|type|format|' of an FU-243 record: its three
// %21x cells.
std::string cellsOf(const std::string& rec) {
  size_t pos = 0;
  for (int i = 0; i < 5 && pos != std::string::npos; ++i) pos = rec.find('|', pos + 1);
  return pos == std::string::npos ? std::string("<no cells>") : rec.substr(pos + 1);
}

}  // namespace

void testW2AhDataVerbs() {
  std::printf("[selftest] --- W2AH data verbs: rename/recode/encode/decode/isid/duplicates/clonevar "
              "(w2ah_data_verbs.log) ---\n");
  namespace fs = std::filesystem;
  const std::string root = (fs::temp_directory_path() / "piet_native_selftest_w2ah").u8string();
  std::error_code ec;
  fs::remove_all(root, ec);
  for (const AhSection& sec : kAhSections) {
    const std::string workdir = root + "/" + sec.id;
    fs::create_directories(workdir, ec);
    piet::SessionState session(workdir);
    const auto got = precRecords(session, sec.src);
    for (const AhArm& a : kAhArms) {
      if (std::string(a.section) != sec.id) continue;
      auto it = got.find(a.arm);
      const std::string have = it == got.end() ? std::string("<no record>") : it->second;
      ahCheck(have == a.oracle, std::string("W2AH ") + a.arm + " (w2ah_data_verbs.log:" +
                                    std::to_string(a.logLine) + ") -- got " + have);
    }
    if (std::string(sec.id) == "b") {
      // FU-243 (BUG-964): the replace after clonevar / rename / ren of an untyped
      // generate stores what an untyped generate of the same value stores.
      auto rec = [&](const char* arm) {
        auto it = got.find(arm);
        return it == got.end() ? std::string("<no record>") : it->second;
      };
      ahCheck(cellsOf(rec("ah_fu243_clone")) == cellsOf(rec("ah_fu243_twin")),
              "W2AH FU-243: `replace y = 2/7' on a clone of an untyped generate equals `generate z "
              "= 2/7' (ah_fu243_clone vs ah_fu243_twin) -- got " + rec("ah_fu243_clone"));
      ahCheck(cellsOf(rec("ah_fu243_rename")) == cellsOf(rec("ah_fu243_twin")),
              "W2AH FU-243: `replace w = 2/7' after `rename x w' equals `generate z = 2/7' "
              "(ah_fu243_rename vs ah_fu243_twin) -- got " + rec("ah_fu243_rename"));
      ahCheck(cellsOf(rec("ah_fu243_ren")) == cellsOf(rec("ah_fu243_twin3")),
              "W2AH FU-243: `replace w2 = 3/7' after `ren x2 w2' equals `generate z3 = 3/7' "
              "(ah_fu243_ren vs ah_fu243_twin3) -- got " + rec("ah_fu243_ren"));
    }
  }
  fs::remove_all(root, ec);
  std::printf("[selftest] W2AH data verbs: %d checks\n", gAhChecks);
}
