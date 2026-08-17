#pragma once
//
// The REYES band loop: bound-and-split the surface into Z-bands, and for each band dice ONLY the
// cells whose displaced extent overlaps it, slice, emit, discard. One band of geometry lives at a
// time -- the memory bound. Cell vertices are recomputed per band (deterministic), never stored
// whole, so the surface is never materialized.
//
// Because displacement here is per-vertex (a pure function of the cell corner), the result is
// band-count-invariant by construction; the gate (band-count=1 == band-count=N) proves the
// bound-split selects each contributing cell into exactly the right band.
//
#include <algorithm>
#include <cmath>
#include <vector>

#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/slicer.hpp"

namespace printman {

// A parametric sphere cage evaluated on demand (stand-in until the USD Catmull-Clark cage lands).
struct SphereCage {
    double R; int nu, nv; const Shader* sh; std::vector<std::string> aov_names; double zoff = 0, top = 0;

    SphereCage(double R_, int nu_, int nv_, const Shader& s)
        : R(R_), nu(nu_), nv(nv_), sh(&s), aov_names(s.aov_names()) {
        double zmin = 1e30, zmax = -1e30;
        for (int j = 0; j <= nv; ++j)
            for (int i = 0; i <= nu; ++i) {
                V3 p, n; V2 uv; vert_raw(i, j, p, n, uv, nullptr);
                zmin = std::min(zmin, p[2]); zmax = std::max(zmax, p[2]);
            }
        zoff = zmin; top = zmax - zmin;  // drop to plate; printed height
    }
    void dir(int i, int j, V3& n, V2& uv) const {
        const double PI = 3.14159265358979323846;
        double u = double(i) / nu, v = double(j) / nv;
        double lat = (v - 0.5) * PI, lon = (u - 0.5) * 2 * PI;
        n = {{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)}};
        uv = {{u, v}};
    }
    void vert_raw(int i, int j, V3& p, V3& n, V2& uv, V3* aov) const {
        dir(i, j, n, uv);
        V3 base{{R * n[0], R * n[1], R * n[2]}};
        double d = sh->displace(base, n, uv);
        p = {{base[0] + d * n[0], base[1] + d * n[1], base[2] + d * n[2]}};
        if (aov) sh->shade(base, n, uv, aov);
    }
    void vert(int i, int j, V3& p, V3& n, V2& uv, V3* aov) const { vert_raw(i, j, p, n, uv, aov); p[2] -= zoff; }
};

// Bound-and-split into `nbands` Z-bands; return per-layer segments for `zs` (ascending).
inline std::vector<LayerSegs> amplify_banded(const SphereCage& c, const std::vector<double>& zs, int nbands) {
    const int na = int(c.aov_names.size());
    std::vector<LayerSegs> out(zs.size());
    const size_t nz = zs.size();
    if (nz == 0) return out;
    for (int b = 0; b < nbands; ++b) {
        size_t g0 = (size_t)b * nz / nbands, g1 = (size_t)(b + 1) * nz / nbands;
        if (g0 >= g1) continue;
        double zlo = zs[g0], zhi = zs[g1 - 1];
        Mesh bm; bm.aov_names = c.aov_names; bm.aov.resize(na);
        std::vector<V3> av(na);
        for (int j = 0; j < c.nv; ++j)
            for (int i = 0; i < c.nu; ++i) {
                // four cell corners
                V3 P[4], N[4]; V2 UV[4]; std::vector<std::array<float, 3>> A[4];
                int ii[4] = {i, i + 1, i + 1, i}, jj[4] = {j, j, j + 1, j + 1};
                double czmin = 1e30, czmax = -1e30;
                for (int k = 0; k < 4; ++k) {
                    c.vert(ii[k], jj[k], P[k], N[k], UV[k], av.data());
                    czmin = std::min(czmin, P[k][2]); czmax = std::max(czmax, P[k][2]);
                    A[k].resize(na);
                    for (int a = 0; a < na; ++a) A[k][a] = {(float)av[a][0], (float)av[a][1], (float)av[a][2]};
                }
                if (czmax < zlo || czmin > zhi) continue;  // bound-split: no overlap with this band
                std::uint32_t base = (std::uint32_t)bm.pos.size();
                for (int k = 0; k < 4; ++k) {
                    bm.pos.push_back({(float)P[k][0], (float)P[k][1], (float)P[k][2]});
                    for (int a = 0; a < na; ++a) bm.aov[a].push_back(A[k][a]);
                }
                bm.tri.push_back({base + 0, base + 1, base + 2});
                bm.tri.push_back({base + 0, base + 2, base + 3});
            }
        std::vector<double> bandzs(zs.begin() + g0, zs.begin() + g1);
        auto segs = slice_mesh(bm, bandzs);
        for (size_t k = 0; k < segs.size(); ++k) out[g0 + k] = std::move(segs[k]);
    }
    return out;
}

}  // namespace printman
