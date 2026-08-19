// Watertightness / crack-free test for amplify_poly_banded's triangle + n-gon (center-split) path.
// Flat meshes displaced along +Z by a spatially-varying height => xy preserved. In a crack-free
// tessellation the used-once (boundary) edges trace the domain outline exactly once, so their total
// xy-length == the domain perimeter. A segment-count mismatch on a shared edge puts vertices at
// different positions => interior unmatched edges => total exceeds the perimeter. No OSL/USD needed.
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "printman/band_usd.hpp"
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/usd.hpp"

using namespace printman;

// d = A*sin(kx*x)*cos(ky*y): varies along every shared edge, so a mismatch would show as a 3D gap.
struct WaveDisp : Shader {
    double A, kx, ky;
    WaveDisp(double a, double x, double y) : A(a), kx(x), ky(y) {}
    double displace(const V3& p, const V3&, const V2&) const override { return A * std::sin(kx * p[0]) * std::cos(ky * p[1]); }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3&, const V3&, const V2&, V3* out) const override { out[0] = V3{{0.7, 0.7, 0.7}}; }
    double max_reach() const override { return std::fabs(A) + 1e-6; }
};

static long long rkey(double v) { return (long long)std::llround(v / 1e-3); }  // 1-micron grid

// Returns {perimeter-worth of boundary edges found, max triangle-uses of any edge}. Prints a verdict.
static bool check(const char* name, const UsdCage& uc, const Shader& sh, double perim) {
    std::vector<double> zs;
    for (int i = 0; i < 200; ++i) zs.push_back(-3.0 + i * 6.0 / 199.0);  // layers spanning the disp range
    std::vector<const Shader*> shaders{&sh};

    // Collect the full diced mesh via a single band (every face emitted exactly once).
    Mesh full;
    auto on_band = [&](const Mesh& bm) {
        std::uint32_t base = (std::uint32_t)full.pos.size();
        for (auto& p : bm.pos) full.pos.push_back(p);
        for (auto& t : bm.tri) full.tri.push_back({t[0] + base, t[1] + base, t[2] + base});
    };
    amplify_poly_banded(uc, shaders, 0.4, zs, 1, on_band, /*do_slice*/ false);

    // Undirected edge usage, keyed by rounded 3D endpoints; skip degenerate (zero-length) edges.
    std::map<std::array<long long, 6>, int> use;
    std::map<std::array<long long, 6>, double> len2;  // xy length of the edge
    auto add = [&](std::uint32_t a, std::uint32_t b) {
        const auto& pa = full.pos[a]; const auto& pb = full.pos[b];
        long long ax = rkey(pa[0]), ay = rkey(pa[1]), az = rkey(pa[2]);
        long long bx = rkey(pb[0]), by = rkey(pb[1]), bz = rkey(pb[2]);
        if (ax == bx && ay == by && az == bz) return;  // degenerate from boundary snapping -- ignore
        std::array<long long, 6> k;
        if (std::array<long long, 3>{ax, ay, az} < std::array<long long, 3>{bx, by, bz}) k = {ax, ay, az, bx, by, bz};
        else k = {bx, by, bz, ax, ay, az};
        use[k]++;
        double dx = pa[0] - pb[0], dy = pa[1] - pb[1];
        len2[k] = std::sqrt(dx * dx + dy * dy);
    };
    for (auto& t : full.tri) { add(t[0], t[1]); add(t[1], t[2]); add(t[2], t[0]); }

    // Parity is what matters: a matched (interior) edge is used an EVEN number of times -- 2, or 4
    // when a zero-area degenerate triangle from snapping stacks on it. An ODD count is a true
    // boundary edge; an ODD count in the interior is an unmatched edge == a crack. So the boundary
    // (odd-count) edges must trace the outline exactly once (total length == perimeter).
    int odd = 0, even2 = 0, even4plus = 0;
    double blen = 0;
    for (auto& kv : use) {
        if (kv.second % 2 == 1) { odd++; blen += len2[kv.first]; }
        else if (kv.second == 2) even2++;
        else even4plus++;
    }
    double excess = blen - perim;
    bool ok = std::fabs(excess) < 0.05 * perim + 0.5;   // odd edges trace exactly the outline
    std::printf("%-22s tris=%-7zu matched(2)=%-6d degen-stacked(>=4)=%-4d odd/boundary=%-5d  boundary_len=%.2f (perim=%.2f, excess=%.2f)  %s\n",
                name, full.tri.size(), even2, even4plus, odd, blen, perim, excess, ok ? "OK crack-free" : "*** CRACK ***");
    return ok;
}

