clear all
set more off
set linesize 255
capture program drop pin
program define pin
    args arm cmd
    display "ARM|`arm'"
    capture noisily `cmd'
    local fit_rc = _rc
    display _newline "RC|`arm'|" `fit_rc'
    if `fit_rc' != 0 exit
    ereturn list
    local scalars : e(scalars)
    foreach s of local scalars {
        display "SCALAR|`arm'|`s'|" %21x e(`s')
    }
    local macros : e(macros)
    foreach s of local macros {
        display "STRING|`arm'|`s'|`e(`s')'"
    }
    foreach mat in b V {
        matrix M = e(`mat')
        local cols : colfullnames M
        local colnum = 0
        foreach col of local cols {
            local ++colnum
            display "COLUMN|`arm'|`mat'|`colnum'|`col'"
        }
        forvalues i=1/`=rowsof(M)' {
            forvalues j=1/`=colsof(M)' {
                display "MATRIX|`arm'|`mat'|`i'|`j'|" %21x M[`i',`j']
            }
        }
    }
end
sysuse auto, clear
generate double w = 1 + mod(_n, 5) / 3
generate double off = mpg / 100
generate double expos = exp(off)
export delimited price mpg weight foreign rep78 w off expos using aer_estim2_glm_auto_input.csv, replace
pin auto_A "glm price mpg weight, family(gaussian) link(identity) nolog"
pin auto_B "glm price mpg weight [aw=w], family(poisson) link(log) vce(cluster rep78) nolog"
pin auto_C "glm price mpg weight, family(gamma) link(log) vce(cluster rep78) nolog"
pin auto_C_oim "glm price mpg weight, family(gamma) link(log) nolog"
pin auto_C_eim "glm price mpg weight, family(gamma) link(log) vce(eim) nolog"
pin auto_C_scale1 "glm price mpg weight, family(gamma) link(log) scale(1) nolog"
pin auto_D "glm foreign mpg weight, family(binomial) link(logit) nolog"
pin auto_D_probit "glm foreign mpg weight, family(binomial) link(probit) nolog"
pin auto_E "glm price mpg weight, family(nbinomial ml) link(log) nolog"
pin auto_F "glm price mpg weight [aw=w], family(poisson) link(log) vce(cluster rep78) eform nolog"
pin auto_G "glm price mpg weight, family(gamma) link(log) vce(cluster rep78) irls nolog"
pin auto_H_offset "glm price mpg weight [aw=w], family(poisson) link(log) vce(cluster rep78) offset(off) nolog"
pin auto_H_exposure "glm price mpg weight [aw=w], family(poisson) link(log) vce(cluster rep78) exposure(expos) nolog"
clear
set obs 240
generate id = mod(_n-1,40)+1
generate cl = mod(id-1,8)+1
generate double x = sin(_n*1.3)
generate double w = 1 + mod(_n,7)/4
generate double off = mod(_n,11)/20
generate double expos = exp(off)
generate double y = exp(.5+.3*x+id/100)*(0.2+mod(_n*37,101)/35)
generate count = floor(y*5)
generate binary = mod(_n*19,37)>18
export delimited using aer_estim2_glm_synthetic_input.csv, replace
pin syn_A "glm y c.x i.id, family(gaussian) link(identity) nolog"
pin syn_B "glm count c.x i.id [aw=w], family(poisson) link(log) vce(cluster cl) nolog"
pin syn_C "glm y c.x i.id, family(gamma) link(log) vce(cluster cl) nolog"
pin syn_C_oim "glm y c.x i.id, family(gamma) link(log) nolog"
pin syn_C_eim "glm y c.x i.id, family(gamma) link(log) vce(eim) nolog"
pin syn_D "glm binary c.x i.id, family(binomial) link(logit) nolog"
pin syn_D_probit "glm binary c.x i.id, family(binomial) link(probit) nolog"
pin syn_E "glm count c.x i.id, family(nbinomial ml) link(log) nolog"
pin syn_G "glm y c.x i.id, family(gamma) link(log) vce(cluster cl) irls nolog"
sysuse auto, clear
pin tight_C "glm price mpg weight, family(gamma) link(log) nolog tolerance(1e-14) ltolerance(1e-14) nrtolerance(1e-12)"
pin tight_C_cl "glm price mpg weight, family(gamma) link(log) vce(cluster rep78) nolog tolerance(1e-14) ltolerance(1e-14) nrtolerance(1e-12)"
pin tight_B "glm price mpg weight, family(poisson) link(log) nolog tolerance(1e-14) ltolerance(1e-14) nrtolerance(1e-12)"
* Independent Stata-only check: OIM at the returned beta times returned phi.
foreach arm in default tight {
    local tuning ""
    if "`arm'" == "tight" local tuning "tolerance(1e-14) ltolerance(1e-14) nrtolerance(1e-12)"
    quietly glm price mpg weight, family(gamma) link(log) `tuning'
    matrix reported_V = e(V)
    scalar reported_phi = e(phi)
    predict double audit_eta, xb
    generate double audit_h = price/exp(audit_eta)
    matrix accum audit_H = mpg weight [iw=audit_h]
    matrix recomputed_V = syminv(audit_H)*reported_phi
    scalar maxrel = 0
    forvalues i=1/3 {
        forvalues j=1/3 {
            scalar maxrel = max(maxrel,abs(recomputed_V[`i',`j']/reported_V[`i',`j']-1))
        }
    }
    display "STATA_OIM_CHECK|`arm'|" %21x maxrel
    drop audit_eta audit_h
}
display "DONE:glm"
