#pragma once
//
// The core's own triangle -> Z-plane slicer. No libslic3r, no external clipper. Each triangle
// crossing a layer plane contributes one oriented segment, carrying every AOV interpolated to the
// two crossing points. Segments follow the triangle winding order so a downstream NON-ZERO
// winding fill treats self-intersecting folds as solid union, not floating islands.
//
#include <algorithm>
#include <array>
#include <vector>

#include "printman/geom.hpp"

namespace printman {

struct Seg {
    V2 a, b;                 // endpoints in the XY plane (mm)
    std::vector<V3> va, vb;  // per-AOV values at a, b (same order as Mesh::aov_names)
};

using LayerSegs = std::vector<Seg>;

inline std::vector<LayerSegs> slice_mesh(const Mesh& m, const std::vector<double>& zs) {
    const int na = int(m.aov.size());
    std::vector<LayerSegs> out(zs.size());
    for (const auto& t : m.tri) {
        const std::array<float, 3>* vp[3] = {&m.pos[t[0]], &m.pos[t[1]], &m.pos[t[2]]};
        double tzmin = std::min({(*vp[0])[2], (*vp[1])[2], (*vp[2])[2]});
        double tzmax = std::max({(*vp[0])[2], (*vp[1])[2], (*vp[2])[2]});
        for (size_t li = 0; li < zs.size(); ++li) {
            double Z = zs[li];
            if (Z < tzmin || Z > tzmax) continue;
            V2 pt[2]; std::vector<V3> col[2]; int nc = 0;
            for (int e = 0; e < 3 && nc < 2; ++e) {
                const auto& u = *vp[e]; const auto& v = *vp[(e + 1) % 3];
                double zu = u[2], zv = v[2];
                if (!((zu <= Z && zv > Z) || (zv <= Z && zu > Z))) continue;
                double tt = (Z - zu) / (zv - zu);
                pt[nc] = {{u[0] + (v[0] - u[0]) * tt, u[1] + (v[1] - u[1]) * tt}};
                col[nc].resize(na);
                for (int k = 0; k < na; ++k) {
                    const auto& cu = m.aov[k][t[e]]; const auto& cv = m.aov[k][t[(e + 1) % 3]];
                    col[nc][k] = {{cu[0] + (cv[0] - cu[0]) * (float)tt,
                                   cu[1] + (cv[1] - cu[1]) * (float)tt,
                                   cu[2] + (cv[2] - cu[2]) * (float)tt}};
                }
                ++nc;
            }
            if (nc == 2) out[li].push_back(Seg{pt[0], pt[1], std::move(col[0]), std::move(col[1])});
        }
    }
    return out;
}

}  // namespace printman
