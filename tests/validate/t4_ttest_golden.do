clear all
set more off
capture log close
cd /tmp
log using "/tmp/cobra_t4_ttest.log", replace text

sysuse auto, clear

file open out using "/tmp/cobra_t4_ttest.csv", write replace
file write out "key,value" _n

* Case 1: one-sample  ttest mpg == 21
ttest mpg == 21
file write out "c1.N_1"     "," %21.15e (r(N_1))    _n
file write out "c1.mu_1"    "," %21.15e (r(mu_1))   _n
file write out "c1.sd_1"    "," %21.15e (r(sd_1))   _n
file write out "c1.se"      "," %21.15e (r(se))     _n
file write out "c1.lb_1"    "," %21.15e (r(lb_1))   _n
file write out "c1.ub_1"    "," %21.15e (r(ub_1))   _n
file write out "c1.t"       "," %21.15e (r(t))      _n
file write out "c1.df_t"    "," %21.15e (r(df_t))   _n
file write out "c1.p_l"     "," %21.15e (r(p_l))    _n
file write out "c1.p"       "," %21.15e (r(p))      _n
file write out "c1.p_u"     "," %21.15e (r(p_u))    _n
file write out "c1.level"   "," %21.15e (r(level))  _n

* Case 2: two-sample by group, equal variances  (diff = mean(0)-mean(1))
ttest mpg, by(foreign)
file write out "c2.N_1"        "," %21.15e (r(N_1))         _n
file write out "c2.N_2"        "," %21.15e (r(N_2))         _n
file write out "c2.N_combined" "," %21.15e (r(N_combined))  _n
file write out "c2.mu_1"       "," %21.15e (r(mu_1))        _n
file write out "c2.mu_2"       "," %21.15e (r(mu_2))        _n
file write out "c2.mu_combined" "," %21.15e (r(mu_combined)) _n
file write out "c2.mu_diff"    "," %21.15e (r(mu_diff))     _n
file write out "c2.sd_1"       "," %21.15e (r(sd_1))        _n
file write out "c2.sd_2"       "," %21.15e (r(sd_2))        _n
file write out "c2.sd"         "," %21.15e (r(sd))          _n
file write out "c2.se"         "," %21.15e (r(se))          _n
file write out "c2.se_1"       "," %21.15e (r(se_1))        _n
file write out "c2.se_2"       "," %21.15e (r(se_2))        _n
file write out "c2.se_combined" "," %21.15e (r(se_combined)) _n
file write out "c2.lb_1"       "," %21.15e (r(lb_1))        _n
file write out "c2.ub_1"       "," %21.15e (r(ub_1))        _n
file write out "c2.lb_2"       "," %21.15e (r(lb_2))        _n
file write out "c2.ub_2"       "," %21.15e (r(ub_2))        _n
file write out "c2.lb_combined" "," %21.15e (r(lb_combined)) _n
file write out "c2.ub_combined" "," %21.15e (r(ub_combined)) _n
file write out "c2.lb_diff"    "," %21.15e (r(lb_diff))     _n
file write out "c2.ub_diff"    "," %21.15e (r(ub_diff))     _n
file write out "c2.t"          "," %21.15e (r(t))           _n
file write out "c2.df_t"       "," %21.15e (r(df_t))        _n
file write out "c2.p_l"        "," %21.15e (r(p_l))         _n
file write out "c2.p"          "," %21.15e (r(p))           _n
file write out "c2.p_u"        "," %21.15e (r(p_u))         _n
file write out "c2.level"      "," %21.15e (r(level))       _n

* Case 3: two-sample by group, UNEQUAL variances (Satterthwaite df)
ttest mpg, by(foreign) unequal
file write out "c3.N_1"        "," %21.15e (r(N_1))         _n
file write out "c3.N_2"        "," %21.15e (r(N_2))         _n
file write out "c3.mu_1"       "," %21.15e (r(mu_1))        _n
file write out "c3.mu_2"       "," %21.15e (r(mu_2))        _n
file write out "c3.mu_diff"    "," %21.15e (r(mu_diff))     _n
file write out "c3.sd_1"       "," %21.15e (r(sd_1))        _n
file write out "c3.sd_2"       "," %21.15e (r(sd_2))        _n
file write out "c3.se"         "," %21.15e (r(se))          _n
file write out "c3.se_1"       "," %21.15e (r(se_1))        _n
file write out "c3.se_2"       "," %21.15e (r(se_2))        _n
file write out "c3.lb_diff"    "," %21.15e (r(lb_diff))     _n
file write out "c3.ub_diff"    "," %21.15e (r(ub_diff))     _n
file write out "c3.t"          "," %21.15e (r(t))           _n
file write out "c3.df_t"       "," %21.15e (r(df_t))        _n
file write out "c3.p_l"        "," %21.15e (r(p_l))         _n
file write out "c3.p"          "," %21.15e (r(p))           _n
file write out "c3.p_u"        "," %21.15e (r(p_u))         _n
file write out "c3.level"      "," %21.15e (r(level))       _n

* Case 3b: Welch df variant
ttest mpg, by(foreign) welch
file write out "c3b.t"     "," %21.15e (r(t))      _n
file write out "c3b.df_t"  "," %21.15e (r(df_t))   _n
file write out "c3b.p"     "," %21.15e (r(p))      _n
file write out "c3b.p_l"   "," %21.15e (r(p_l))    _n
file write out "c3b.p_u"   "," %21.15e (r(p_u))    _n
file write out "c3b.lb_diff" "," %21.15e (r(lb_diff)) _n
file write out "c3b.ub_diff" "," %21.15e (r(ub_diff)) _n

* Case 4: paired  ttest price == weight
ttest price == weight
file write out "c4.N_1"      "," %21.15e (r(N_1))       _n
file write out "c4.N_2"      "," %21.15e (r(N_2))       _n
file write out "c4.mu_1"     "," %21.15e (r(mu_1))      _n
file write out "c4.mu_2"     "," %21.15e (r(mu_2))      _n
file write out "c4.mu_diff"  "," %21.15e (r(mu_diff))   _n
file write out "c4.sd_1"     "," %21.15e (r(sd_1))      _n
file write out "c4.sd_2"     "," %21.15e (r(sd_2))      _n
file write out "c4.sd_diff"  "," %21.15e (r(sd_diff))   _n
file write out "c4.se"       "," %21.15e (r(se))        _n
file write out "c4.se_1"     "," %21.15e (r(se_1))      _n
file write out "c4.se_2"     "," %21.15e (r(se_2))      _n
file write out "c4.lb_1"     "," %21.15e (r(lb_1))      _n
file write out "c4.ub_1"     "," %21.15e (r(ub_1))      _n
file write out "c4.lb_2"     "," %21.15e (r(lb_2))      _n
file write out "c4.ub_2"     "," %21.15e (r(ub_2))      _n
file write out "c4.lb_diff"  "," %21.15e (r(lb_diff))   _n
file write out "c4.ub_diff"  "," %21.15e (r(ub_diff))   _n
file write out "c4.t"        "," %21.15e (r(t))         _n
file write out "c4.df_t"     "," %21.15e (r(df_t))      _n
file write out "c4.p_l"      "," %21.15e (r(p_l))       _n
file write out "c4.p"        "," %21.15e (r(p))         _n
file write out "c4.p_u"      "," %21.15e (r(p_u))       _n
file write out "c4.level"    "," %21.15e (r(level))     _n

file close out
log close