int main() {
    WaveDisp wave(3.0, 0.7, 0.5);
    int fails = 0;

    // 1) Single triangle (center-split into 3 corner-quads). Domain = the triangle; perim = 3 edges.
    {
        UsdCage uc; uc.subdiv_scheme = "none";
        uc.points = {{{0, 0, 0}}, {{30, 0, 0}}, {{12, 20, 0}}};
        uc.counts = {3}; uc.indices = {0, 1, 2};
        double p = 0; V3 v[3] = {uc.points[0], uc.points[1], uc.points[2]};
        for (int i = 0; i < 3; ++i) { auto& a = v[i]; auto& b = v[(i + 1) % 3]; p += std::hypot(a[0] - b[0], a[1] - b[1]); }
        fails += !check("triangle", uc, wave, p);
    }

    // 2) Single pentagon (n-gon center-split into 5 corner-quads). perim = 5 edges.
    {
        UsdCage uc; uc.subdiv_scheme = "none";
        for (int i = 0; i < 5; ++i) { double a = 2 * M_PI * i / 5; uc.points.push_back({{20 + 15 * std::cos(a), 15 + 15 * std::sin(a), 0}}); }
        uc.counts = {5}; uc.indices = {0, 1, 2, 3, 4};
        double p = 0; for (int i = 0; i < 5; ++i) { auto& a = uc.points[i]; auto& b = uc.points[(i + 1) % 5]; p += std::hypot(a[0] - b[0], a[1] - b[1]); }
        fails += !check("pentagon", uc, wave, p);
    }

    // 3) MIXED quad + two triangles sharing tall edges of very different neighbour scale.
    //    Domain rectangle [0,40]x[0,20], perim = 2*(40+20)=120.
    //    Faces: thin quad [v0,v1,v4,v3] (2mm wide, 20 tall) | triangles [v1,v2,v5] and [v1,v5,v4].
    //    A non-quad present => split=true => the quad center-splits too; the shared 20mm edge v1-v4
    //    is halved identically by quad and triangle. Crack-free is the whole point of this case.
    {
        UsdCage uc; uc.subdiv_scheme = "none";
        uc.points = {{{0, 0, 0}}, {{2, 0, 0}}, {{40, 0, 0}}, {{0, 20, 0}}, {{2, 20, 0}}, {{40, 20, 0}}};
        uc.counts = {4, 3, 3};
        uc.indices = {0, 1, 4, 3,   1, 2, 5,   1, 5, 4};
        fails += !check("mixed quad+tri", uc, wave, 120.0);
    }

    // 4) Regression: a pure all-quad strip (direct path, split=false), 2/18/20mm widths, tall 20mm
    //    shared edges. Domain [0,40]x[0,20]? widths sum 2+18+20=40. perim=120.
    {
        UsdCage uc; uc.subdiv_scheme = "none";
        uc.points = {{{0, 0, 0}}, {{2, 0, 0}}, {{20, 0, 0}}, {{40, 0, 0}},
                     {{0, 20, 0}}, {{2, 20, 0}}, {{20, 20, 0}}, {{40, 20, 0}}};
        uc.counts = {4, 4, 4};
        uc.indices = {0, 1, 5, 4,   1, 2, 6, 5,   2, 3, 7, 6};
        fails += !check("quad strip (direct)", uc, wave, 120.0);
    }
    return fails;
}
