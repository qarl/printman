// Does the 1-ring halo keep adaptive CC band-invariant WITH a semi-sharp crease, at 1-micron slice
// precision (tighter than the pixel gate)? Build the curved creased wall in code, compare band=1 vs
// band=12 slice segments exactly. 0 mismatch => 1-ring suffices for creases; else halo-widening needed.
#include <cstdio>
#include <cmath>
#include <set>
#include <array>
#include <vector>
#include "printman/band_usd.hpp"
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/usd.hpp"
using namespace printman;
static long long rk(double v){ return (long long)std::llround(v/1e-3); }   // 1 micron

int main(){
    UsdCage uc; uc.subdiv_scheme="catmullClark";
    const int C=4, R=14; double cols[4]={0,10,20,30};
    auto vid=[&](int i,int j){ return j*C+i; };
    for(int j=0;j<R;++j) for(int i=0;i<C;++i){ double x=cols[i], y=8.0*std::sin(M_PI*x/30.0), z=j*5.0; uc.points.push_back({{x,y,z}}); }
    for(int j=0;j<R-1;++j) for(int i=0;i<C-1;++i){ uc.counts.push_back(4);
        uc.indices.insert(uc.indices.end(), {vid(i,j),vid(i+1,j),vid(i+1,j+1),vid(i,j+1)}); }
    const int midj=6;                                   // crease across the middle row, sharpness 4
    for(int i=0;i<C;++i) uc.crease_indices.push_back(vid(i,midj));
    uc.crease_lengths.push_back(C); uc.crease_sharpnesses.push_back(4.0);

    FlatShader flat;                  // reach 0: slice the CC limit surface itself
    std::vector<const Shader*> sh{&flat};
    std::vector<double> zs; for(double z=0.2; z<=65.0; z+=0.2) zs.push_back(z);
    const double tol=0.4; int level=6;

    auto A=amplify_usd_adaptive(uc, sh, level, tol, zs, 1);
    auto B=amplify_usd_adaptive(uc, sh, level, tol, zs, 12);
    long long mism=0, tot=0;
    for(size_t l=0;l<zs.size();++l){
        auto canon=[&](const LayerSegs&L){ std::multiset<std::array<long long,4>> s;
            for(auto&g:L) s.insert({rk(g.a[0]),rk(g.a[1]),rk(g.b[0]),rk(g.b[1])}); return s; };
        auto sa=canon(A[l]), sb=canon(B[l]); tot+=(long long)sa.size();
        if(sa!=sb) mism+=(long long)std::max(sa.size(),sb.size());
    }
    printf("creased wall (crease sharpness 4, 1-ring halo): band1 vs band12 over %zu layers -- %s (mismatch=%lld / %lld segs)\n",
           zs.size(), mism==0?"PASS: 1-ring suffices for creases":"*** FAIL: need halo widening ***", mism, tot);
    return mism==0?0:1;
}
