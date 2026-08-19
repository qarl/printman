// Per-face-average displacement on the polygon path (Karl's rule): a polygon mesh keeps its facets,
// so the shader is evaluated per micro-triangle with that triangle's FLAT normal and the displacement
// VECTORS are averaged at each welded vertex. Two things must hold:
//   (1) DAMPING -- at a hard edge the two faces' normals diverge, so the averaged vector shrinks; the
//       ridge of a tent displaces LESS than a face interior. (The old smoothed-unit-normal path moved
//       every vertex by the full amount, so this is the behavioural signature of the new rule.)
//   (2) BAND-INVARIANCE -- the average at a vertex needs all its incident faces, so a band pulls in
//       the 1-ring of its sliced faces; band-count=1 must equal band-count=N to 1-micron. The
//       accordion stacks short tilted panels so a band holds some faces while their neighbours are
//       out of the z +/- max_disp window -- exactly the case the 1-ring halo exists to cover.
// No OSL/USD needed: cages are built in code and the shader displaces a constant along the normal.
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include "printman/band_usd.hpp"
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/usd.hpp"

using namespace printman;

// Constant outward displacement, so a vertex's move IS its per-face-averaged normal * d -- makes the
// damping directly measurable (interior |move| == d; a hard edge shrinks it).
struct ConstDisp : Shader {
    double d;
    explicit ConstDisp(double d_) : d(d_) {}
    double displace(const V3&, const V3&, const V2&) const override { return d; }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3&, const V3&, const V2&, V3* o) const override { o[0] = V3{{0.6, 0.6, 0.6}}; }
    double max_reach() const override { return std::fabs(d); }
};

static long long rk(double v) { return (long long)std::llround(v / 1e-3); }  // 1 micron

// Collect the single-band welded+displaced mesh (do_slice=false, nbands=1). Dicing is shader-
// independent, so d=0 and d=const yield the same vertex order -> per-vertex displacement by index.
static Mesh full_mesh(const UsdCage& uc, const Shader& sh, const std::vector<double>& zs) {
    Mesh out;
    std::vector<const Shader*> shaders{&sh};
    auto grab = [&](const Mesh& bm) { out = bm; };
    amplify_poly_banded(uc, shaders, 0.4, zs, 1, grab, /*do_slice*/ false);
    return out;
}

// Index of the vertex whose position is closest to target.
static size_t nearest(const Mesh& m, const V3& t) {
    size_t best = 0; double bd = 1e30;
    for (size_t v = 0; v < m.pos.size(); ++v) {
        double dx = m.pos[v][0]-t[0], dy = m.pos[v][1]-t[1], dz = m.pos[v][2]-t[2];
        double d = dx*dx+dy*dy+dz*dz; if (d < bd) { bd = d; best = v; }
    }
    return best;
}

// band=1 vs band=N slices, compared exactly at 1-micron. Returns mismatching segment count.
static long long band_mismatch(const UsdCage& uc, const Shader& sh, const std::vector<double>& zs, int N) {
    std::vector<const Shader*> shaders{&sh};
    auto A = amplify_poly_banded(uc, shaders, 0.4, zs, 1);
    auto B = amplify_poly_banded(uc, shaders, 0.4, zs, N);
    long long mism = 0;
    for (size_t l = 0; l < zs.size(); ++l) {
        auto canon = [&](const LayerSegs& L) { std::multiset<std::array<long long, 4>> s;
            for (auto& g : L) s.insert({rk(g.a[0]), rk(g.a[1]), rk(g.b[0]), rk(g.b[1])}); return s; };
        auto sa = canon(A[l]), sb = canon(B[l]);
        if (sa != sb) mism += (long long)std::max(sa.size(), sb.size());
    }
    return mism;
}

