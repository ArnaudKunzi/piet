* Golden for factor-variable interactions (i. / c. / # / ##) — CoBrA fv parity.
* Fully-populated synthetic design so every factorial cell exists (no empty-cell
* omission), validating the design-matrix expansion against Stata's parser.
clear all
set seed 20260618
set obs 4000
gen treat = mod(_n,2)                      // 1,0,1,0 → balanced
gen post  = mod(floor((_n-1)/2),2)         // 0,0,1,1 → all 4 (treat,post) cells
gen g3    = mod(_n,3) + 1                   // 1,2,3 cycling
gen x = rnormal()
gen z = rnormal()
gen y = 2 + 1.5*treat + 0.8*post + 1.2*treat*post ///
        + 0.5*x - 0.3*z + 0.7*x*x ///
        + 0.4*(g3==2) + 0.9*(g3==3) + 0.6*(g3==2)*x + rnormal()

* data for cobra (native CSV reader → hbin)
export delimited treat post g3 x z y using "fvint.csv", replace

file open fh using "fvint_golden.csv", write replace text
file write fh "key,value" _n
program define dumpb
    file write fh "`1'," %21.15e (`2') _n
end

* (1) i.treat##i.post — main effects + 2x2 interaction
regress y i.treat##i.post
dumpb "c1.1.treat"            _b[1.treat]
dumpb "c1.1.post"            _b[1.post]
dumpb "c1.1.treat#1.post"   _b[1.treat#1.post]
dumpb "c1._cons"             _b[_cons]

* (2) c.x##c.x — x and x^2
regress y c.x##c.x
dumpb "c2.x"                 _b[x]
dumpb "c2.x#x"               _b[c.x#c.x]

* (3) i.g3##c.x — group main + continuous + group-specific slopes
regress y i.g3##c.x
dumpb "c3.2.g3"             _b[2.g3]
dumpb "c3.3.g3"             _b[3.g3]
dumpb "c3.x"                 _b[x]
dumpb "c3.2.g3#x"           _b[2.g3#c.x]
dumpb "c3.3.g3#x"           _b[3.g3#c.x]

* (4) i.treat#i.post — interaction ONLY (single (1,1) cell)
regress y i.treat#i.post
dumpb "c4.1.treat#1.post"  _b[1.treat#1.post]

* (5) i.g3##i.post — 3x2 fully populated
regress y i.g3##i.post
dumpb "c5.2.g3"             _b[2.g3]
dumpb "c5.3.g3"             _b[3.g3]
dumpb "c5.1.post"           _b[1.post]
dumpb "c5.2.g3#1.post"     _b[2.g3#1.post]
dumpb "c5.3.g3#1.post"     _b[3.g3#1.post]

file close fh
type "fvint_golden.csv"
