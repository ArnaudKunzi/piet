// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"
#include "estimators/oaxaca.h"
#include "core/distributions.h"
#include <set>

namespace cecon::cli {
int cmd_oaxaca(const std::vector<std::string>& a) {
    std::string data=opt(a,"--data"), y=opt(a,"--y"), by=opt(a,"--by");
    auto xs=split_csv(opt(a,"--x"));
    const bool pooled=has(a,"--pooled"),relax=has(a,"--relax");
    const std::string lev=opt(a,"--level");
    const double level=lev.empty()?95:std::stod(lev);
    if(data_missing(data)||y.empty()||by.empty()||!pooled||!(level>0&&level<100))
        throw EngineError("bad_input","oaxaca: requires --data --y --by --pooled; --level must be between 0 and 100");
    data=maybe_keepif(data,opt(a,"--keepif"));
    const Dataset& ds=acquire_data(data);
    const double* yy=require_num(ds,"oaxaca",y),*gg=require_num(ds,"oaxaca",by);
    std::vector<const double*> xx;
    std::set<std::string> unique;
    for(const auto& x:xs) {
        if(x==by) throw EngineError("bad_input","oaxaca: group variable in regressors is not supported");
        if(!unique.insert(x).second) throw EngineError("bad_input","oaxaca: duplicate regressor name");
        xx.push_back(require_num(ds,"oaxaca",x));
    }
    std::set<double> levels;
    std::vector<int64_t> rows;
    for(int64_t i=0;i<ds.n_rows();++i) {
        if(!std::isfinite(gg[i])) continue;
        levels.insert(gg[i]);
        if(!std::isfinite(yy[i])) continue;
        bool ok=true; for(auto col:xx) if(!std::isfinite(col[i])) {ok=false;break;}
        if(ok) rows.push_back(i);
    }
    if(levels.size()>2) throw EngineError("bad_input","oaxaca: more than 2 groups found","r(420)");
    if(levels.size()<2||rows.empty()) throw EngineError("no_obs","oaxaca: insufficient observations");
    const double g1=*levels.begin(),g2=*levels.rbegin();
    DesignMatrix dm; dm.n=rows.size();dm.k=xs.size()+1;dm.xnames=xs;dm.xnames.push_back("_cons");
    dm.X.resize(dm.n*dm.k);dm.y.resize(dm.n);
    std::vector<int> group(dm.n);
    for(int64_t i=0;i<dm.n;++i) {
        dm.y[i]=yy[rows[i]];group[i]=gg[rows[i]]==g1?0:1;
        for(size_t j=0;j<xs.size();++j) dm.X[i+j*dm.n]=xx[j][rows[i]];
        dm.X[i+(dm.k-1)*dm.n]=1;
    }
    auto r=oaxaca_fit(dm,group,relax);
    json o={{"estimator","oaxaca"},{"depvar",y},{"n_obs",dm.n},{"N_1",r.n[0]},{"N_2",r.n[1]},
            {"group_1",g1},{"group_2",g2},{"by",by},{"vcov_type","robust"},{"level",level}};
    json ct=json::array(), V=json::array();
    const double z=z_critical(1-level/100);
    int m=r.beta.size();
    for(int i=0;i<m;++i) {
        double se=std::sqrt(std::max(0.,r.V[i+i*m])),b=r.beta[i];
        json t=se>0?json(b/se):json(nullptr);
        json pv=se>0?json(std::erfc(std::fabs(b/se)/std::sqrt(2.))):json(nullptr);
        ct.push_back({{"name",r.names[i]},{"b",b},{"se",se},{"t",t},{"p",pv},{"ci_lo",b-z*se},{"ci_hi",b+z*se}});
        json row=json::array();for(int j=0;j<m;++j) row.push_back(r.V[i+j*m]);V.push_back(row);
    }
    o["coeftable"]=ct;o["e_b"]={{"names",r.names},{"values",r.beta}};o["e_V"]={{"names",r.names},{"matrix",V}};
    if(r.omitted)o["warnings"]={"oaxaca: dropped coefficients or zero variances retained under relax"};
    emit(o,opt(a,"--out"));return 0;
}
}
