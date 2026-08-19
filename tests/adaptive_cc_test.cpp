// Full-function test for amplify_usd_adaptive: (1) band-count invariance -- band=1 slices IDENTICALLY
// to band=8 (the pixel-exact gate's criterion, here as a per-layer segment multiset match), and
// (2) crack-free -- the per-band mesh of a CLOSED box is watertight (0 odd edges). Uses a real
// displacement shader; the CC box is curved so normals genuinely vary (exercises the normal path).
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
#include "printman/subdiv.hpp"

using namespace printman;

struct WaveDisp : Shader {
    double A;
    WaveDisp(double a) : A(a) {}
    double displace(const V3& p, const V3&, const V2&) const override { return A * std::sin(0.3*p[0]) * std::cos(0.4*p[1]) * std::sin(0.35*p[2]); }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3&, const V3&, const V2&, V3* o) const override { o[0] = V3{{0.6,0.6,0.6}}; }
    double max_reach() const override { return std::fabs(A) + 1e-6; }
};

static long long rk(double v){ return (long long)std::llround(v/1e-3); }

int main() {
    UsdCage uc; uc.subdiv_scheme = "catmullClark";
    uc.points = {{0,0,0},{40,0,0},{40,8,0},{0,8,0},{0,0,8},{40,0,8},{40,8,8},{0,8,8}};
    uc.counts = {4,4,4,4,4,4};
    uc.indices = {0,3,2,1, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7};
    WaveDisp sh(2.0);
    std::vector<const Shader*> shaders{&sh};

    // layer range from the refined limit AABB + displacement margin
    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices, {},{},{},{},{}, subdiv::BOUNDARY_EDGE_AND_CORNER, false, {});
    subdiv::Pt lo, hi; subdiv::refined_limit_aabb(cage, "catmullClark", 6, lo, hi);
    double zlo = lo[2] - 2.5, zhi = hi[2] + 2.5, layer = 0.2;
    std::vector<double> zs; for (double z = zlo; z <= zhi; z += layer) zs.push_back(z);

    const double tol = 0.4;
    int level = subdiv::device_level(1.0, subdiv::max_control_edge(cage), tol, 0.0);
    printf("box CC: level=%d  layers=%zu  all_quads=%d\n", level, zs.size(), (int)all_quads(uc));

    // ---- (1) band invariance: band=1 vs band=8 ----
    auto A = amplify_usd_adaptive(uc, shaders, level, tol, zs, 1);
    auto B = amplify_usd_adaptive(uc, shaders, level, tol, zs, 8);
    long long mismatch = 0, total = 0;
    for (size_t l = 0; l < zs.size(); ++l) {
        auto canon = [](const LayerSegs& L){
            std::multiset<std::array<long long,4>> s;
            for (auto& g : L) s.insert({rk(g.a[0]),rk(g.a[1]),rk(g.b[0]),rk(g.b[1])});
            return s;
        };
        auto sa = canon(A[l]), sb = canon(B[l]);
        total += (long long)sa.size();
        if (sa != sb) { mismatch += (long long)std::max(sa.size(), sb.size()); }
    }
    printf("invariance: band1 vs band8 over %zu layers -- %s (mismatched segs=%lld / %lld)\n",
           zs.size(), mismatch==0 ? "PASS pixel-identical slices" : "*** FAIL ***", mismatch, total);

    // ---- (2) crack-free: collect the whole displaced mesh (band=1) and check watertight ----
    Mesh full;
    auto onb = [&](const Mesh& bm){ uint32_t base=(uint32_t)full.pos.size();
        for (auto& p : bm.pos) full.pos.push_back(p);
        for (auto& t : bm.tri) full.tri.push_back({t[0]+base,t[1]+base,t[2]+base}); };
    amplify_usd_adaptive(uc, shaders, level, tol, zs, 1, onb, /*do_slice*/false);
    std::map<std::array<long long,6>,int> use;
    auto add=[&](uint32_t x,uint32_t y){ const auto&pa=full.pos[x];const auto&pb=full.pos[y];
        long long ax=rk(pa[0]),ay=rk(pa[1]),az=rk(pa[2]),bx=rk(pb[0]),by=rk(pb[1]),bz=rk(pb[2]);
        if(ax==bx&&ay==by&&az==bz)return; std::array<long long,6> k;
        if(std::array<long long,3>{ax,ay,az}<std::array<long long,3>{bx,by,bz})k={ax,ay,az,bx,by,bz};else k={bx,by,bz,ax,ay,az};
        use[k]++; };
    for(auto&t:full.tri){ add(t[0],t[1]); add(t[1],t[2]); add(t[2],t[0]); }
    int odd=0; for(auto&kv:use) if(kv.second%2==1) odd++;
    // displacement actually applied? (some vertex moved off the z in [0,8] control range)
    double zmin=1e9,zmax=-1e9; for(auto&p:full.pos){ zmin=std::min(zmin,(double)p[2]); zmax=std::max(zmax,(double)p[2]); }
    printf("crack-free: tris=%zu odd_edges=%d (closed box => 0)  z-range=[%.2f,%.2f]  ->  %s\n",
           full.tri.size(), odd, zmin, zmax, odd==0 ? "PASS watertight" : "*** FAIL ***");
    return (mismatch==0 && odd==0) ? 0 : 1;
}
