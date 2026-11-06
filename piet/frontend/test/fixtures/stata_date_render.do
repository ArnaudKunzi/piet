* P1-A2-UX-DATE-RENDER — Stata render oracle for the browse-grid %t* renderer.
* Emits one line per case:  RENDER|<fmt>|<raw index>|<Stata display string>
* Truth source for piet/frontend/test/ DOM cases (frontend renders the RAW family
* index, A1 Q1=(b), so both halves of each line are needed).
clear all
set more off

capture program drop _r
program define _r
    args fmt val
    di "RENDER|`fmt'|`val'|" `fmt' `val'
end

* ── %td (days since 01jan1960) ──
_r %td 0
_r %td -1
_r %td `=td(29feb2020)'
_r %td `=td(15mar2020)'
_r %td `=td(31dec2020)'

* ── %tw (fixed-52-week years) ──
_r %tw 0
_r %tw -1
_r %tw `=tw(2019w52)'
_r %tw `=tw(2020w1)'
_r %tw `=tw(2020w2)'
_r %tw `=tw(2020w52)'

* ── %tm ──
_r %tm 0
_r %tm -1
_r %tm `=tm(2020m3)'
_r %tm `=tm(2020m12)'

* ── %tq ──
_r %tq 0
_r %tq -1
_r %tq `=tq(2020q1)'
_r %tq `=tq(2020q4)'

* ── %th ──
_r %th 0
_r %th -1
_r %th `=th(2020h1)'
_r %th `=th(2020h2)'

* ── %ty (value IS the year — must NOT be rescaled) ──
_r %ty 1960
_r %ty 2020

* ── %td detail-string tokens (FIX:GRID-TFMT-DETAIL — added by
*    an automated session): the family-default cases above
*    only exercise fmtStataDate(); these exercise the token map covering every
*    token this rider implements — CCYY/NN/DD/dd/Mon/Month/`_`. jan5/2020 (not
*    mar15) is deliberate for the dd/DD pair: a single-digit day is the only
*    value that can tell "05" (DD, zero-padded) apart from "5" (dd, not).
_r %tdCCYY-NN-DD `=td(15mar2020)'
_r %tdNN/DD/CCYY `=td(29feb2020)'
_r %tdMon_dd,_CCYY `=td(05jan2020)'
_r %tdDD_Month_CCYY `=td(05jan2020)'
_r %tdYY/NN/DD `=td(15mar2020)'
_r %tdCCYY-NN-DD -1

* ── %tc (P1-A2B — milliseconds since 01jan1960 00:00:00.000) ──
*    The six raw indices are the SAME datetime boundaries the writer's Q6
*    measurement recovered bit-exactly (tests/eval/golden/tc_datetime_fixture.dta:
*    0 / 500 / 1000 / -86400000 / 1893456930000 / 2082326400000), so the render
*    oracle and the write oracle pin one shared set of hard cases. The two
*    `…123`/`…999` variants are the full-precision forms from that same
*    measurement — they are what proves the default %tc layout TRUNCATES the
*    sub-second remainder rather than rounding it into the next second.
_r %tc 0
_r %tc 500
_r %tc 1000
_r %tc -86400000
_r %tc 1893456930000
_r %tc 2082326400000
_r %tc 1893456930123
_r %tc 2082326400999
* Mid-day time-of-day: without one of these every case above lands on a
* 00:00:0x clock, and an hour/minute field that is never exercised could be
* wrong in any way at all and still leave the suite green.
_r %tc `=tc(15mar2020 13:45:07)'

* ── `c` comma-grouping suffix (P1-A2B — UX-052 named residual ①) ──
*    Stata's `c` suffix on a %fmt inserts thousands separators. Both the g
*    (general) and f (fixed) branches carry it; the negative and the
*    below-1000 cases pin that the separator is placed by magnitude, not by
*    blindly chunking the digit string.
_r %12.0gc 1234567
_r %12.0gc -1234567
_r %12.0gc 999
_r %12.0gc 1000
_r %12.2fc 1234567.891
_r %12.2fc -9876.5

di "ORACLE_DONE"
exit, clear STATA
