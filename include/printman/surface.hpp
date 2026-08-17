#pragma once
//
// Dicing: turn a parametric cage into a micro-mesh and displace it. Displacement is PER-VERTEX
// along the vertex normal (a pure function of the vertex), so the result is band-independent by
// construction -- the band-seam averaging-domain bug simply cannot arise. (A per-face-average
// mode for strongly normal-dependent shaders would need the halo trick; not needed here.)
//
// This parametric sphere stands in for the real USD Catmull-Clark cage until the USD front-end
// lands; it exercises the whole amplification + slice + raster path with zero heavy deps.
//
#include <cmath>
#include <vector>

#include "printman/geom.hpp"
#include "printman/shader.hpp"

namespace printman {

// A UV-sphere of radius R, diced nu x nv, displaced by the shader, then dropped so it sits on the
// plate (min z == 0). Equirectangular UVs. Albedo baked per vertex.
inline Mesh dice_sphere(double R, int nu, int nv, const Shader& sh) {
    Mesh m;
    m.aov_names = sh.aov_names();
    const int na = int(m.aov_names.size());
    m.aov.resize(na);
    std::vector<V3> tmp(na);
    const double PI = 3.14159265358979323846;
    auto vid = [nu](int i, int j) { return j * (nu + 1) + i; };  // i in [0..nu], j in [0..nv]
    for (int j = 0; j <= nv; ++j) {
        double v = double(j) / nv;                 // 0 south .. 1 north
        double lat = (v - 0.5) * PI;               // -pi/2 .. pi/2
        for (int i = 0; i <= nu; ++i) {
            double u = double(i) / nu;             // 0 .. 1 longitude
            double lon = (u - 0.5) * 2 * PI;
            V3 n{{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)}};
            V3 base{{R * n[0], R * n[1], R * n[2]}};
            V2 uv{{u, v}};
            double d = sh.displace(base, n, uv);   // geometry
            V3 p{{base[0] + d * n[0], base[1] + d * n[1], base[2] + d * n[2]}};
            sh.shade(base, n, uv, tmp.data());     // all AOVs at this vertex
            m.pos.push_back({(float)p[0], (float)p[1], (float)p[2]});
            m.nrm.push_back({(float)n[0], (float)n[1], (float)n[2]});
            m.uv.push_back({(float)u, (float)v});
            for (int k = 0; k < na; ++k) m.aov[k].push_back({(float)tmp[k][0], (float)tmp[k][1], (float)tmp[k][2]});
        }
    }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            std::uint32_t a = vid(i, j), b = vid(i + 1, j), c = vid(i + 1, j + 1), dd = vid(i, j + 1);
            m.tri.push_back({a, b, c});
            m.tri.push_back({a, c, dd});
        }
    // drop to the plate
    float zmin = 1e30f;
    for (auto& p : m.pos) zmin = std::min(zmin, p[2]);
    for (auto& p : m.pos) p[2] -= zmin;
    return m;
}

inline void mesh_z_range(const Mesh& m, double& zlo, double& zhi) {
    zlo = 1e30; zhi = -1e30;
    for (auto& p : m.pos) { zlo = std::min(zlo, (double)p[2]); zhi = std::max(zhi, (double)p[2]); }
}

}  // namespace printman
