// Adaptive Catmull-Clark on a NON-QUAD control cage. amplify_usd_adaptive used to handle all-quad
// cages only; a non-quad cage now gets one Catmull-Clark step first (making it all-quad without
// changing the limit surface), then the same adaptive machinery dices it. This checks that on a
// closed all-triangle octahedron: (A) band-count=1 == band-count=N to 1 micron (the invariance
// keystone survives the pre-subdivide), (B) the diced surface is watertight (a closed manifold has
// zero boundary edges -- a crack at the pre-split seam or an extraordinary vertex would show as odd
// edges), and (C) its slice extent matches the uniform reference path (the limit surface is the same).
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "printman/band_usd.hpp"
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/usd.hpp"

using namespace printman;
static long long rk(double v) { return (long long)std::llround(v / 1e-3); }   // 1 micron

// Octahedron: 6 vertices, 8 triangle faces (all non-quad), closed. CC-subdivides to a smooth blob.
static UsdCage octahedron() {
    UsdCage uc; uc.subdiv_scheme = "catmullClark";
    const double a = 15.0;
    uc.points = {{{a,0,0}}, {{-a,0,0}}, {{0,a,0}}, {{0,-a,0}}, {{0,0,a}}, {{0,0,-a}}};
    // 8 faces, each wound CCW as seen from outside.
    const int F[8][3] = {{0,2,4},{2,1,4},{1,3,4},{3,0,4}, {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    for (auto& f : F) { uc.counts.push_back(3); uc.indices.insert(uc.indices.end(), {f[0], f[1], f[2]}); }
    return uc;
}

int main() {
    int fails = 0;
    UsdCage oct = octahedron();
    FlatShader flat;                                   // reach 0 -> slice the CC limit surface itself
    std::vector<const Shader*> sh{&flat};
    std::vector<double> zs; for (double z = -14.5; z <= 14.5; z += 0.2) zs.push_back(z);
    const double tol = 0.4; const int level = 4;

    // (A) band-invariance across the pre-subdivide.
    auto A = amplify_usd_adaptive(oct, sh, level, tol, zs, 1);
    auto B = amplify_usd_adaptive(oct, sh, level, tol, zs, 12);
    long long mism = 0, tot = 0;
    for (size_t l = 0; l < zs.size(); ++l) {
        auto canon = [&](const LayerSegs& L) { std::multiset<std::array<long long, 4>> s;
            for (auto& g : L) s.insert({rk(g.a[0]), rk(g.a[1]), rk(g.b[0]), rk(g.b[1])}); return s; };
        auto sa = canon(A[l]), sb = canon(B[l]); tot += (long long)sa.size();
        if (sa != sb) mism += (long long)std::max(sa.size(), sb.size());
    }
    bool aok = (mism == 0) && (tot > 0);
    std::printf("(A) non-quad band-invariance: band1 vs band12 over %zu layers -- mismatch=%lld / %lld  %s\n",
                zs.size(), mism, tot, aok ? "OK" : "*** FAIL ***");
    fails += !aok;

    // (B) watertight: collect the whole diced mesh (single band) and count boundary (odd-use) edges.
    Mesh full;
    auto on_band = [&](const Mesh& bm) { std::uint32_t base = (std::uint32_t)full.pos.size();
        for (auto& p : bm.pos) full.pos.push_back(p);
        for (auto& t : bm.tri) full.tri.push_back({t[0]+base, t[1]+base, t[2]+base}); };
    amplify_usd_adaptive(oct, sh, level, tol, zs, 1, on_band, /*do_slice*/ false);
    std::map<std::array<long long, 6>, int> use;
    auto add = [&](std::uint32_t a, std::uint32_t b) {
        const auto& pa = full.pos[a]; const auto& pb = full.pos[b];
        std::array<long long,3> ka{rk(pa[0]),rk(pa[1]),rk(pa[2])}, kb{rk(pb[0]),rk(pb[1]),rk(pb[2])};
        if (ka == kb) return;                                   // degenerate (snapped) edge -- skip
        std::array<long long,6> k = ka < kb ? std::array<long long,6>{ka[0],ka[1],ka[2],kb[0],kb[1],kb[2]}
                                            : std::array<long long,6>{kb[0],kb[1],kb[2],ka[0],ka[1],ka[2]};
        use[k]++; };
    for (auto& t : full.tri) { add(t[0],t[1]); add(t[1],t[2]); add(t[2],t[0]); }
    long long odd = 0; for (auto& kv : use) if (kv.second % 2 == 1) ++odd;
    bool bok = (odd == 0) && !full.tri.empty();
    std::printf("(B) watertight (closed manifold): %zu tris, boundary(odd) edges=%lld  %s\n",
                full.tri.size(), odd, bok ? "OK crack-free" : "*** CRACK ***");
    fails += !bok;

    // (C) extent matches the uniform reference (both slice the same limit surface).
    auto U = amplify_usd_banded(oct, sh, level, zs, 1);
    auto bbox = [&](const std::vector<LayerSegs>& S, double& xr, double& yr) {
        double x0=1e30,x1=-1e30,y0=1e30,y1=-1e30;
        for (auto& L : S) for (auto& g : L) { for (double x : {g.a[0], g.b[0]}) { x0=std::min(x0,x); x1=std::max(x1,x); }
            for (double y : {g.a[1], g.b[1]}) { y0=std::min(y0,y); y1=std::max(y1,y); } }
        xr = x1-x0; yr = y1-y0; };
    double axr, ayr, uxr, uyr; bbox(A, axr, ayr); bbox(U, uxr, uyr);
    bool cok = uxr > 0 && uyr > 0 && std::fabs(axr-uxr) < 0.05*uxr && std::fabs(ayr-uyr) < 0.05*uyr;
    std::printf("(C) extent vs uniform: adaptive=%.2fx%.2f uniform=%.2fx%.2f  %s\n",
                axr, ayr, uxr, uyr, cok ? "OK matches" : "*** FAIL ***");
    fails += !cok;

    std::printf("%s\n", fails == 0 ? "PASS: non-quad adaptive CC is band-invariant, watertight, faithful" : "*** non-quad adaptive FAILED ***");
    return fails ? 1 : 0;
}
