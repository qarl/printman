#pragma once
//
// Amplify a loaded USD cage: Catmull-Clark subdivide (carrying face-varying UV), then displace and
// shade per corner via the Shader. Per-corner vertices share positions at face edges (watertight
// slice); UV splits at authored seams. Displacement is per-vertex-normal so shared positions move
// together. (Memory-bounded per-band subdivision is a later refinement; this materializes the
// refined cage.)
//
#include <cmath>
#include <cstdint>
#include <vector>

#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/subdiv.hpp"
#include "printman/usd.hpp"

namespace printman {

inline Mesh amplify_usd(const UsdCage& uc, const Shader& sh, int level) {
    // build a proper cage (no creases/corners for now) carrying the face-varying UV
    subdiv::Cage c = subdiv::build_cage(uc.points, uc.counts, uc.indices,
                                        /*creaseIndices*/ {}, /*creaseLengths*/ {}, /*creaseSharp*/ {},
                                        /*cornerIndices*/ {}, /*cornerSharp*/ {},
                                        subdiv::BOUNDARY_EDGE_AND_CORNER, /*triangle_smooth*/ false, uc.st);
    subdiv::Cage r = subdiv::subdivide(c, uc.subdiv_scheme, level);

    // per-vertex normals (Newell per face, accumulated)
    std::vector<V3> vn(r.verts.size(), V3{{0, 0, 0}});
    for (int f = 0; f < r.nfaces(); ++f) {
        int a = r.foff[f], b = r.foff[f + 1];
        V3 n{{0, 0, 0}};
        for (int i = a; i < b; ++i) {
            const auto& p0 = r.verts[r.fvi[i]]; const auto& p1 = r.verts[r.fvi[(i + 1 < b) ? i + 1 : a]];
            n[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
            n[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
            n[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
        }
        for (int i = a; i < b; ++i) { vn[r.fvi[i]][0] += n[0]; vn[r.fvi[i]][1] += n[1]; vn[r.fvi[i]][2] += n[2]; }
    }
    for (auto& n : vn) { double l = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (l > 1e-12) { n[0]/=l; n[1]/=l; n[2]/=l; } }

    Mesh m; m.aov_names = sh.aov_names();
    const int na = int(m.aov_names.size()); m.aov.resize(na);
    std::vector<V3> av(na);
    const bool hasuv = r.has_uv();
    auto emit = [&](int corner) -> std::uint32_t {
        int vid = r.fvi[corner];
        const auto& P = r.verts[vid]; const V3& N = vn[vid];
        V3 Pv{{P[0], P[1], P[2]}};
        V2 uv = hasuv ? V2{{r.fvar[corner][0], r.fvar[corner][1]}} : V2{{0, 0}};
        double d = sh.displace(Pv, N, uv);
        sh.shade(Pv, N, uv, av.data());
        std::uint32_t id = (std::uint32_t)m.pos.size();
        m.pos.push_back({(float)(P[0] + d * N[0]), (float)(P[1] + d * N[1]), (float)(P[2] + d * N[2])});
        m.nrm.push_back({(float)N[0], (float)N[1], (float)N[2]});
        m.uv.push_back({(float)uv[0], (float)uv[1]});
        for (int k = 0; k < na; ++k) m.aov[k].push_back({(float)av[k][0], (float)av[k][1], (float)av[k][2]});
        return id;
    };
    for (int f = 0; f < r.nfaces(); ++f) {
        int a = r.foff[f], b = r.foff[f + 1];
        for (int i = a + 1; i + 1 < b; ++i) { auto v0 = emit(a); auto v1 = emit(i); auto v2 = emit(i + 1); m.tri.push_back({v0, v1, v2}); }
    }
    return m;  // world-space; the caller drops the assembled model to the plate once (see below)
}

// Concatenate amplified parts into one mesh. A multi-mesh model's parts each carry their own
// shader's AOVs; align by name on the first part's schema (missing channels -> 0) so at least
// Cout survives across parts. Triangle indices are rebased per part.
inline Mesh merge_meshes(const std::vector<Mesh>& parts) {
    Mesh out;
    for (const Mesh& p : parts) if (!p.aov_names.empty()) { out.aov_names = p.aov_names; break; }
    out.aov.resize(out.aov_names.size());
    for (const Mesh& p : parts) {
        std::uint32_t base = (std::uint32_t)out.pos.size();
        out.pos.insert(out.pos.end(), p.pos.begin(), p.pos.end());
        out.nrm.insert(out.nrm.end(), p.nrm.begin(), p.nrm.end());
        out.uv.insert(out.uv.end(), p.uv.begin(), p.uv.end());
        for (std::size_t k = 0; k < out.aov_names.size(); ++k) {
            int src = p.aov_index(out.aov_names[k]);
            if (src >= 0) out.aov[k].insert(out.aov[k].end(), p.aov[src].begin(), p.aov[src].end());
            else out.aov[k].resize(out.aov[k].size() + p.pos.size(), {0, 0, 0});
        }
        for (const auto& t : p.tri) out.tri.push_back({t[0] + base, t[1] + base, t[2] + base});
    }
    return out;
}

// Sit the model on the build plate (z=0). Done once, on the assembled model, so parts keep their
// relative heights (dropping each part independently would flatten them all onto the plate).
inline void drop_to_plate(Mesh& m) {
    float zmin = 1e30f;
    for (const auto& p : m.pos) zmin = std::min(zmin, p[2]);
    if (zmin < 1e30f) for (auto& p : m.pos) p[2] -= zmin;
}

}  // namespace printman
