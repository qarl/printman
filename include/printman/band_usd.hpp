#pragma once
//
// Full REYES-for-slicing on a USD Catmull-Clark cage: never materialize the whole refined surface.
// Per Z-band, refine + displace + shade ONLY the control faces whose displaced 1-ring hull overlaps
// the band (via subdiv::refine_region's localized subdivision), slice that band's layers, discard.
// One band of geometry lives at a time -- the memory bound.
//
// Band-count invariance: a control face's children are bit-identical to a whole-cage refinement
// (a CC patch is exactly supported by its 1-ring, which refine_region always includes as halo), and
// normals are taken over the halo-INCLUSIVE refined region, so a core vertex gets its whole-cage
// normal regardless of which band it lands in. Displacement is therefore a band-independent function
// of the vertex => band-count=1 and band-count=N slice pixel-identically (the USD gate proves it).
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "printman/amplify.hpp"   // child_face_tags, subdiv_step, merge_meshes
#include "printman/geom.hpp"
#include "printman/shader.hpp"
#include "printman/slicer.hpp"
#include "printman/subdiv.hpp"
#include "printman/usd.hpp"

namespace printman {

// `shaders` is one per uc.materials entry; `zs` are the (ascending) layer heights in the cage frame;
// `nbands` splits `zs` into that many disjoint layer ranges. Returns per-layer slice segments.
// on_band, if set, is handed each band's mesh before it's freed (e.g. to render it). do_slice=false
// skips the slice (for a render-only pass at a different dicing level), returning empty layers.
inline std::vector<LayerSegs> amplify_usd_banded(const UsdCage& uc,
        const std::vector<const Shader*>& shaders, int level,
        const std::vector<double>& zs, int nbands,
        const std::function<void(const Mesh&)>& on_band = {}, bool do_slice = true) {
    std::vector<LayerSegs> out(zs.size());
    const size_t nz = zs.size();
    if (nz == 0 || nbands < 1) return out;

    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices, {}, {}, {}, {}, {},
                                           subdiv::BOUNDARY_EDGE_AND_CORNER, false, uc.st);
    const auto vf = subdiv::vertex_faces(cage);
    const int nf = cage.nfaces();

    // Per control face: world-Z bound over its 1-ring control vertices (points are already Z-up).
    // A CC limit patch of face f is contained in this hull, so displaced it lies within ±max_reach.
    std::vector<double> flo(nf, 1e30), fhi(nf, -1e30);
    for (int f = 0; f < nf; ++f)
        for (int k = cage.foff[f]; k < cage.foff[f + 1]; ++k)
            for (int nfc : vf[cage.fvi[k]])
                for (int kk = cage.foff[nfc]; kk < cage.foff[nfc + 1]; ++kk) {
                    double z = cage.verts[cage.fvi[kk]][2];
                    flo[f] = std::min(flo[f], z); fhi[f] = std::max(fhi[f], z);
                }

    double max_disp = 0;
    for (const Shader* s : shaders) max_disp = std::max(max_disp, s->max_reach());
    const std::vector<int> ctag = uc.face_material.empty() ? std::vector<int>(nf, 0) : uc.face_material;
    const int M = int(shaders.size());

