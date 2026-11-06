* ============================================================================
* aer_estim2_oracle.do — StataBE 19.5 + oaxaca 4.1.1 (SSC, Ben Jann) truth for the
* AER scope-2 B2 eval golden (manifest group "aer_estim2"): oaxaca_pooled_relax.
* Grammar: ORACLE|<golden csv>|<key>|<%21.15e> and ORACLEX|<csv>|<key>|<%21x>
* (rederive_golden.py / lint_self_captured_goldens.py). Keys == the manifest `q`.
* Run: tools/stata_batch.sh tests/eval/golden/aer_estim2_oracle.do AER_ESTIM2_ORACLE_END 600
* Judge: grep -c AER_ESTIM2_ORACLE_END aer_estim2_oracle.log == 1 and no ^r(NNN); line
*
* The fixture is built IN-BODY (no RNG) and exported with %24.17e so that the
* CSV cobra's convert-prep reads carries the SAME doubles Stata computed on.
* The method receipt for this command (why `pooled` implies robust suest, the
* two finite-sample corrections, the retained cross-covariance blocks) is
* internal note oaxaca-se-20260908/REPORT.md; this file only captures.
* ============================================================================
clear all
set more off
set type double
capture log close
log using "aer_estim2_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
which oaxaca

clear
set obs 200
gen double id = _n
gen double x1 = mod(_n * 7, 23) / 23
gen double x2 = mod(_n * 13, 17) / 17
gen byte   g  = mod(_n, 2)
gen byte   t  = mod(floor((_n - 1) / 3), 2)
gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5
format y x1 x2 %24.17e
export delimited id g y x1 x2 using "aer_estim2_oaxaca_input.csv", datafmt replace
display "INFO|ox_input|y[1]|" %21x y[1]

