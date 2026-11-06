// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/oaxaca.h"
#include "core/error.h"
#include "core/linalg.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace cecon {
namespace {
struct Fit {
    std::vector<double> b, scores; // scores: full coefficient count x sample rows
    bool omitted = false;
};

Fit fit(const std::vector<double>& X, const std::vector<double>& y, int p, int intercept) {
    const int n = static_cast<int>(y.size());
    std::vector<double> gram(p*p), work, orig;
    la::xtx(X.data(), n, p, gram.data());
    work = gram;
    orig.resize(p);
    for (int j=0;j<p;++j) orig[j]=gram[j+j*p];
    std::vector<int> kept;
    std::vector<char> used(p,0);
    // Protect the intercept, then pivot on remaining diagonal size. This is
    // deliberately local: Oaxaca's Stata 9.2 regression branch need not share
    // modern regress's omission ordering. Duplicate/constant/empty-school
    // oracle fixtures pin this choice, including the later duplicate surviving.
    for (int step=0;step<p;++step) {
        int pivot=-1;
        if (step==0) pivot=intercept;
        else for (int j=0;j<p;++j) {
            if (used[j] || work[j+j*p] <= 1e-12*orig[j]) continue;
            if (pivot<0 || work[j+j*p] > work[pivot+pivot*p]*(1+1e-12)) pivot=j;
        }
        if (pivot<0 || work[pivot+pivot*p]<=0) break;
        kept.push_back(pivot); used[pivot]=1;
        const double d=work[pivot+pivot*p];
        for (int j=0;j<p;++j) if (!used[j])
            for (int i=0;i<p;++i) if (!used[i])
                work[i+j*p] -= work[i+pivot*p]*work[pivot+j*p]/d;
    }
    const int k=static_cast<int>(kept.size());
    if (n<=k) throw EngineError("no_obs","oaxaca: insufficient observations");
    std::vector<double> xr(n*k), bread(k*k), xy(k), b(k), e(n);
    for (int j=0;j<k;++j) {
        std::copy_n(X.data()+kept[j]*n,n,xr.data()+j*n);
        for (int i=0;i<k;++i) bread[i+j*k]=gram[kept[i]+kept[j]*p];
    }
    if (la::spd_inverse(bread.data(),k)) throw EngineError("collinear","oaxaca: singular design");
    la::xtv(xr.data(),n,k,y.data(),xy.data());
    la::matvec(bread.data(),k,xy.data(),b.data());
    la::residual(xr.data(),n,k,b.data(),y.data(),e.data());
    // bread * X' in coefficient-major rows: gemm avoids a scalar n*k*k loop.
    std::vector<double> influence(k*n);
    la::gemm_nt(bread.data(),xr.data(),influence.data(),k,k,n);
    Fit out;
    out.b.assign(p,0); out.scores.assign(p*n,0); out.omitted=k<p;
    for (int j=0;j<k;++j) {
        out.b[kept[j]]=b[j];
        for (int i=0;i<n;++i) out.scores[kept[j]+i*p]=influence[j+i*k]*e[i];
    }
    return out;
}
}

