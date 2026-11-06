clear all
set more off
sysuse auto, clear

file open fh using "/tmp/cobra_t4_sdtest.csv", write replace text
file write fh "key,value" _n

*--------------------------------------------------------------------*
* CASE A: two-sample variance-comparison by group: sdtest mpg, by(foreign)
*--------------------------------------------------------------------*
sdtest mpg, by(foreign)

file write fh "A_cmd,sdtest mpg by(foreign)" _n
file write fh "A_N,"      %21.15e (r(N))     _n
file write fh "A_df_1,"   %21.15e (r(df_1))  _n
file write fh "A_df_2,"   %21.15e (r(df_2))  _n
file write fh "A_F,"      %21.15e (r(F))     _n
file write fh "A_sd_1,"   %21.15e (r(sd_1))  _n
file write fh "A_sd_2,"   %21.15e (r(sd_2))  _n
file write fh "A_p,"      %21.15e (r(p))     _n
file write fh "A_p_l,"    %21.15e (r(p_l))   _n
file write fh "A_p_u,"    %21.15e (r(p_u))   _n

* derived group summary stats (group order: foreign==0 first, foreign==1 second)
quietly summarize mpg if foreign==0
file write fh "A_g1_n,"    %21.15e (r(N))    _n
file write fh "A_g1_mean," %21.15e (r(mean)) _n
file write fh "A_g1_sd,"   %21.15e (r(sd))   _n
file write fh "A_g1_se,"   %21.15e (r(sd)/sqrt(r(N))) _n
scalar g1n = r(N)
scalar g1m = r(mean)
scalar g1s = r(sd)
quietly summarize mpg if foreign==1
file write fh "A_g2_n,"    %21.15e (r(N))    _n
file write fh "A_g2_mean," %21.15e (r(mean)) _n
file write fh "A_g2_sd,"   %21.15e (r(sd))   _n
file write fh "A_g2_se,"   %21.15e (r(sd)/sqrt(r(N))) _n
scalar g2n = r(N)
scalar g2m = r(mean)
scalar g2s = r(sd)

* mean CIs (t-based, level 95)
scalar tcrit1 = invttail(g1n-1, 0.025)
scalar tcrit2 = invttail(g2n-1, 0.025)
file write fh "A_g1_ci_lo," %21.15e (g1m - tcrit1*g1s/sqrt(g1n)) _n
file write fh "A_g1_ci_hi," %21.15e (g1m + tcrit1*g1s/sqrt(g1n)) _n
file write fh "A_g2_ci_lo," %21.15e (g2m - tcrit2*g2s/sqrt(g2n)) _n
file write fh "A_g2_ci_hi," %21.15e (g2m + tcrit2*g2s/sqrt(g2n)) _n

* combined row
quietly summarize mpg
file write fh "A_comb_n,"    %21.15e (r(N))    _n
file write fh "A_comb_mean," %21.15e (r(mean)) _n
file write fh "A_comb_sd,"   %21.15e (r(sd))   _n

*--------------------------------------------------------------------*
* CASE B: one-sample test of variance: sdtest mpg == 5
*--------------------------------------------------------------------*
sdtest mpg == 5

file write fh "B_cmd,sdtest mpg == 5" _n
file write fh "B_N,"    %21.15e (r(N))    _n
file write fh "B_df,"   %21.15e (r(df))   _n
file write fh "B_chi2," %21.15e (r(chi2)) _n
file write fh "B_sd,"   %21.15e (r(sd))   _n
file write fh "B_p,"    %21.15e (r(p))    _n
file write fh "B_p_l,"  %21.15e (r(p_l))  _n
file write fh "B_p_u,"  %21.15e (r(p_u))  _n

* summary stats underlying it
quietly summarize mpg
file write fh "B_mean," %21.15e (r(mean)) _n
file write fh "B_se,"   %21.15e (r(sd)/sqrt(r(N))) _n
scalar bn = r(N)
scalar bm = r(mean)
scalar bs = r(sd)
scalar btcrit = invttail(bn-1, 0.025)
file write fh "B_ci_lo," %21.15e (bm - btcrit*bs/sqrt(bn)) _n
file write fh "B_ci_hi," %21.15e (bm + btcrit*bs/sqrt(bn)) _n

* explicit hand recomputation cross-checks
* one-sample chi2 = (n-1)*s^2/SD0^2 ; SD0=5
file write fh "B_chi2_recomp," %21.15e ((bn-1)*bs^2/25) _n
* one-sided tails on chi2 dist
file write fh "B_PrCltc," %21.15e (chi2(bn-1, (bn-1)*bs^2/25)) _n
file write fh "B_PrCgtc," %21.15e (chi2tail(bn-1, (bn-1)*bs^2/25)) _n

*--------------------------------------------------------------------*
* CASE C: cross-check F two-sided p convention with raw tail probs
*--------------------------------------------------------------------*
* F = s1^2 / s2^2, df1=g1n-1, df2=g2n-1
scalar Fval = g1s^2/g2s^2
file write fh "C_F_recomp,"  %21.15e (Fval) _n
file write fh "C_PrFltf,"    %21.15e (F(g1n-1,g2n-1,Fval))     _n
file write fh "C_PrFgtf,"    %21.15e (Ftail(g1n-1,g2n-1,Fval)) _n
file write fh "C_two_min,"   %21.15e (2*min(F(g1n-1,g2n-1,Fval), Ftail(g1n-1,g2n-1,Fval))) _n

file close fh
display "DONE WRITING CSV"
type "/tmp/cobra_t4_sdtest.csv"