local csv aer_estim2_oaxaca_pooled_relax.csv
oaxaca y x1 x2, by(g) pooled relax
display "OX_BEGIN"
display "INFO|ox|e(vce)|" e(vce)
display "INFO|ox|e(suest)|" e(suest)
display "ORACLE|`csv'|n_obs|" %21.15e e(N)
display "ORACLE|`csv'|N_1|" %21.15e e(N_1)
display "ORACLE|`csv'|N_2|" %21.15e e(N_2)
matrix bo = e(b)
matrix Vo = e(V)
local cn : colnames bo
local ce : coleq bo
forval j = 1/`=colsof(bo)' {
    local nm : word `j' of `cn'
    local eq : word `j' of `ce'
    display "ORACLE|`csv'|coeftable[`eq':`nm'].b|" %21.15e bo[1,`j']
    display "ORACLEX|`csv'|coeftable[`eq':`nm'].b|" %21x bo[1,`j']
    display "ORACLE|`csv'|coeftable[`eq':`nm'].se|" %21.15e sqrt(Vo[`j',`j'])
    display "ORACLEX|`csv'|coeftable[`eq':`nm'].se|" %21x sqrt(Vo[`j',`j'])
}
* Off-diagonal e(V) entries: the cross-covariance blocks are where a wrong
* construction hides while every diagonal still looks close (REPORT.md exp. B).
foreach ij in "1 2" "1 3" "3 4" "4 5" "6 8" "8 10" {
    local i : word 1 of `ij'
    local j : word 2 of `ij'
    display "ORACLE|`csv'|e_V.matrix[`=`i'-1'][`=`j'-1']|" %21.15e Vo[`i',`j']
    display "ORACLEX|`csv'|e_V.matrix[`=`i'-1'][`=`j'-1']|" %21x Vo[`i',`j']
}
display "OX_END"

* Edge behaviours the PIET handler pins (rc only; the numbers above are the golden).
gen byte g3 = mod(_n, 3)
capture oaxaca y x1 x2, by(g3) pooled relax
display "INFO|ox_edge|three_groups_rc|" _rc
capture oaxaca y x1 x2, pooled
display "INFO|ox_edge|no_by_rc|" _rc
gen double dup = 2*x1
capture oaxaca y x1 x2 dup, by(g) pooled
display "INFO|ox_edge|dup_norelax_rc|" _rc
capture oaxaca y x1 x2 dup, by(g) pooled relax
display "INFO|ox_edge|dup_relax_rc|" _rc
matrix bd = e(b)
display "INFO|ox_edge|dup_relax_b_explained_x1|" %21x bd[1,6]
display "INFO|ox_edge|dup_relax_b_explained_dup|" %21x bd[1,8]
quietly oaxaca y x1 x2 if _n <= 150, by(g) pooled relax
display "INFO|ox_edge|if150_N|" e(N)
gen byte sch2 = mod(_n, 4) == 0
gen byte sch3 = mod(_n, 4) == 1
quietly oaxaca y x1 sch2-sch3, by(g) pooled relax noisily
matrix br = e(b)
display "INFO|ox_edge|range_ncol|" colsof(br)
display "INFO|ox_edge|range_colnames|" "`: colnames br'"
* the pangram S38 pins, for the tier-2 key
display "PANGRAM_PIN S38.oaxaca_N = " e(N)
quietly oaxaca y x1 x2, by(g) pooled relax
display "PANGRAM_PIN S38.oaxaca_rc = " _rc
display "PANGRAM_PIN S38.oaxaca_N = " e(N)
display "PANGRAM_PIN S38.oaxaca_N_1 = " e(N_1)
display "PANGRAM_PIN S38.oaxaca_diff = " _b[overall:difference]
display "PANGRAM_PIN S38.oaxaca_explained = " _b[overall:explained]
display "PANGRAM_PIN S38.oaxaca_unexplained = " _b[overall:unexplained]
display "PANGRAM_PIN S38.oaxaca_se_g1 = " _se[overall:group_1]
capture oaxaca y x1 x2, pooled
display "PANGRAM_PIN S38.oaxaca_noby_rc = " _rc

* ============================================================================
* teffects nnmatch (StataCorp [CAUSAL] -- AER scope-2 B2-4, paper 076).
* Method identity is internal note teffects-nnmatch-20260908/REPORT.md as
* CORRECTED by REFUTE-0.md (the H2 chained-tolerance match-set rule) and
* REFUTE-1.md (anchors).  This block only CAPTURES; nothing here computes.
* Four goldens: the B2 fixture (continuous, defaults), the production shape of
* clean_15_matching.do:366 (six i. factors + gen()), an atet run with
* nneighbor(2), and a NESTED-FACTOR run that forces _rmcoll to drop a column
* (REPORT open question 3 -- no fixture existed for it).
* ============================================================================
clear
set obs 200
gen double id = _n
gen double x1 = mod(_n * 7, 23) / 23
gen double x2 = mod(_n * 13, 17) / 17
gen byte   g  = mod(_n, 2)
gen byte   t  = mod(floor((_n - 1) / 3), 2)
gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5
format y x1 x2 %24.17e
export delimited id t y x1 x2 using "aer_estim2_nnmatch_input.csv", datafmt replace

local csv aer_estim2_nnmatch_ate.csv
teffects nnmatch (y x1 x2) (t), gen(mm)
display "NN_BEGIN"
display "INFO|nn|e(cmd)|" e(cmd)
display "INFO|nn|e(subcmd)|" e(subcmd)
display "INFO|nn|e(vcetype)|" e(vcetype)
display "INFO|nn|e(metric)|" e(metric)
display "INFO|nn|e(stat)|" e(stat)
display "INFO|nn|e(title)|" e(title)
display "INFO|nn|e(tlevels)|" e(tlevels)
display "INFO|nn|e(indexvar)|" e(indexvar)
display "INFO|nn|e(fvdvarlist)|" e(fvdvarlist)
display "INFO|nn|dtolerance|" %21x e(dtolerance)
matrix bn = e(b)
display "INFO|nn|bcolnames|" "`: colfullnames bn'"
display "ORACLE|`csv'|n_obs|" %21.15e e(N)
display "ORACLE|`csv'|n0|" %21.15e e(n0)
display "ORACLE|`csv'|n1|" %21.15e e(n1)
display "ORACLE|`csv'|k_nneighbor|" %21.15e e(k_nneighbor)
display "ORACLE|`csv'|k_nnmin|" %21.15e e(k_nnmin)
display "ORACLE|`csv'|k_nnmax|" %21.15e e(k_nnmax)
display "ORACLE|`csv'|k_robust|" %21.15e e(k_robust)
matrix Vn = e(V)
display "ORACLEX|`csv'|coeftable[ATE:r1vs0.t].b|" %21x bn[1,1]
display "ORACLEX|`csv'|coeftable[ATE:r1vs0.t].se|" %21x sqrt(Vn[1,1])
display "ORACLEX|`csv'|e_V.matrix[0][0]|" %21x Vn[1,1]
display "ORACLE|`csv'|matches.k_max|" %21.15e e(k_nnmax)
display "ORACLE|`csv'|matches.ids[0][0]|" %21.15e mm1[1]
display "ORACLE|`csv'|matches.ids[1][0]|" %21.15e mm1[2]
display "ORACLE|`csv'|matches.ids[4][0]|" %21.15e mm1[5]
display "ORACLE|`csv'|matches.ids[103][0]|" %21.15e mm1[104]
display "ORACLE|`csv'|matches.ids[103][1]|" %21.15e mm2[104]
display "ORACLE|`csv'|matches.ids[199][0]|" %21.15e mm1[200]
predict double te_, te
predict double po0_ po1_, po
predict double dd_*, distance
display "ORACLEX|`csv'|te[0]|" %21x te_[1]
display "ORACLEX|`csv'|te[103]|" %21x te_[104]
display "ORACLEX|`csv'|po0[0]|" %21x po0_[1]
display "ORACLEX|`csv'|po1[0]|" %21x po1_[1]
display "ORACLEX|`csv'|dist[0][0]|" %21x dd_1[1]
quietly count if !missing(mm1)
display "ORACLE|`csv'|matches_nonmissing|" %21.15e r(N)

* Edge rc's the handler pins (rc only; every number above is the golden).
capture noisily teffects nnmatch (y x1 x2) (t), nneighbor(500)
display "INFO|nn_edge|nn_too_large_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), vce(robust, nn(1))
display "INFO|nn_edge|vce_nn1_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), gen(mm)
display "INFO|nn_edge|gen_collision_rc|" _rc
gen byte t12 = t + 1
capture noisily teffects nnmatch (y x1 x2) (t12), gen(zz)
display "INFO|nn_edge|t12_rc|" _rc
capture matrix b12 = e(b)
display "INFO|nn_edge|t12_bcolnames|" "`: colfullnames b12'"
* `if` on an ODD-ROW subsample: are the gen() ids _n of the FULL data or of the
* kept subsample?  Odd ids answer "full data"; 1..100 answers "subsample".
quietly teffects nnmatch (y x1 x2) (t) if mod(id,2)==1, gen(ifm)
display "INFO|nn_edge|ifodd_N|" e(N)
display "INFO|nn_edge|ifodd_mm1_1|" ifm1[1]
display "INFO|nn_edge|ifodd_mm1_3|" ifm1[3]
quietly count if !missing(ifm1)
display "INFO|nn_edge|ifodd_nonmissing|" r(N)
quietly count if !missing(ifm1) & mod(ifm1,2)==0
display "INFO|nn_edge|ifodd_even_ids|" r(N)

* ---- production shape: six i. factors, 1:4 treated, gen(match_id) ----------
clear
set obs 600
gen double id = _n
gen byte f1 = mod(_n, 2)
gen byte f2 = mod(floor(_n / 3), 3)
gen byte f3 = mod(floor(_n / 5), 5)
gen byte f4 = mod(floor(_n / 11), 4)
gen byte f5 = mod(floor(_n / 13), 2)
gen byte f6 = mod(floor(_n / 17), 3)
gen byte treatment = (mod(_n, 5) == 0)
gen double outcome = 1 + 0.5*f1 + 0.3*f2 - 0.2*f3 + 0.1*f4 - 0.4*f5 + 0.15*f6 + 0.25*treatment + mod(_n * 13, 37)/37
format outcome %24.17e
export delimited id treatment outcome f1 f2 f3 f4 f5 f6 using "aer_estim2_nnmatch_prod_input.csv", datafmt replace
local csv2 aer_estim2_nnmatch_prod.csv
teffects nnmatch (outcome i.f1 i.f2 i.f3 i.f4 i.f5 i.f6) (treatment), gen(match_id)
matrix bp = e(b)
matrix Vp = e(V)
display "INFO|nn_prod|bcolnames|" "`: colfullnames bp'"
display "INFO|nn_prod|fvdvarlist|" "`e(fvdvarlist)'"
display "INFO|nn_prod|indexvar|" "`e(indexvar)'"
display "ORACLE|`csv2'|n_obs|" %21.15e e(N)
display "ORACLE|`csv2'|n0|" %21.15e e(n0)
display "ORACLE|`csv2'|n1|" %21.15e e(n1)
display "ORACLE|`csv2'|k_nnmin|" %21.15e e(k_nnmin)
display "ORACLE|`csv2'|k_nnmax|" %21.15e e(k_nnmax)
display "ORACLEX|`csv2'|coeftable[ATE:r1vs0.treatment].b|" %21x bp[1,1]
display "ORACLEX|`csv2'|coeftable[ATE:r1vs0.treatment].se|" %21x sqrt(Vp[1,1])
display "ORACLE|`csv2'|matches.ids[0][0]|" %21.15e match_id1[1]
display "ORACLE|`csv2'|matches.ids[4][0]|" %21.15e match_id1[5]
display "ORACLE|`csv2'|matches.ids[364][0]|" %21.15e match_id1[365]
display "ORACLE|`csv2'|matches.ids[529][0]|" %21.15e match_id1[530]
display "ORACLE|`csv2'|matches.ids[599][0]|" %21.15e match_id1[600]
* the paper's consumer (clean_15_matching.do:382-413) keeps match_id1 of the
* TREATED rows and joins it to control row numbers: that join must be lossless.
quietly count if treatment==1 & !missing(match_id1)
display "ORACLE|`csv2'|treated_with_match|" %21.15e r(N)

* ---- atet with nneighbor(2): control rows are filled too -------------------
clear
set obs 90
gen double id = _n
gen byte a = mod(_n, 2)
gen byte b = mod(floor(_n / 2), 3)
gen byte c = mod(floor(_n / 5), 5)
gen byte t = (mod(_n, 5) != 0)
gen double y = 1 + 0.5*a + 0.3*b - 0.2*c + 0.25*t + mod(_n * 11, 31)/31
format y %24.17e
export delimited id t y a b c using "aer_estim2_nnmatch_atet_input.csv", datafmt replace
local csv3 aer_estim2_nnmatch_atet.csv
teffects nnmatch (y i.a i.b i.c) (t), atet nneighbor(2) gen(am)
matrix ba = e(b)
matrix Va = e(V)
display "INFO|nn_atet|bcolnames|" "`: colfullnames ba'"
display "ORACLE|`csv3'|n_obs|" %21.15e e(N)
display "ORACLE|`csv3'|n0|" %21.15e e(n0)
display "ORACLE|`csv3'|n1|" %21.15e e(n1)
display "ORACLE|`csv3'|k_nnmin|" %21.15e e(k_nnmin)
display "ORACLE|`csv3'|k_nnmax|" %21.15e e(k_nnmax)
display "ORACLEX|`csv3'|coeftable[ATET:r1vs0.t].b|" %21x ba[1,1]
display "ORACLEX|`csv3'|coeftable[ATET:r1vs0.t].se|" %21x sqrt(Va[1,1])
display "ORACLE|`csv3'|matches.ids[0][0]|" %21.15e am1[1]
display "ORACLE|`csv3'|matches.ids[4][0]|" %21.15e am1[5]
display "ORACLE|`csv3'|matches.ids[89][0]|" %21.15e am1[90]
quietly count if t==0 & !missing(am1)
display "ORACLE|`csv3'|control_rows_filled|" %21.15e r(N)

* ---- NESTED FACTORS: _rmcoll must drop a matching column ------------------
* i.b has four levels, a == (b>=2), so 1.a == 2.b + 3.b exactly.  Which column
* does Stata drop, and what does the estimate become?  REPORT §6 lists this as
* untested; e(fvdvarlist) / e(mscale) row names answer it.
clear
set obs 120
gen double id = _n
gen byte b = mod(_n, 4)
gen byte a = (b >= 2)
gen byte t = mod(floor((_n - 1) / 3), 2)
gen double y = 1 + 0.5*a + 0.3*b + 0.25*t + mod(_n * 11, 31)/31
format y %24.17e
export delimited id t y a b using "aer_estim2_nnmatch_rmcoll_input.csv", datafmt replace
local csv4 aer_estim2_nnmatch_rmcoll.csv
capture noisily teffects nnmatch (y i.a i.b) (t), gen(rm)
display "INFO|nn_rmcoll|rc|" _rc
display "INFO|nn_rmcoll|fvdvarlist|" "`e(fvdvarlist)'"
display "INFO|nn_rmcoll|mvarlist|" "`e(mvarlist)'"
capture matrix msr = e(mscale)
display "INFO|nn_rmcoll|mscale_rows|" "`: rowfullnames msr'"
capture matrix mstr = e(mstats)
display "INFO|nn_rmcoll|mstats_cols|" "`: colfullnames mstr'"
matrix br = e(b)
matrix Vr = e(V)
display "ORACLE|`csv4'|n_obs|" %21.15e e(N)
display "ORACLE|`csv4'|k_nnmax|" %21.15e e(k_nnmax)
display "ORACLEX|`csv4'|coeftable[ATE:r1vs0.t].b|" %21x br[1,1]
display "ORACLEX|`csv4'|coeftable[ATE:r1vs0.t].se|" %21x sqrt(Vr[1,1])
display "ORACLE|`csv4'|matches.ids[0][0]|" %21.15e rm1[1]
display "ORACLE|`csv4'|matches.ids[1][0]|" %21.15e rm1[2]
display "ORACLE|`csv4'|matches.ids[119][0]|" %21.15e rm1[120]
* the same design WITHOUT the redundant factor, for the "same answer" check
capture noisily teffects nnmatch (y i.b) (t), gen(rn)
display "INFO|nn_rmcoll|bonly_rc|" _rc
matrix bn2 = e(b)
display "INFO|nn_rmcoll|bonly_b|" %21x bn2[1,1]
display "NN_END"

* ---- pangram S41 pins (tier-2 key) ----------------------------------------
clear
set obs 200
gen double id = _n
gen double x1 = mod(_n * 7, 23) / 23
gen double x2 = mod(_n * 13, 17) / 17
gen byte   g  = mod(_n, 2)
gen byte   t  = mod(floor((_n - 1) / 3), 2)
gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5
quietly teffects nnmatch (y x1 x2) (t), gen(mm)
display "PANGRAM_PIN S41.nn_rc = " _rc
display "PANGRAM_PIN S41.nn_N = " e(N)
display "PANGRAM_PIN S41.nn_n0 = " e(n0)
display "PANGRAM_PIN S41.nn_n1 = " e(n1)
display "PANGRAM_PIN S41.nn_kmax = " e(k_nnmax)
display "PANGRAM_PIN S41.nn_ate = " _b[ATE:r1vs0.t]
display "PANGRAM_PIN S41.nn_se = " _se[ATE:r1vs0.t]
display "PANGRAM_PIN S41.nn_mm1_1 = " mm1[1]
display "PANGRAM_PIN S41.nn_mm1_2 = " mm1[2]
display "PANGRAM_PIN S41.nn_mm1_200 = " mm1[200]
capture teffects nnmatch (y x1 x2) (t), vce(robust, nn(1))
display "PANGRAM_PIN S41.nn_vcenn1_rc = " _rc
capture teffects nnmatch (y x1 x2) (t), nneighbor(500)
display "PANGRAM_PIN S41.nn_toolarge_rc = " _rc
capture teffects ipw (y) (t x1 x2)
display "PANGRAM_PIN S41.nn_ipw_rc = " _rc

* ---- B2-4 review pass (2026-09-09): the axes NO fixture read -------------
* Four review findings needed measurement, not argument: (1) what interval
* `teffects nnmatch` actually reports at the default level, (2) whether
* `level(#)` moves it and what Stata refuses, (3) what e(tlevels) is when the
* treatment is NOT 0/1, (4) whether e(mvarlist) keeps the factor operator in a
* MIXED typed order.  Same 200-row in-body fixture (no RNG).
clear
set obs 200
gen double id = _n
gen double x1 = mod(_n * 7, 23) / 23
gen double x2 = mod(_n * 13, 17) / 17
gen byte   g  = mod(_n, 2)
gen byte   t  = mod(floor((_n - 1) / 3), 2)
gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5
local csvL aer_estim2_nnmatch_ate.csv
teffects nnmatch (y x1 x2) (t)
matrix rtL = r(table)
display "ORACLEX|`csvL'|coeftable[ATE:r1vs0.t].ci_lo|" %21x rtL[5,1]
display "ORACLEX|`csvL'|coeftable[ATE:r1vs0.t].ci_hi|" %21x rtL[6,1]
display "INFO|nn_level|default_e_level|" e(level)
display "INFO|nn_level|default_lo_dec|" %12.7f rtL[5,1]
display "INFO|nn_level|default_hi_dec|" %12.7f rtL[6,1]
teffects nnmatch (y x1 x2) (t), level(90)
matrix rt90 = r(table)
display "INFO|nn_level|l90_e_level|" e(level)
display "INFO|nn_level|l90_lo|" %21x rt90[5,1]
display "INFO|nn_level|l90_hi|" %21x rt90[6,1]
display "PANGRAM_PIN S41.nn_level90 = " e(level)
capture teffects nnmatch (y x1 x2) (t), level(90)
display "PANGRAM_PIN S41.nn_level90_rc = " _rc
display "PANGRAM_PIN S41.nn_ci90_lo = " rt90[5,1]
display "PANGRAM_PIN S41.nn_ci90_hi = " rt90[6,1]
display "PANGRAM_PIN S41.nn_ci95_lo = " rtL[5,1]
display "PANGRAM_PIN S41.nn_ci95_hi = " rtL[6,1]
capture noisily teffects nnmatch (y x1 x2) (t), level(0)
display "INFO|nn_level|l0_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), level(101)
display "INFO|nn_level|l101_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), level(9)
display "INFO|nn_level|l9_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), level(99.99)
display "INFO|nn_level|l9999_rc|" _rc
capture noisily teffects nnmatch (y x1 x2) (t), level(abc)
display "INFO|nn_level|labc_rc|" _rc
* e(tlevels) when the treatment is coded 1/2 (the colname says r2vs1).
gen byte t12 = t + 1
capture noisily teffects nnmatch (y x1 x2) (t12)
display "INFO|nn_level|t12_rc2|" _rc
display "INFO|nn_level|t12_tlevels|" "`e(tlevels)'"
display "INFO|nn_level|t12_treated|" e(treated)
display "INFO|nn_level|t12_control|" e(control)
* e(mvarlist) in MIXED typed order, and with an explicit c. operator.
gen byte a = mod(_n, 2)
gen byte b = mod(floor((_n - 1) / 2), 3)
capture noisily teffects nnmatch (y x1 i.a i.b) (t)
display "INFO|nn_level|mixed_rc|" _rc
display "INFO|nn_level|mixed_mvarlist|" "`e(mvarlist)'"
display "INFO|nn_level|mixed_fvdvarlist|" "`e(fvdvarlist)'"
capture noisily teffects nnmatch (y c.x1 i.a) (t)
display "INFO|nn_level|cdot_rc|" _rc
display "INFO|nn_level|cdot_mvarlist|" "`e(mvarlist)'"
* `in` — the registry refuses it today (r198); what does Stata do?
capture noisily teffects nnmatch (y x1 x2) (t) in 1/40
display "INFO|nn_level|in_rc|" _rc
display "INFO|nn_level|in_N|" e(N)
* The S41 pangram pins for the same axes (tier-2 key; the pangram surface has
* no r(table), so the CI itself is pinned by the eval golden / freeze /
* selftest and the pangram pins the option BEHAVIOUR).
capture teffects nnmatch (y x1 x2) (t), level(9)
display "PANGRAM_PIN S41.nn_l9_rc = " _rc
capture teffects nnmatch (y x1 x2) (t), level(101)
display "PANGRAM_PIN S41.nn_l101_rc = " _rc
quietly teffects nnmatch (y x1 i.a i.b) (t)
display "PANGRAM_PIN S41.nn_mvarlist = " "`e(mvarlist)'"
quietly teffects nnmatch (y x1 x2) (t12)
display "PANGRAM_PIN S41.nn_tlevels12 = " "`e(tlevels)'"
quietly teffects nnmatch (y x1 x2) (t)
display "PANGRAM_PIN S41.nn_tlevels01 = " "`e(tlevels)'"
display "NN_LEVEL_END"

local se1 "AER_ESTIM2"
local se2 "_ORACLE_END"
display "`se1'`se2'"
log close
