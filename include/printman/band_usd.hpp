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
// `nbands` splits `zs` into that many disjoint layer ranges. `dice_scheme` is how the sub-cage is
// refined -- "catmullClark" for a subdivision surface, "bilinear" to dice a polygon mesh (like
// RenderMan) so displacement resolves on it. Returns per-layer slice segments.
// on_band, if set, is handed each band's mesh before it's freed (e.g. to render it). do_slice=false
// skips the slice (for a render-only pass at a different dicing level), returning empty layers.
inline std::vector<LayerSegs> amplify_usd_banded(const UsdCage& uc,
        const std::vector<const Shader*>& shaders, int level,
        const std::vector<double>& zs, int nbands,
        const std::function<void(const Mesh&)>& on_band = {}, bool do_slice = true,
        const std::string& dice_scheme = "catmullClark") {
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
            // Dice the sub-cage, carrying the tag; the core children lead the refined faces. Catmull-
            // Clark smooths a subdiv surface; bilinear dices a polygon mesh (adds micropolygon density,
            // preserves the flat shape) -- both emit one quad per corner, so the ncc count below holds.
            subdiv::Cage r = sub;
            for (int i = 0; i < level; ++i) { tag = child_face_tags(r, dice_scheme, tag); r = subdiv_step(r, dice_scheme); }
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

// Segments an edge of world length `len` needs to sit under tolerance `tol` (a power of two,
// clamped). Edge-intrinsic, so two faces sharing an edge always agree -> crack-free boundaries.
inline int edge_segments(double len, double tol) {
    if (tol <= 0 || len <= tol) return 1;
    int L = std::min(8, std::max(0, (int)std::ceil(std::log2(len / tol))));
    return 1 << L;
}

// ADAPTIVE bound-and-split for a quad polygon mesh (true REYES dicing): each control face dices to
// its OWN edge scale, not one global level, so a fine face beside a coarse panel is not over-diced.
// Crack-free by construction: each edge's segment count comes from the edge alone (shared faces
// agree), boundary samples snap to it (coincident snaps make harmless degenerate triangles, never
// hanging vertices), and per-control-vertex normals -- interpolated across the face -- match on
// shared edges so the DISPLACED boundary matches too. `tol` is the world facet target (bead width
// for the slice, ~1px for the render). Quads only; non-quad meshes use the uniform bilinear path.
inline std::vector<LayerSegs> amplify_poly_banded(const UsdCage& uc,
        const std::vector<const Shader*>& shaders, double tol,
        const std::vector<double>& zs, int nbands,
        const std::function<void(const Mesh&)>& on_band = {}, bool do_slice = true) {
    std::vector<LayerSegs> out(zs.size());
    const size_t nz = zs.size();
    if (nz == 0 || nbands < 1) return out;
    const int nf = int(uc.counts.size());
    std::vector<int> foff(nf + 1, 0);
    for (int f = 0; f < nf; ++f) foff[f + 1] = foff[f] + uc.counts[f];

    // Per control-vertex normal (Newell per face, accumulated) -- band-independent, and shared, so
    // interpolating it across faces gives matching normals on shared edges.
    const int nv = int(uc.points.size());
    std::vector<V3> vn(nv, V3{{0, 0, 0}});
    for (int f = 0; f < nf; ++f) {
        int a = foff[f], b = foff[f + 1]; V3 n{{0, 0, 0}};
        for (int i = a; i < b; ++i) {
            const auto& p0 = uc.points[uc.indices[i]]; const auto& p1 = uc.points[uc.indices[(i + 1 < b) ? i + 1 : a]];
            n[0] += (p0[1]-p1[1])*(p0[2]+p1[2]); n[1] += (p0[2]-p1[2])*(p0[0]+p1[0]); n[2] += (p0[0]-p1[0])*(p0[1]+p1[1]);
        }
        for (int i = a; i < b; ++i) { vn[uc.indices[i]][0]+=n[0]; vn[uc.indices[i]][1]+=n[1]; vn[uc.indices[i]][2]+=n[2]; }
    }
    for (auto& n : vn) { double l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (l>1e-12){n[0]/=l;n[1]/=l;n[2]/=l;} }

    double max_disp = 0; for (const Shader* s : shaders) max_disp = std::max(max_disp, s->max_reach());
    std::vector<double> flo(nf, 1e30), fhi(nf, -1e30);
    for (int f = 0; f < nf; ++f) for (int i = foff[f]; i < foff[f + 1]; ++i) { double z = uc.points[uc.indices[i]][2]; flo[f]=std::min(flo[f],z); fhi[f]=std::max(fhi[f],z); }
    const std::vector<int> ctag = uc.face_material.empty() ? std::vector<int>(nf, 0) : uc.face_material;
    const int M = int(shaders.size());
    const bool hasuv = uc.st.size() == uc.indices.size();

    for (int band = 0; band < nbands; ++band) {
        size_t g0 = (size_t)band*nz/nbands, g1 = (size_t)(band+1)*nz/nbands;
        if (g0 >= g1) continue;
        const double zlo = zs[g0], zhi = zs[g1-1];
        std::vector<Mesh> parts(M); std::vector<int> na(M); int maxna = 1;
        for (int m = 0; m < M; ++m) { parts[m].aov_names = shaders[m]->aov_names(); na[m]=int(parts[m].aov_names.size()); parts[m].aov.resize(na[m]); maxna=std::max(maxna,na[m]); }
        std::vector<V3> av(maxna);

        for (int f = 0; f < nf; ++f) {
            if (fhi[f] < zlo - max_disp || flo[f] > zhi + max_disp) continue;   // face not in this band
            if (foff[f+1] - foff[f] != 4) continue;                            // adaptive path is quads only
            int mi = (f < int(ctag.size())) ? ctag[f] : 0; if (mi < 0 || mi >= M) mi = 0;
            Mesh& mm = parts[mi]; const Shader& sh = *shaders[mi]; const int nak = na[mi];
            V3 P[4], Nc[4]; V2 UV[4];
            for (int c = 0; c < 4; ++c) { int vid = uc.indices[foff[f]+c];
                P[c] = {{uc.points[vid][0], uc.points[vid][1], uc.points[vid][2]}}; Nc[c] = vn[vid];
                UV[c] = hasuv ? V2{{uc.st[foff[f]+c][0], uc.st[foff[f]+c][1]}} : V2{{0, 0}}; }
            auto elen = [&](int a, int b){ double dx=P[a][0]-P[b][0],dy=P[a][1]-P[b][1],dz=P[a][2]-P[b][2]; return std::sqrt(dx*dx+dy*dy+dz*dz); };
            // edges: 0 bottom P0P1 (s), 1 right P1P2 (t), 2 top P3P2 (s), 3 left P0P3 (t)
            const int s0 = edge_segments(elen(0,1), tol), s2 = edge_segments(elen(3,2), tol);
            const int t1 = edge_segments(elen(1,2), tol), t3 = edge_segments(elen(0,3), tol);
            const int Ns = std::max(s0, s2), Nt = std::max(t1, t3);
            auto snap = [](double v, int seg){ return std::round(v*seg)/seg; };
            auto emit = [&](double s, double t) -> std::uint32_t {
                V3 a{{(1-s)*P[0][0]+s*P[1][0], (1-s)*P[0][1]+s*P[1][1], (1-s)*P[0][2]+s*P[1][2]}};
                V3 b{{(1-s)*P[3][0]+s*P[2][0], (1-s)*P[3][1]+s*P[2][1], (1-s)*P[3][2]+s*P[2][2]}};
                V3 p{{(1-t)*a[0]+t*b[0], (1-t)*a[1]+t*b[1], (1-t)*a[2]+t*b[2]}};
                V3 na0{{(1-s)*Nc[0][0]+s*Nc[1][0], (1-s)*Nc[0][1]+s*Nc[1][1], (1-s)*Nc[0][2]+s*Nc[1][2]}};
                V3 nb0{{(1-s)*Nc[3][0]+s*Nc[2][0], (1-s)*Nc[3][1]+s*Nc[2][1], (1-s)*Nc[3][2]+s*Nc[2][2]}};
                V3 n{{(1-t)*na0[0]+t*nb0[0], (1-t)*na0[1]+t*nb0[1], (1-t)*na0[2]+t*nb0[2]}};
                double nl=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (nl>1e-12){n[0]/=nl;n[1]/=nl;n[2]/=nl;}
                V2 ua{{(1-s)*UV[0][0]+s*UV[1][0], (1-s)*UV[0][1]+s*UV[1][1]}};
                V2 ub{{(1-s)*UV[3][0]+s*UV[2][0], (1-s)*UV[3][1]+s*UV[2][1]}};
                V2 uv{{(1-t)*ua[0]+t*ub[0], (1-t)*ua[1]+t*ub[1]}};
                double d = sh.displace(p, n, uv); sh.shade(p, n, uv, av.data());
                std::uint32_t id = (std::uint32_t)mm.pos.size();
                mm.pos.push_back({(float)(p[0]+d*n[0]), (float)(p[1]+d*n[1]), (float)(p[2]+d*n[2])});
                mm.nrm.push_back({(float)n[0], (float)n[1], (float)n[2]});
                mm.uv.push_back({(float)uv[0], (float)uv[1]});
                for (int k = 0; k < nak; ++k) mm.aov[k].push_back({(float)av[k][0], (float)av[k][1], (float)av[k][2]});
                return id;
            };
            std::vector<std::uint32_t> idx((size_t)(Ns+1)*(Nt+1));
            for (int j = 0; j <= Nt; ++j) for (int i = 0; i <= Ns; ++i) {
                double s = (double)i/Ns, t = (double)j/Nt;
                if (j == 0) s = snap(s, s0); else if (j == Nt) s = snap(s, s2);   // bottom/top edges -> their segment count
                if (i == 0) t = snap(t, t3); else if (i == Ns) t = snap(t, t1);   // left/right edges
                idx[(size_t)j*(Ns+1)+i] = emit(s, t);
            }
            for (int j = 0; j < Nt; ++j) for (int i = 0; i < Ns; ++i) {
                std::uint32_t a = idx[(size_t)j*(Ns+1)+i], b = idx[(size_t)j*(Ns+1)+i+1];
                std::uint32_t c = idx[(size_t)(j+1)*(Ns+1)+i+1], dd = idx[(size_t)(j+1)*(Ns+1)+i];
                mm.tri.push_back({a, b, c}); mm.tri.push_back({a, c, dd});
            }
        }
        Mesh bm = (M == 1) ? std::move(parts[0]) : merge_meshes(parts);
        if (do_slice) { std::vector<double> bandzs(zs.begin()+g0, zs.begin()+g1); auto segs = slice_mesh(bm, bandzs); for (size_t k=0;k<segs.size();++k) out[g0+k]=std::move(segs[k]); }
        if (on_band) on_band(bm);
    }
    return out;
}

// True if every face is a quad -- the adaptive polygon path handles quads only.
inline bool all_quads(const UsdCage& uc) {
    for (int c : uc.counts) if (c != 4) return false;
    return !uc.counts.empty();
}

}  // namespace printman
