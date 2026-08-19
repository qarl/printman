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
#include <cassert>
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

    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices,
                                           uc.crease_indices, uc.crease_lengths, uc.crease_sharpnesses,
                                           uc.corner_indices, uc.corner_sharpnesses,
                                           uc.boundary, uc.triangle_smooth, uc.st);
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

// ADAPTIVE Catmull-Clark: dice each control face to its OWN edge scale, not one global level. Refine
// the band's region uniformly to the global level `level` (so a core face's refined vertices are
// shared/bit-identical across bands -- the invariance gate stays pixel-exact), then emit a COARSER
// per-face grid whose four sides each carry that control edge's own device_level. A shared control
// edge gives both faces the same level and the same lattice nodes, so the seam matches (crack-free),
// while a large flat face emits far fewer micropolygons. The (2^level+1)^2 vertex lattice is read off
// a TEMPLATE (a single parametric quad refined once: fvar refines to an exact dyadic (i,j), and the
// quad-child ordering is universal), so no per-band parametric pass is needed. ALL-QUAD control cages
// only -- a cage with any non-quad control face uses the uniform amplify_usd_banded path, since
// crack-free needs one dicing discipline across the whole mesh.
inline std::vector<LayerSegs> amplify_usd_adaptive(const UsdCage& uc,
        const std::vector<const Shader*>& shaders, int level, double tol,
        const std::vector<double>& zs, int nbands,
        const std::function<void(const Mesh&)>& on_band = {}, bool do_slice = true) {
    std::vector<LayerSegs> out(zs.size());
    const size_t nz = zs.size();
    if (nz == 0 || nbands < 1) return out;
    const int Lg = std::max(level, 0);
    const int S = 1 << Lg;                       // segments per side at the global level

    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices,
                                           uc.crease_indices, uc.crease_lengths, uc.crease_sharpnesses,
                                           uc.corner_indices, uc.corner_sharpnesses,
                                           uc.boundary, uc.triangle_smooth, uc.st);
    const auto vf = subdiv::vertex_faces(cage);
    const int nf = cage.nfaces();

    // TEMPLATE: refine one parametric quad to `level`; tmpl[m*4+c] = lattice node index (j*(S+1)+i)
    // of leaf m's corner c. Every quad's leaf block shares this ordering, so the map is reusable.
    std::vector<int> tmpl((size_t)S * S * 4);
    {
        subdiv::Cage tq;
        tq.verts = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}}; tq.fvi = {0,1,2,3}; tq.foff = {0,4};
        tq.corner_sharp.assign(4, 0.0); tq.boundary = subdiv::BOUNDARY_EDGE_AND_CORNER;
        tq.fvar = {{0,0},{1,0},{1,1},{0,1}};
        subdiv::Cage tr = subdiv::subdivide(tq, "catmullClark", Lg);
        for (int m = 0; m < S * S; ++m)
            for (int k = tr.foff[m], c = 0; k < tr.foff[m + 1]; ++k, ++c) {
                int i = (int)std::llround(tr.fvar[k][0] * S), j = (int)std::llround(tr.fvar[k][1] * S);
                tmpl[(size_t)m * 4 + c] = j * (S + 1) + i;
            }
#ifndef NDEBUG
        // The template must cover every (i,j) lattice node (surjective) -- otherwise latflat keeps a
        // stale/-1 entry and emit() reads a wrong vertex. Fails loudly if subdivide's child/corner
        // ordering ever stops being a pure function of local corner order (the reused-block premise).
        std::vector<char> hit((size_t)(S + 1) * (S + 1), 0);
        for (int x : tmpl) { assert(x >= 0 && x < (int)hit.size()); hit[x] = 1; }
        for (char h : hit) assert(h && "template lattice does not tile every node");
