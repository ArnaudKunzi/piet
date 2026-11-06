// Dump all cells for direct comparison with csdid_attgt.csv. Output: cohort-ordered.
#include <cstdio>
#include <vector>
#include "estimators/csdid.h"
#include "io/columnar.h"
using namespace cecon;
// Run from tests/validate (csdid_input.hbin is a generated, untracked fixture).
int main(){
    Dataset ds = Dataset::open("csdid_input.hbin");
    const double* h_pid = ds.col("h_pid").f64();
    const double* wv    = ds.col("wv").f64();
    const double* gvar  = ds.col("gvar").f64();
    const double* y     = ds.col("visits_w").f64();
    const double* female= ds.col("female").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi= ds.col("edu_hi").f64();
    int64_t n_obs = ds.n_rows();
    std::vector<double> X(static_cast<size_t>(n_obs) * 3);
    for (int64_t i=0; i<n_obs; ++i) { X[i]=female[i]; X[n_obs+i]=age_z[i]; X[2*n_obs+i]=edu_hi[i]; }
    CsdidConfig cfg;
    auto R = csdid_dripw_run(h_pid, wv, gvar, y, X.data(), 3, n_obs, cfg, -5, 5);
    std::printf("g,base,t,att,se,n_cell,ok\n");
    for (const auto& c : R.cells)
        std::printf("%g,%g,%g,%.12g,%.12g,%lld,%d\n",
                    c.g, c.base, c.t, c.att, c.se, (long long)c.n_cell, c.ok?1:0);
    return 0;
}