    for (int b = 0; b < nbands; ++b) {
        size_t g0 = (size_t)b * nz / nbands, g1 = (size_t)(b + 1) * nz / nbands;
        if (g0 >= g1) continue;
        const double zlo = zs[g0], zhi = zs[g1 - 1];

        // core = control faces whose displaced 1-ring hull overlaps this band's layer span
        std::vector<int> core;
        for (int f = 0; f < nf; ++f)
            if (fhi[f] >= zlo - max_disp && flo[f] <= zhi + max_disp) core.push_back(f);
        if (!core.empty()) {
            const int ncore = int(core.size());
            subdiv::Cage sub = subdiv::build_region_subcage(cage, vf, core);
            // material tag per sub-cage face: core faces first carry their control face's material
            std::vector<int> tag(sub.nfaces(), 0);
            for (int j = 0; j < ncore; ++j) tag[j] = (core[j] < int(ctag.size())) ? ctag[core[j]] : 0;
            // subdivide the sub-cage, carrying the tag; the core children lead the refined faces
            subdiv::Cage r = sub;
            for (int i = 0; i < level; ++i) { tag = child_face_tags(r, "catmullClark", tag); r = subdiv_step(r, "catmullClark"); }
            long long ncc = (level < 1) ? ncore : (long long)sub.foff[ncore];
            for (int l = 1; l < level; ++l) ncc *= 4;
            ncc = std::min<long long>(ncc, r.nfaces());

            // per-vertex normals over the FULL refined region (core + halo) -> band-independent for core
            std::vector<V3> vn(r.verts.size(), V3{{0, 0, 0}});
            for (int f = 0; f < r.nfaces(); ++f) {
                int a = r.foff[f], e = r.foff[f + 1];
                V3 n{{0, 0, 0}};
                for (int i = a; i < e; ++i) {
                    const auto& p0 = r.verts[r.fvi[i]]; const auto& p1 = r.verts[r.fvi[(i + 1 < e) ? i + 1 : a]];
                    n[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
                    n[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
                    n[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
                }
                for (int i = a; i < e; ++i) { vn[r.fvi[i]][0] += n[0]; vn[r.fvi[i]][1] += n[1]; vn[r.fvi[i]][2] += n[2]; }
            }
            for (auto& n : vn) { double L = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (L > 1e-12) { n[0]/=L; n[1]/=L; n[2]/=L; } }

            // emit only the core children, displaced + shaded per corner by that face's material shader
            std::vector<Mesh> parts(M);
            std::vector<int> na(M); int maxna = 1;
            for (int m = 0; m < M; ++m) { parts[m].aov_names = shaders[m]->aov_names(); na[m] = int(parts[m].aov_names.size()); parts[m].aov.resize(na[m]); maxna = std::max(maxna, na[m]); }
            std::vector<V3> av(maxna);
            const bool hasuv = r.has_uv();
            auto emit = [&](Mesh& mm, const Shader& sh, int nak, int corner) -> std::uint32_t {
                int vid = r.fvi[corner]; const auto& P = r.verts[vid]; const V3& N = vn[vid];
                V3 Pv{{P[0], P[1], P[2]}};
                V2 uv = hasuv ? V2{{r.fvar[corner][0], r.fvar[corner][1]}} : V2{{0, 0}};
                double d = sh.displace(Pv, N, uv);
                sh.shade(Pv, N, uv, av.data());
                std::uint32_t id = (std::uint32_t)mm.pos.size();
                mm.pos.push_back({(float)(P[0] + d * N[0]), (float)(P[1] + d * N[1]), (float)(P[2] + d * N[2])});
                mm.nrm.push_back({(float)N[0], (float)N[1], (float)N[2]});
                mm.uv.push_back({(float)uv[0], (float)uv[1]});
                for (int k = 0; k < nak; ++k) mm.aov[k].push_back({(float)av[k][0], (float)av[k][1], (float)av[k][2]});
                return id;
            };
            for (long long f = 0; f < ncc; ++f) {
                int mi = (f < (long long)tag.size()) ? tag[f] : 0; if (mi < 0 || mi >= M) mi = 0;
                Mesh& mm = parts[mi]; const Shader& sh = *shaders[mi];
                int a = r.foff[f], e = r.foff[f + 1];
                for (int i = a + 1; i + 1 < e; ++i) { auto v0 = emit(mm, sh, na[mi], a); auto v1 = emit(mm, sh, na[mi], i); auto v2 = emit(mm, sh, na[mi], i + 1); mm.tri.push_back({v0, v1, v2}); }
            }
            Mesh bm = (M == 1) ? std::move(parts[0]) : merge_meshes(parts);

            if (do_slice) {
                std::vector<double> bandzs(zs.begin() + g0, zs.begin() + g1);
                auto segs = slice_mesh(bm, bandzs);
                for (size_t k = 0; k < segs.size(); ++k) out[g0 + k] = std::move(segs[k]);
            }
            if (on_band) on_band(bm);   // e.g. render this band into a shared frame before it's freed
        }
        // band geometry (sub, r, vn, parts, bm, segs) is freed here -- the memory bound
    }
    return out;
}

// Band count for a target thickness ~ the widest control-face 1-ring Z-span, clamped [16, 256]
// (Engine.cpp's band_layers_for), then ceil(nlayers / band_layers).
inline int usd_band_count(const UsdCage& uc, const std::vector<double>& zs) {
    if (zs.size() < 2) return 1;
    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices, {}, {}, {}, {}, {},
                                           subdiv::BOUNDARY_EDGE_AND_CORNER, false, uc.st);
    const auto vf = subdiv::vertex_faces(cage);
    double max_span = 0;
    for (int f = 0; f < cage.nfaces(); ++f) {
        double lo = 1e30, hi = -1e30;
        for (int k = cage.foff[f]; k < cage.foff[f + 1]; ++k)
            for (int nfc : vf[cage.fvi[k]])
                for (int kk = cage.foff[nfc]; kk < cage.foff[nfc + 1]; ++kk) {
                    double z = cage.verts[cage.fvi[kk]][2]; lo = std::min(lo, z); hi = std::max(hi, z);
                }
        max_span = std::max(max_span, hi - lo);
    }
    const double layer_h = zs[1] - zs[0];
    long long band_layers = layer_h > 0 ? (long long)std::ceil(max_span / layer_h) : 16;
    band_layers = std::min<long long>(256, std::max<long long>(16, band_layers));
    return int((zs.size() + band_layers - 1) / band_layers);
}

}  // namespace printman
