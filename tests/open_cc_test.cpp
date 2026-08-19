// Adaptive CC on an OPEN all-quad cage (the box was closed). A flat non-uniform strip under CC stays
// planar with creased (preserved) boundaries, so displacement along +Z preserves xy and I can check
// crack-free by the polygon criterion: odd-count (boundary) edges trace the outline exactly (perim).
// Also re-check band invariance. Exercises boundary control edges + the boundary interpolation rules.
#include <cstdio>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <array>
#include "printman/band_usd.hpp"
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/usd.hpp"

using namespace printman;

struct Wave : Shader {
    double displace(const V3& p, const V3&, const V2&) const override { return 2.0*std::sin(0.5*p[0])*std::cos(0.4*p[1]); }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3&, const V3&, const V2&, V3* o) const override { o[0]=V3{{0.6,0.6,0.6}}; }
    double max_reach() const override { return 2.0+1e-6; }
};
static long long rk(double v){ return (long long)std::llround(v/1e-3); }

int main() {
    // Open strip: 3 quads widths 2/18/20, 20mm tall, z=0. Shared tall (20mm) edges of very different
    // neighbour scale. Domain [0,40]x[0,20], perimeter 120.
    UsdCage uc; uc.subdiv_scheme = "catmullClark";
    uc.points = {{0,0,0},{2,0,0},{20,0,0},{40,0,0},{0,20,0},{2,20,0},{20,20,0},{40,20,0}};
    uc.counts = {4,4,4};
    uc.indices = {0,1,5,4, 1,2,6,5, 2,3,7,6};
    Wave sh; std::vector<const Shader*> shaders{&sh};

    std::vector<double> zs; for (double z=-2.4; z<=2.4; z+=0.2) zs.push_back(z);
    const double tol = 0.4;
    // global level from longest control edge (the 20mm or 18mm side)
    int level = subdiv::device_level(1.0, 20.0, tol, 0.0);
    printf("open strip CC: level=%d layers=%zu all_quads=%d\n", level, zs.size(), (int)all_quads(uc));

    auto A = amplify_usd_adaptive(uc, shaders, level, tol, zs, 1);
    auto B = amplify_usd_adaptive(uc, shaders, level, tol, zs, 8);
    long long mis=0; for(size_t l=0;l<zs.size();++l){ auto canon=[](const LayerSegs&L){ std::multiset<std::array<long long,4>> s; for(auto&g:L)s.insert({rk(g.a[0]),rk(g.a[1]),rk(g.b[0]),rk(g.b[1])}); return s; };
        if(canon(A[l])!=canon(B[l])) mis++; }
    printf("invariance: band1 vs band8 -- %s (mismatched layers=%lld)\n", mis==0?"PASS":"*** FAIL ***", mis);

    Mesh full; auto onb=[&](const Mesh&bm){ uint32_t base=(uint32_t)full.pos.size(); for(auto&p:bm.pos)full.pos.push_back(p); for(auto&t:bm.tri)full.tri.push_back({t[0]+base,t[1]+base,t[2]+base}); };
    amplify_usd_adaptive(uc, shaders, level, tol, zs, 1, onb, false);
    std::map<std::array<long long,6>,int> use; std::map<std::array<long long,6>,double> len;
    auto add=[&](uint32_t x,uint32_t y){ const auto&pa=full.pos[x];const auto&pb=full.pos[y];
        long long ax=rk(pa[0]),ay=rk(pa[1]),az=rk(pa[2]),bx=rk(pb[0]),by=rk(pb[1]),bz=rk(pb[2]);
        if(ax==bx&&ay==by&&az==bz)return; std::array<long long,6> k;
        if(std::array<long long,3>{ax,ay,az}<std::array<long long,3>{bx,by,bz})k={ax,ay,az,bx,by,bz};else k={bx,by,bz,ax,ay,az};
        use[k]++; len[k]=std::hypot(pa[0]-pb[0],pa[1]-pb[1]); };
    for(auto&t:full.tri){add(t[0],t[1]);add(t[1],t[2]);add(t[2],t[0]);}
    double blen=0; for(auto&kv:use) if(kv.second%2==1) blen+=len[kv.first];
    double perim=120.0, excess=blen-perim;
    printf("crack-free: tris=%zu boundary_len=%.2f (perim=%.2f, excess=%.2f) -> %s\n",
           full.tri.size(), blen, perim, excess, std::fabs(excess)<0.05*perim+0.5?"PASS crack-free":"*** CRACK ***");
    return (mis==0 && std::fabs(excess) < 0.05*perim + 0.5) ? 0 : 1;
}