int main() {
    int fails = 0;

    // -- Case A: TENT (all-quad, two 45-degree slopes meeting at a hard ridge x=0,z=10). ----------
    // Left slope normal (-1,0,1)/sqrt2, right (1,0,1)/sqrt2 -> 90 deg apart, so the ridge's averaged
    // move is |nL+nR|/2 = cos(45) = 0.707 of a face interior's. That gap is the whole rule.
    UsdCage tent; tent.subdiv_scheme = "none";
    tent.points = {{{-10,0,0}}, {{0,0,10}}, {{10,0,0}}, {{-10,20,0}}, {{0,20,10}}, {{10,20,0}}};
    tent.counts = {4, 4};
    tent.indices = {0,1,4,3,   1,2,5,4};                     // left quad | right quad, CCW from above
    {
        std::vector<double> zs; for (double z = 0.1; z < 10.5; z += 0.2) zs.push_back(z);
        ConstDisp d0(0.0), d1(1.0);
        Mesh U = full_mesh(tent, d0, zs);                    // undisplaced (d=0)
        Mesh D = full_mesh(tent, d1, zs);                    // displaced by 1 along per-face-average normal
        bool same_order = U.pos.size() == D.pos.size();
        auto move = [&](const V3& target) {
            size_t v = nearest(U, target);
            double dx = D.pos[v][0]-U.pos[v][0], dy = D.pos[v][1]-U.pos[v][1], dz = D.pos[v][2]-U.pos[v][2];
            return std::sqrt(dx*dx+dy*dy+dz*dz);
        };
        double m_int = move(V3{{-5, 10, 5}});                // interior of the left slope
        double m_ridge = move(V3{{0, 10, 10}});              // the hard ridge (shared by both slopes)
        bool damp = same_order && m_int > 0.9 && m_ridge < 0.85 * m_int && m_ridge > 0.6 * m_int;
        std::printf("tent damping: interior|move|=%.3f  ridge|move|=%.3f  (expect ~1.0 vs ~0.707)  %s\n",
                    m_int, m_ridge, damp ? "OK per-face-average damps the hard edge" : "*** FAIL ***");
        fails += !damp;

        long long mism = band_mismatch(tent, d1, zs, 8);
        std::printf("tent band-invariance: band1 vs band8 -- mismatch=%lld  %s\n",
                    mism, mism == 0 ? "OK" : "*** FAIL ***");
        fails += (mism != 0);
    }

    // -- Case B: ACCORDION (short tilted panels stacked in z, tiny displacement). ------------------
    // Each panel spans only 4mm in z; with max_disp=0.05 a thin band holds a panel while its
    // neighbour two rows away is outside the z-window. Only the 1-ring halo completes the shared-edge
    // averages, so band1 == bandN proves the halo is right.
    {
        UsdCage acc; acc.subdiv_scheme = "none";
        const int R = 6; const double H = 4.0, D = 12.0;
        for (int r = 0; r < R; ++r) { double x = (r % 2) ? 6.0 : 0.0, z = r * H;
            acc.points.push_back({{x, 0, z}}); acc.points.push_back({{x, D, z}}); }
        for (int r = 0; r < R - 1; ++r) {                    // panel between row r and r+1
            int a = 2*r, b = 2*r+1, c = 2*(r+1)+1, d = 2*(r+1);
            acc.counts.push_back(4); acc.indices.insert(acc.indices.end(), {a, b, c, d});
        }
        ConstDisp dd(0.05);
        std::vector<double> zs; for (double z = 0.1; z < (R-1)*H; z += 0.2) zs.push_back(z);
        long long mism = band_mismatch(acc, dd, zs, 10);
        std::printf("accordion band-invariance (1-ring halo): band1 vs band10 over %zu layers -- mismatch=%lld  %s\n",
                    zs.size(), mism, mism == 0 ? "OK: 1-ring halo suffices" : "*** FAIL: halo incomplete ***");
        fails += (mism != 0);
    }

    std::printf("%s\n", fails == 0 ? "PASS: per-face-average damps + stays band-invariant" : "*** per-face-average FAILED ***");
    return fails ? 1 : 0;
}
