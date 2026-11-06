clear all
set more off
capture log close
log using "/tmp/cobra_t4_vif.log", replace text
sysuse auto, clear
regress price mpg weight length turn
scalar N=e(N)
scalar Kx=e(df_m)
scalar e_r2=e(r2)
estat vif
scalar st_vif_weight=.
scalar st_vif_length=.
scalar st_vif_turn=.
scalar st_vif_mpg=.
forvalues i=1/`=Kx' {
    local nm `: display r(name_`i')'
    scalar st_vif_`nm'=r(vif_`i')
}
local xvars mpg weight length turn
file open out using "/tmp/cobra_t4_vif.csv", write replace text
file write out "key,value" _n
file write out "n_obs," %21.15e (N) _n
file write out "k_regressors," %21.15e (Kx) _n
file write out "main_r2," %21.15e (e_r2) _n
scalar vifsum=0
local J=0
foreach v of local xvars {
    local others : list xvars - v
    quietly regress `v' `others'
    scalar r2_`v'=e(r2)
    scalar tol_`v'=1-e(r2)
    scalar vif_`v'=1/(1-e(r2))
    scalar vifsum=vifsum+vif_`v'
    local J=`J'+1
    file write out "r2_`v',"  %21.15e (r2_`v')  _n
    file write out "tol_`v'," %21.15e (tol_`v') _n
    file write out "vif_`v'," %21.15e (vif_`v') _n
}
scalar mean_vif=vifsum/`J'
file write out "mean_vif," %21.15e (mean_vif) _n
file write out "stata_rvif_mpg,"    %21.15e (st_vif_mpg)    _n
file write out "stata_rvif_weight," %21.15e (st_vif_weight) _n
file write out "stata_rvif_length," %21.15e (st_vif_length) _n
file write out "stata_rvif_turn,"   %21.15e (st_vif_turn)   _n
file close out
log close