OaxacaResult oaxaca_fit(const DesignMatrix& dm,const std::vector<int>& group,bool relax) {
    const int n=static_cast<int>(dm.n),p=dm.k, m=2*p+4, q=5*p;
    std::array<std::vector<int>,2> rows;
    for (int i=0;i<n;++i) rows[group[i]].push_back(i);
    std::array<Fit,3> fits;
    std::vector<double> theta(q,0), C(q*q,0);
    OaxacaResult out;
    for (int g=0;g<2;++g) {
        const int ng=static_cast<int>(rows[g].size()); out.n[g]=ng;
        if (ng<2) throw EngineError("no_obs","oaxaca: insufficient observations in group");
        std::vector<double> x(ng*p), y(ng), centered(ng*p), cov(p*p);
        for (int i=0;i<ng;++i) y[i]=dm.y[rows[g][i]];
        for (int j=0;j<p;++j) {
            for (int i=0;i<ng;++i) x[i+j*ng]=dm.X[rows[g][i]+j*n];
            const double mean=la::sum(x.data()+j*ng,ng)/ng;
            theta[(3+g)*p+j]=mean;
            for (int i=0;i<ng;++i) centered[i+j*ng]=x[i+j*ng]-mean;
        }
        fits[g]=fit(x,y,p,p-1);
        std::copy(fits[g].b.begin(),fits[g].b.end(),theta.begin()+g*p);
        la::xtx(centered.data(),ng,p,cov.data());
        for (int j=0;j<p;++j) for (int i=0;i<p;++i)
            C[(3+g)*p+i+((3+g)*p+j)*q]=cov[i+j*p]/(double(ng)*(ng-1));
    }
    {
        std::vector<double> z=dm.X;
        z.resize(n*(p+1));
        for (int i=0;i<n;++i) z[n*p+i]=group[i];
        Fit fp=fit(z,dm.y,p+1,p-1);
        fits[2].b.assign(fp.b.begin(),fp.b.begin()+p);
        fits[2].scores.resize(n*p);
        fits[2].omitted=fp.omitted;
        for (int i=0;i<n;++i) std::copy_n(fp.scores.data()+i*(p+1),p,fits[2].scores.data()+i*p);
        std::copy(fits[2].b.begin(),fits[2].b.end(),theta.begin()+2*p);
    }
    const double factor=double(n)/(n-1);
    auto put=[&](int a,int b,const std::vector<double>& v) {
        for (int j=0;j<p;++j) for (int i=0;i<p;++i) {
            C[a*p+i+(b*p+j)*q]=v[i+j*p]*factor;
            C[b*p+j+(a*p+i)*q]=v[i+j*p]*factor;
        }
    };
    for (int a=0;a<3;++a) {
        const int na=a<2?out.n[a]:n;
        std::vector<double> cov(p*p);
        la::gemm_nt(fits[a].scores.data(),fits[a].scores.data(),cov.data(),p,na,p);
        put(a,a,cov);
        out.omitted |= fits[a].omitted;
        for (int j=0;j<p;++j) if (cov[j+j*p]==0) out.omitted=true;
        if (a<2) {
            std::vector<double> pooled(p*na);
            for (int i=0;i<na;++i) std::copy_n(fits[2].scores.data()+rows[a][i]*p,p,pooled.data()+i*p);
            la::gemm_nt(fits[a].scores.data(),pooled.data(),cov.data(),p,na,p);
            put(a,2,cov);
        }
    }
    if (out.omitted && !relax)
        throw EngineError("bad_input","oaxaca: dropped coefficients or zero variances; specify relax","r(499)");
    out.names={"overall:group_1","overall:group_2","overall:difference","overall:explained","overall:unexplained"};
    for(int j=0;j<p-1;++j) out.names.push_back("explained:"+dm.xnames[j]);
    for(int j=0;j<p;++j) out.names.push_back("unexplained:"+dm.xnames[j]);
    out.beta.assign(m,0);
    // Sparse delta rows. Means and coefficients are separate covariance blocks;
    // empirically stacking all their scores would estimate a different V.
    std::vector<std::vector<std::pair<int,double>>> J(m);
    for (int j=0;j<p;++j) {
        double b1=theta[j],b2=theta[p+j],br=theta[2*p+j],x1=theta[3*p+j],x2=theta[4*p+j];
        out.beta[0]+=x1*b1; out.beta[1]+=x2*b2;
        J[0].insert(J[0].end(),{{j,x1},{3*p+j,b1}});
        J[1].insert(J[1].end(),{{p+j,x2},{4*p+j,b2}});
        const int u=4+p+j;
        out.beta[u]=x1*(b1-br)+x2*(br-b2);
        J[u]={{j,x1},{p+j,-x2},{2*p+j,x2-x1},{3*p+j,b1-br},{4*p+j,br-b2}};
        out.beta[4]+=out.beta[u];
        J[4].insert(J[4].end(),J[u].begin(),J[u].end());
        if(j<p-1) {
            const int e=5+j;
            out.beta[e]=(x1-x2)*br; out.beta[3]+=out.beta[e];
            J[e]={{2*p+j,x1-x2},{3*p+j,br},{4*p+j,-br}};
            J[3].insert(J[3].end(),J[e].begin(),J[e].end());
        }
    }
    out.beta[2]=out.beta[0]-out.beta[1];
    J[2]=J[0]; for(auto [j,v]:J[1]) J[2].push_back({j,-v});
    std::vector<double> JC(m*q,0);
    for(int row=0;row<m;++row) for(auto [k,v]:J[row])
        for(int col=0;col<q;++col) JC[row+col*m]+=v*C[k+col*q];
    out.V.assign(m*m,0);
    for(int j=0;j<m;++j) for(int i=0;i<=j;++i) {
        double v=0;
        for(auto [k,d]:J[j]) v+=JC[i+k*m]*d;
        out.V[i+j*m]=out.V[j+i*m]=v;
    }
    return out;
}
}