#endif
    }

    // per-control-face z-hull over its 1-ring (same band bound as the uniform path)
    std::vector<double> flo(nf, 1e30), fhi(nf, -1e30);
    for (int f = 0; f < nf; ++f)
        for (int k = cage.foff[f]; k < cage.foff[f + 1]; ++k)
            for (int nfc : vf[cage.fvi[k]])
                for (int kk = cage.foff[nfc]; kk < cage.foff[nfc + 1]; ++kk) {
                    double z = cage.verts[cage.fvi[kk]][2];
                    flo[f] = std::min(flo[f], z); fhi[f] = std::max(fhi[f], z);
                }

    double max_disp = 0; for (const Shader* s : shaders) max_disp = std::max(max_disp, s->max_reach());
    const std::vector<int> ctag = uc.face_material.empty() ? std::vector<int>(nf, 0) : uc.face_material;
    const int M = int(shaders.size());
    // a control edge's segment count -- edge-intrinsic (world length only), so neighbours agree.
    auto side_seg = [&](int f, int c0, int c1) -> int {
        const auto& a = cage.verts[cage.fvi[cage.foff[f] + c0]]; const auto& b = cage.verts[cage.fvi[cage.foff[f] + c1]];
        double d = std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
        return 1 << std::min(Lg, subdiv::device_level(1.0, d, tol, 0.0));
    };
    auto snap = [](double v, int seg){ return std::round(v * seg) / seg; };
    std::vector<int> latflat((size_t)(S + 1) * (S + 1), -1);   // (i,j) -> corner index in r; reused per face

    for (int b = 0; b < nbands; ++b) {
        size_t g0 = (size_t)b * nz / nbands, g1 = (size_t)(b + 1) * nz / nbands;
        if (g0 >= g1) continue;
        const double zlo = zs[g0], zhi = zs[g1 - 1];
        std::vector<int> core;
        for (int f = 0; f < nf; ++f)
            if (fhi[f] >= zlo - max_disp && flo[f] <= zhi + max_disp) core.push_back(f);
        if (core.empty()) continue;
        const int ncore = int(core.size());

        subdiv::Cage sub = subdiv::build_region_subcage(cage, vf, core);
        subdiv::Cage r = subdiv::subdivide(sub, "catmullClark", Lg);   // geometry + real UV, core leads

        // halo-inclusive per-vertex normals over the full refined region (band-independent for core)
        std::vector<V3> vn(r.verts.size(), V3{{0, 0, 0}});
        for (int f = 0; f < r.nfaces(); ++f) {
            int a = r.foff[f], e = r.foff[f + 1]; V3 n{{0, 0, 0}};
            for (int i = a; i < e; ++i) { const auto& p0 = r.verts[r.fvi[i]]; const auto& p1 = r.verts[r.fvi[(i + 1 < e) ? i + 1 : a]];
                n[0] += (p0[1]-p1[1])*(p0[2]+p1[2]); n[1] += (p0[2]-p1[2])*(p0[0]+p1[0]); n[2] += (p0[0]-p1[0])*(p0[1]+p1[1]); }
            for (int i = a; i < e; ++i) { vn[r.fvi[i]][0]+=n[0]; vn[r.fvi[i]][1]+=n[1]; vn[r.fvi[i]][2]+=n[2]; }
        }
        for (auto& n : vn) { double l = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (l > 1e-12) { n[0]/=l; n[1]/=l; n[2]/=l; } }

        std::vector<Mesh> parts(M); std::vector<int> na(M); int maxna = 1;
        for (int m = 0; m < M; ++m) { parts[m].aov_names = shaders[m]->aov_names(); na[m] = int(parts[m].aov_names.size()); parts[m].aov.resize(na[m]); maxna = std::max(maxna, na[m]); }
        std::vector<V3> av(maxna);
        const bool hasuv = r.has_uv();

        for (int fl = 0; fl < ncore; ++fl) {
            const int orig = core[fl];
            int mi = (orig < int(ctag.size())) ? ctag[orig] : 0; if (mi < 0 || mi >= M) mi = 0;
            Mesh& mm = parts[mi]; const Shader& sh = *shaders[mi]; const int nak = na[mi];
            // reconstruct this face's lattice: leaf block [fl*S^2, (fl+1)*S^2), corner c -> tmpl node.
            // 64-bit leaf index: at level 8 a face is 65536 leaves, so fl*S*S overflows int past ~32k faces.
            const long long lo = (long long)fl * S * S;
            for (long long m = 0; m < (long long)S * S; ++m) { const long long g = lo + m;
                assert(r.foff[g + 1] - r.foff[g] == 4 && "adaptive CC needs quad leaves (all-quad cage)");
                for (int k = r.foff[g], c = 0; k < r.foff[g + 1]; ++k, ++c) latflat[tmpl[(size_t)m * 4 + c]] = k;
            }
            const int ns0 = side_seg(orig, 0, 1), ns2 = side_seg(orig, 3, 2);   // bottom, top (s dir)
            const int nt1 = side_seg(orig, 1, 2), nt3 = side_seg(orig, 0, 3);   // right, left (t dir)
            const int Ns = std::max(ns0, ns2), Nt = std::max(nt1, nt3);
            auto emit = [&](double s, double t) -> std::uint32_t {
                int li = (int)std::llround(s * S), lj = (int)std::llround(t * S);
                int corner = latflat[(size_t)lj * (S + 1) + li];
                assert(corner >= 0 && "lattice node not populated -- template/coverage broke");
                int vid = r.fvi[corner]; const auto& P = r.verts[vid]; const V3& N = vn[vid];
                V3 Pv{{P[0], P[1], P[2]}};
                V2 uv = hasuv ? V2{{r.fvar[corner][0], r.fvar[corner][1]}} : V2{{0, 0}};
                double d = sh.displace(Pv, N, uv); sh.shade(Pv, N, uv, av.data());
                std::uint32_t id = (std::uint32_t)mm.pos.size();
                mm.pos.push_back({(float)(P[0]+d*N[0]), (float)(P[1]+d*N[1]), (float)(P[2]+d*N[2])});
                mm.nrm.push_back({(float)N[0], (float)N[1], (float)N[2]});
                mm.uv.push_back({(float)uv[0], (float)uv[1]});
                for (int k = 0; k < nak; ++k) mm.aov[k].push_back({(float)av[k][0], (float)av[k][1], (float)av[k][2]});
                return id;
            };
            std::vector<std::uint32_t> idx((size_t)(Ns + 1) * (Nt + 1));
            for (int j = 0; j <= Nt; ++j) for (int i = 0; i <= Ns; ++i) {
                double s = (double)i/Ns, t = (double)j/Nt;
                if (j == 0) s = snap(s, ns0); else if (j == Nt) s = snap(s, ns2);
                if (i == 0) t = snap(t, nt3); else if (i == Ns) t = snap(t, nt1);
                idx[(size_t)j*(Ns+1)+i] = emit(s, t);
            }
            for (int j = 0; j < Nt; ++j) for (int i = 0; i < Ns; ++i) {
                std::uint32_t v00 = idx[(size_t)j*(Ns+1)+i], v10 = idx[(size_t)j*(Ns+1)+i+1];
                std::uint32_t v11 = idx[(size_t)(j+1)*(Ns+1)+i+1], v01 = idx[(size_t)(j+1)*(Ns+1)+i];
                mm.tri.push_back({v00, v10, v11}); mm.tri.push_back({v00, v11, v01});
            }
        }
        Mesh bm = (M == 1) ? std::move(parts[0]) : merge_meshes(parts);
        if (do_slice) { std::vector<double> bandzs(zs.begin()+g0, zs.begin()+g1); auto segs = slice_mesh(bm, bandzs); for (size_t k=0;k<segs.size();++k) out[g0+k]=std::move(segs[k]); }
        if (on_band) on_band(bm);
    }
    return out;
}

