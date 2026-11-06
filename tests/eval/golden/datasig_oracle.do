* P1-A3-DATASIG-ORACLE -- Q9 empirical observation.
* Truth-source capture of live Stata `datasignature` behavior over TWO
* existing read fixtures (sysuse auto -- already reused elsewhere in this
* manifest via auto.hbin; golden/f16_tw_fixture.dta -- already committed for
* the C1-F16 %tw reject cell), under controlled mutations, to determine
* whether the algorithm is determinism/order/type/format-sensitive. Answers
* writer Q9 (must the engine reproduce Stata's exact signature bit-for-bit,
* or is a mutually-deterministic engine-only signature enough).
* Read-only probe: no fixture file is modified on disk (sysuse + `use` of the
* committed .dta, all mutations happen in memory only; nothing is `save`d).

* ---------- Fixture A: sysuse auto ----------
sysuse auto, clear
di "MARK_A_DETERMINISM_1"
datasignature
di "MARK_A_DETERMINISM_2"
datasignature

di "MARK_A_BASE"
datasignature

order foreign, first
di "MARK_A_REORDER"
datasignature

sysuse auto, clear
gen double dblvar = mpg
di "MARK_A_DOUBLE"
datasignature
recast float dblvar
di "MARK_A_FLOAT_NARROW"
datasignature

sysuse auto, clear
replace price = price[1] + 1 in 1
di "MARK_A_CELLEDIT"
datasignature

* ---------- Fixture B: existing golden/f16_tw_fixture.dta ----------
use "f16_tw_fixture.dta", clear
di "MARK_B_BASE"
datasignature

order y, first
di "MARK_B_REORDER"
datasignature

use "f16_tw_fixture.dta", clear
format wk %td
di "MARK_B_FORMAT_ONLY"
datasignature

use "f16_tw_fixture.dta", clear
replace wk = wk + 1 in 1
di "MARK_B_CELLEDIT"
datasignature

di "MARK_DONE"
exit, clear
