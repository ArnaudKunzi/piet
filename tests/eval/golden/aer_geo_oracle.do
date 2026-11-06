clear all
set more off
set linesize 200
local source "../../kitchensink/aer/geo"
local work "`c(pwd)'/../../kitchensink/aer/geo/.oracle"
capture mkdir "`work'"
foreach ext in shp shx dbf {
    copy "`source'/fixture.`ext'" "`work'/fixture.`ext'", replace
    copy "`source'/point.`ext'" "`work'/point.`ext'", replace
}
cd "`work'"
about
capture noisily which shp2dta
display "WHICH_SHP2DTA_RC=" _rc
capture noisily which geoinpoly
display "WHICH_GEOINPOLY_RC=" _rc
spshape2dta fixture.shp, replace saving(fx)
use fx, clear
describe
char list
ds, has(type numeric)
format `r(varlist)' %21x
list, noobs clean
use fx_shp, clear
describe
char list
ds, has(type numeric)
format `r(varlist)' %21x
list, noobs clean
capture noisily spshape2dta fixture.shp, saving(fx)
display "EXISTS_RC=" _rc
capture noisily spshape2dta point.shp, replace saving(pt)
local point_rc = _rc
display "POINT_RC=" `point_rc'
if `point_rc' == 0 {
    use pt, clear
    describe
    list, noobs clean
    use pt_shp, clear
    describe
    list, noobs clean
}
display "DONE:aer_geo"