// Estimate of the micropolygon-triangle count amplify_usd_adaptive emits at (level, tol) -- one
// per-face grid whose sides carry each control edge's own clamped device_level (for the CLI stat).
inline size_t usd_adaptive_tri_estimate(const UsdCage& uc, int level, double tol) {
    const int Lg = std::max(level, 0);
    const int nf = int(uc.counts.size());
    std::vector<int> foff(nf + 1, 0);
    for (int f = 0; f < nf; ++f) foff[f + 1] = foff[f] + uc.counts[f];
    auto seg = [&](int f, int c0, int c1) -> int {
        const auto& a = uc.points[uc.indices[foff[f] + c0]]; const auto& b = uc.points[uc.indices[foff[f] + c1]];
        double d = std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
        return 1 << std::min(Lg, subdiv::device_level(1.0, d, tol, 0.0));
    };
    size_t tris = 0;
    for (int f = 0; f < nf; ++f) {
        if (foff[f + 1] - foff[f] != 4) continue;
        int Ns = std::max(seg(f, 0, 1), seg(f, 3, 2)), Nt = std::max(seg(f, 1, 2), seg(f, 0, 3));
        tris += (size_t)Ns * Nt * 2;
    }
    return tris;
}

// Band count for a target thickness ~ the widest control-face 1-ring Z-span, clamped [16, 256]
// (Engine.cpp's band_layers_for), then ceil(nlayers / band_layers).
inline int usd_band_count(const UsdCage& uc, const std::vector<double>& zs) {
    if (zs.size() < 2) return 1;
    subdiv::Cage cage = subdiv::build_cage(uc.points, uc.counts, uc.indices,
                                           uc.crease_indices, uc.crease_lengths, uc.crease_sharpnesses,
                                           uc.corner_indices, uc.corner_sharpnesses,
                                           uc.boundary, uc.triangle_smooth, uc.st);
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

// True if every face is a quad -- an all-quad mesh dices each face directly as a grid.
inline bool all_quads(const UsdCage& uc) {
    for (int c : uc.counts) if (c != 4) return false;
    return !uc.counts.empty();
}

// ADAPTIVE bound-and-split for a POLYGON mesh (true REYES dicing): each control face dices to its
// OWN edge scale, not one global level, so a fine face beside a coarse panel is not over-diced.
// Quads dice directly as an (Ns x Nt) grid. Triangles and n-gons are center-split into one quad per
// corner (face centroid + edge midpoints -- the first step of USD "bilinear"/Catmull-Clark
// refinement) and each corner-quad dices through the SAME grid routine, so one path covers every
// polygon. Crack-free by construction: every edge's segment count comes from the edge alone (a
// shared original edge is halved identically from both sides, so neighbours agree), boundary samples
// snap to it (coincident snaps make harmless degenerate triangles, never hanging vertices), and
// per-control-vertex normals -- with edge-midpoint/centroid normals derived from them -- match on
// shared edges so the DISPLACED boundary matches too. `tol` is the world facet target (bead width
// for the slice, ~1px for the render). A mesh dices uniformly: all-quad -> direct; any non-quad ->
// center-split EVERY face, so a mixed quad/tri mesh stays crack-free across quad/tri boundaries too.
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
    const bool split = !all_quads(uc);   // any non-quad face -> center-split EVERY face for consistency

    for (int band = 0; band < nbands; ++band) {
        size_t g0 = (size_t)band*nz/nbands, g1 = (size_t)(band+1)*nz/nbands;
        if (g0 >= g1) continue;
        const double zlo = zs[g0], zhi = zs[g1-1];
        std::vector<Mesh> parts(M); std::vector<int> na(M); int maxna = 1;
        for (int m = 0; m < M; ++m) { parts[m].aov_names = shaders[m]->aov_names(); na[m]=int(parts[m].aov_names.size()); parts[m].aov.resize(na[m]); maxna=std::max(maxna,na[m]); }
        std::vector<V3> av(maxna);

        // Dice one quad to its own per-edge scale, displace+shade each sample, emit tris into mm.
        // Corners in grid order: edge 0 bottom P0P1 (s), 1 right P1P2 (t), 2 top P3P2 (s), 3 left P0P3 (t).
        auto dice_quad = [&](Mesh& mm, const Shader& sh, int nak, const V3 P[4], const V3 Nc[4], const V2 UV[4]) {
            auto elen = [&](int a, int b){ double dx=P[a][0]-P[b][0],dy=P[a][1]-P[b][1],dz=P[a][2]-P[b][2]; return std::sqrt(dx*dx+dy*dy+dz*dz); };
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
        };

        for (int f = 0; f < nf; ++f) {
            if (fhi[f] < zlo - max_disp || flo[f] > zhi + max_disp) continue;   // face not in this band
            const int aoff = foff[f], cnt = foff[f+1] - aoff;
            if (cnt < 3) continue;                                             // degenerate face
            int mi = (f < int(ctag.size())) ? ctag[f] : 0; if (mi < 0 || mi >= M) mi = 0;
            Mesh& mm = parts[mi]; const Shader& sh = *shaders[mi]; const int nak = na[mi];
            auto corner = [&](int c, V3& P, V3& N, V2& U){ int vid = uc.indices[aoff+c];
                P = {{uc.points[vid][0], uc.points[vid][1], uc.points[vid][2]}}; N = vn[vid];
                U = hasuv ? V2{{uc.st[aoff+c][0], uc.st[aoff+c][1]}} : V2{{0, 0}}; };
            if (!split && cnt == 4) {                       // quad in an all-quad mesh: dice directly
                V3 P[4], Nc[4]; V2 UV[4];
                for (int c = 0; c < 4; ++c) corner(c, P[c], Nc[c], UV[c]);
                dice_quad(mm, sh, nak, P, Nc, UV);
            } else {                                        // tri / n-gon (or any face once split): center-split
                std::vector<V3> Cp(cnt), Cn(cnt); std::vector<V2> Cuv(cnt);
                V3 Fp{{0,0,0}}, Fn{{0,0,0}}; V2 Fuv{{0,0}};
                for (int c = 0; c < cnt; ++c) { corner(c, Cp[c], Cn[c], Cuv[c]);
                    Fp[0]+=Cp[c][0]; Fp[1]+=Cp[c][1]; Fp[2]+=Cp[c][2];
                    Fn[0]+=Cn[c][0]; Fn[1]+=Cn[c][1]; Fn[2]+=Cn[c][2];
                    Fuv[0]+=Cuv[c][0]; Fuv[1]+=Cuv[c][1]; }
                Fp[0]/=cnt; Fp[1]/=cnt; Fp[2]/=cnt; Fuv[0]/=cnt; Fuv[1]/=cnt;
                { double l=std::sqrt(Fn[0]*Fn[0]+Fn[1]*Fn[1]+Fn[2]*Fn[2]); if (l>1e-12){Fn[0]/=l;Fn[1]/=l;Fn[2]/=l;} }
                // Edge midpoints (position/normal/uv) -- derived from the two endpoints alone, so a
                // shared original edge yields the SAME midpoint from either face (order-independent).
                std::vector<V3> Emp(cnt), Emn(cnt); std::vector<V2> Emuv(cnt);
                for (int e = 0; e < cnt; ++e) { int e1 = (e+1)%cnt;
                    Emp[e] = {{0.5*(Cp[e][0]+Cp[e1][0]), 0.5*(Cp[e][1]+Cp[e1][1]), 0.5*(Cp[e][2]+Cp[e1][2])}};
                    Emn[e] = {{Cn[e][0]+Cn[e1][0], Cn[e][1]+Cn[e1][1], Cn[e][2]+Cn[e1][2]}};
                    double l=std::sqrt(Emn[e][0]*Emn[e][0]+Emn[e][1]*Emn[e][1]+Emn[e][2]*Emn[e][2]); if (l>1e-12){Emn[e][0]/=l;Emn[e][1]/=l;Emn[e][2]/=l;}
                    Emuv[e] = {{0.5*(Cuv[e][0]+Cuv[e1][0]), 0.5*(Cuv[e][1]+Cuv[e1][1])}};
                }
                // one quad per corner: [corner k, mid of edge k, centroid, mid of edge k-1]
                for (int k = 0; k < cnt; ++k) { int kp = (k+cnt-1)%cnt;
                    V3 P[4]  = {Cp[k],  Emp[k],  Fp,  Emp[kp]};
                    V3 Nc[4] = {Cn[k],  Emn[k],  Fn,  Emn[kp]};
                    V2 UV[4] = {Cuv[k], Emuv[k], Fuv, Emuv[kp]};
                    dice_quad(mm, sh, nak, P, Nc, UV);
                }
            }
        }
        Mesh bm = (M == 1) ? std::move(parts[0]) : merge_meshes(parts);
        if (do_slice) { std::vector<double> bandzs(zs.begin()+g0, zs.begin()+g1); auto segs = slice_mesh(bm, bandzs); for (size_t k=0;k<segs.size();++k) out[g0+k]=std::move(segs[k]); }
        if (on_band) on_band(bm);
    }
    return out;
}

