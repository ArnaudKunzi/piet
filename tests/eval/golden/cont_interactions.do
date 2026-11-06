* ============================================================================
* Stata truth for cobra 0.12 continuous-interaction goldens (MSG-MI-059 / CO-0.12)
* StataNow BE 19.5, public sysuse auto (n=74). Full precision: display %21.15e.
* Captures the EXACT Stata coefficient (term) names cobra must match, plus
* b/se per term, N, and residual dof for each of the four models.
*   M1  regress mpg c.weight#c.length          (continuous x continuous product)
*   M2  regress mpg c.weight#i.foreign         (slope-by-group)
*   M3  regress mpg c.weight##c.length         (full factorial, continuous)
*   M4  regress mpg i.foreign##c.weight         (full factorial, factor x continuous)
* Output: cont_interactions.log  (read by Eval Opus, transcribed into goldens).
* ============================================================================
clear all
set linesize 255
sysuse auto, clear

* helper: dump every coef name + b + se at full precision for the active e()
capture program drop dumpci
program define dumpci
    local names : colnames e(b)
    di as txt "{hline 70}"
    di as txt "TERMNAMES: `names'"
    di as txt "N = " e(N)  "   df_r = " e(df_r)  "   df_m = " e(df_m)
    di as txt "{hline 70}"
    matrix b = e(b)
    matrix V = e(V)
    local k = colsof(b)
    forvalues j = 1/`k' {
        local nm : word `j' of `names'
        local bj = b[1,`j']
        local vj = V[`j',`j']
        local sj = sqrt(`vj')
        di as res "TERM|" "`nm'" "|b=" %21.15e (`bj') "|se=" %21.15e (`sj')
    }
    di as txt "scalar N      = " %21.15e (e(N))
    di as txt "scalar df_r   = " %21.15e (e(df_r))
    di as txt "scalar r2     = " %21.15e (e(r2))
    di as txt "scalar rmse   = " %21.15e (e(rmse))
end

di as err "===== M1: regress mpg c.weight#c.length ====="
regress mpg c.weight#c.length
dumpci

di as err "===== M2: regress mpg c.weight#i.foreign ====="
regress mpg c.weight#i.foreign
dumpci

di as err "===== M3: regress mpg c.weight##c.length ====="
regress mpg c.weight##c.length
dumpci

di as err "===== M4: regress mpg i.foreign##c.weight ====="
regress mpg i.foreign##c.weight
dumpci

di as err "===== DONE ====="
