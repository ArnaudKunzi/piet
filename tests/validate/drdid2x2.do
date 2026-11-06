clear all
set more off
* Run from tests/validate: every path below is relative to it.
capture log close
log using "drdid2x2.log", replace text
use drdid2x2.dta, clear
drdid Y x1 x2, ivar(id) time(t) treatment(treat) dripw
matrix b=e(b)
matrix V=e(V)
di "DRDID_ATT=" %20.15g b[1,1] " SE=" %20.15g sqrt(V[1,1])
log close
