sysuse auto, clear
* MI-193a: FE-stored model (areg absorb rep78)
areg price mpg weight, absorb(rep78)
display "M193a_mpg=" %21.15e _b[mpg]
display "M193a_weight=" %21.15e _b[weight]
display "M193a_cons=" %21.15e _b[_cons]
* MI-193b: keep/drop factor coefplot (reg with i.foreign)
reg price mpg i.foreign
display "M193b_mpg=" %21.15e _b[mpg]
display "M193b_foreign1=" %21.15e _b[1.foreign]
display "M193b_cons=" %21.15e _b[_cons]
* MI-194: roundtrip pin: mean(price)/1000
quietly summarize price
display "M194_meanprice=" %21.15e r(mean)
display "M194_z=" %21.15e r(mean)/1000
exit, clear