// Estimate of the micropolygon-triangle count amplify_poly_banded emits at tolerance `tol` (a
// single-band count, for the CLI stat line). Mirrors the dicing: all-quad -> one grid per face;
// else one grid per corner-quad of every center-split face.
inline size_t poly_tri_estimate(const UsdCage& uc, double tol) {
    const int nf = int(uc.counts.size());
    if (nf == 0) return 0;
    std::vector<int> foff(nf + 1, 0);
    for (int f = 0; f < nf; ++f) foff[f + 1] = foff[f] + uc.counts[f];
    const bool split = !all_quads(uc);
    auto plen = [&](const V3& a, const V3& b){ double dx=a[0]-b[0],dy=a[1]-b[1],dz=a[2]-b[2]; return std::sqrt(dx*dx+dy*dy+dz*dz); };
    auto pt = [&](int c)->V3{ const auto& p = uc.points[uc.indices[c]]; return V3{{p[0],p[1],p[2]}}; };
    size_t tris = 0;
    for (int f = 0; f < nf; ++f) {
        const int a = foff[f], cnt = foff[f+1] - a;
        if (cnt < 3) continue;
        if (!split) {
            int s0 = edge_segments(plen(pt(a+0),pt(a+1)), tol), s2 = edge_segments(plen(pt(a+3),pt(a+2)), tol);
            int t1 = edge_segments(plen(pt(a+1),pt(a+2)), tol), t3 = edge_segments(plen(pt(a+0),pt(a+3)), tol);
            tris += (size_t)std::max(s0,s2)*std::max(t1,t3)*2;
        } else {
            V3 F{{0,0,0}}; for (int c = 0; c < cnt; ++c) { V3 p = pt(a+c); F[0]+=p[0]; F[1]+=p[1]; F[2]+=p[2]; }
            F[0]/=cnt; F[1]/=cnt; F[2]/=cnt;
            auto mid = [&](int i, int j){ V3 p = pt(a+i), q = pt(a+j); return V3{{0.5*(p[0]+q[0]),0.5*(p[1]+q[1]),0.5*(p[2]+q[2])}}; };
            for (int k = 0; k < cnt; ++k) { int kp = (k+cnt-1)%cnt, kn = (k+1)%cnt;
                V3 Ck = pt(a+k), Emk = mid(k,kn), Emp = mid(kp,k);
                int s0 = edge_segments(plen(Ck,Emk), tol), s2 = edge_segments(plen(Emp,F), tol);
                int t1 = edge_segments(plen(Emk,F), tol), t3 = edge_segments(plen(Ck,Emp), tol);
                tris += (size_t)std::max(s0,s2)*std::max(t1,t3)*2;
            }
        }
    }
    return tris;
}

}  // namespace printman
