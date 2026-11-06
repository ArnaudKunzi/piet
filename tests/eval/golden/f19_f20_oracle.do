* F19: +-0.0 collapse to one FE group
import delimited "f19.csv", clear
tab g
areg y x, absorb(g)
display "F19_coef_x=" _b[x]
display "F19_ngroups=" e(N_clust)
* F20a: 2-arg round
display "F20a_round=" round(2.567, 0.01)
* F20b: chained comparison
display "F20b_chain=" (1<2<3)
display "F20b_altparse=" ((1<2)<3)
exit, clear
