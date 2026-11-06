* MI-066 .dta reader test fixtures — regenerates tests/validate/dta_fixtures/
* (gitignored; *.dta is repo-wide ignored). Run from tests/validate/:
*   StataBE -b do dta_fixtures_gen.do
*
* Produces two logical datasets, each saved in all 3 supported formats:
*   mixed_{115,117,118}.dta — synthetic: value-labeled byte, string, double
*     (var-labeled), float, long, int, %td date, and a partial-missing column.
*     Exercises every storage type + a value label + a variable label + a
*     date format the native reader (src/io/dta_reader.cpp) must handle.
*   fatalities_{115,117,118}.dta — our own 336-obs panel (BUG-375; was auto.dta,
*     StataCorp's bundled dataset, retired so nothing Stata-bundled ships), a
*     second independent cross-check carrying a string column, a value-labeled
*     numeric, and a declared sort claim.
*
* Format notes (StataNow 19.5 saveold quirks, verified empirically):
*   version(12) -> true legacy binary format 115 (non-XML container).
*   version(13) -> XML-sectioned format 117.
*   save / version(14+) -> XML-sectioned format 118 (Stata's current default;
*     19.5 writes the same container Stata 14-18 do).
*   version(15)/version(17) do NOT produce formats 115/117 on this Stata
*     build — they silently no-op to the current 118 container. Only 12/13
*     produce genuinely distinct formats; verify with `xxd` if regenerating
*     on a different Stata version (are the first bytes "<stata_dta>" with
*     <release>115|117|118, or the true fixed-binary 115 header starting
*     with byte 0x73?).
clear
set more off
set obs 8
gen id = _n
gen byte sex = mod(_n,2)
label define sexlbl 0 "Female" 1 "Male"
label values sex sexlbl
gen str10 city = "City" + string(_n)
gen double x = _n * 1.5 + 0.1
gen float y = _n / 3.0
gen long big = _n * 1000000
gen int med = _n * 100
gen d = mdy(1,_n,2020)
format d %td
gen missing_test = .
replace missing_test = _n if _n > 4
label variable x "an x variable label"
save mixed_118.dta, replace
saveold mixed_117.dta, replace version(13)
saveold mixed_115.dta, replace version(12)

* BUG-375 (User ruling 2026-08-31): was `sysuse auto` — StataCorp's own bundled
* dataset, and the only Stata-bundled asset in the whole test tree. It is what made
* "can we ship the fixtures publicly" a LICENSING question instead of an
* engineering one. `fatalities` is a public-domain-derived panel this project
* already ships in its own demo package, so the question disappears rather than
* being deferred.
*
* The replacement keeps the structural features the reader test depends on:
*   a string column (was `make`)          -> state      (str2)
*   a value-labeled NUMERIC (was foreign) -> breathcode (long, 1=no 2=yes)
*   a declared sort claim (was on foreign)-> sorted on state
import delimited "../../piet/packaging/sample/fatalities.csv", clear varnames(1)
encode breath, gen(breathcode)
drop breath
sort state
save fatalities_118.dta, replace
saveold fatalities_117.dta, replace version(13)
saveold fatalities_115.dta, replace version(12)

* BUG-024 regression fixture — a string column with a genuine empty-string
* ("") cell mixed among non-empty cells. Stata/pandas.read_stata treat ""
* as a real, non-missing string value distinct from `.`; the native .dta
* reader must preserve it as its own factor level (see
* src/data/frame.cpp Frame::append_factor_column, src/io/dta_reader.cpp).
clear
set more off
set obs 6
gen id = _n
gen str10 name = "Name" + string(_n)
replace name = "" in 3
gen double val = _n * 2.5
save empty_str_118.dta, replace
saveold empty_str_117.dta, replace version(13)
saveold empty_str_115.dta, replace version(12)
