#include "printman/usd.hpp"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>

namespace printman {

namespace {

// Resolve the material bound directly to a prim (a mesh, or a materialBind GeomSubset), trying the
// "preview"/"full" purposes before allPurpose -- Maya/pxr exports (Apple's) bind that way.
pxr::UsdShadeMaterial resolve_direct(const pxr::UsdPrim& p) {
    using namespace pxr;
    UsdShadeMaterialBindingAPI b(p);
    UsdShadeMaterial m = b.ComputeBoundMaterial(UsdShadeTokens->preview);
    if (!m) m = b.ComputeBoundMaterial(UsdShadeTokens->full);
    if (!m) m = b.ComputeBoundMaterial();
    return m;
}

// Fill a UsdMaterial from a resolved UsdShadeMaterial, per the USD convention ladder: our
// displacement/surface shader info:ids for the OSL path; else a diffuseColor texture (bytes pulled
// through the asset resolver) or constant; else the mesh's displayColor; else the 0.18 gray.
void extract_material(const pxr::UsdStageRefPtr& stage, const pxr::UsdShadeMaterial& mat,
                      const pxr::UsdGeomPrimvar& displayColor, UsdMaterial& out) {
    using namespace pxr;
    UsdShadeShader surf;
    if (mat) {
        auto idOf = [](const UsdShadeShader& s) -> std::string {
            if (!s) return "";
            TfToken id; s.GetIdAttr().Get(&id); return id.GetString();
        };
        // ComputeSurface/DisplacementSource resolve the terminal shader properly: render-context
        // fallback (universal) and through NodeGraph boundaries -- unlike reading the output's
        // direct connection, which misses materials wired that way (e.g. Apple's).
        UsdShadeShader disp = mat.ComputeDisplacementSource();
        surf = mat.ComputeSurfaceSource();
        out.osl_disp = idOf(disp);
        out.osl_color = idOf(surf);
        if (disp) {
            UsdShadeInput mm = disp.GetInput(TfToken("printman:maxMagnitude"));
            if (mm) { float v = 0; if (mm.Get(&v)) out.max_magnitude = v; }
        }
    }

    VtVec3fArray dcv;
    bool color_from_surface = false;
    if (surf) {
        UsdShadeInput dcin = surf.GetInput(TfToken("diffuseColor"));
        GfVec3f c;
        if (dcin && dcin.HasConnectedSource()) {
            // diffuseColor <- UsdUVTexture(file, st): resolve the image bytes for per-vertex sampling.
            UsdShadeConnectableAPI src; TfToken n; UsdShadeAttributeType t;
            if (dcin.GetConnectedSource(&src, &n, &t)) {
                UsdShadeShader tex(src.GetPrim());
                TfToken tid; if (tex) tex.GetIdAttr().Get(&tid);
                if (tid == TfToken("UsdUVTexture")) {
                    UsdShadeInput fileIn = tex.GetInput(TfToken("file"));
                    SdfAssetPath ap;
                    if (fileIn && fileIn.Get(&ap)) {
                        std::string rp = ap.GetResolvedPath();
                        ArResolverContextBinder binder(stage->GetPathResolverContext());
                        ArResolver& res = ArGetResolver();
                        ArResolvedPath resolved = rp.empty() ? res.Resolve(ap.GetAssetPath()) : ArResolvedPath(rp);
                        if (std::shared_ptr<ArAsset> asset = res.OpenAsset(resolved)) {
                            size_t sz = asset->GetSize();
                            std::shared_ptr<const char> buf = asset->GetBuffer();
                            if (buf && sz) {
                                out.diffuse_tex.assign(buf.get(), buf.get() + sz);
                                out.diffuse_tex_name = ap.GetAssetPath();
                            }
                        }
                        TfToken cs; UsdShadeInput csin = tex.GetInput(TfToken("sourceColorSpace"));
                        if (csin && csin.Get(&cs)) out.diffuse_tex_srgb = (cs != TfToken("raw"));
                    }
                }
            }
        } else if (dcin && dcin.Get(&c)) {  // authored constant diffuseColor
            out.fallback_color = {c[0], c[1], c[2]};
            color_from_surface = true;
        }
    }
    // displayColor is USD's fallback color for an unshaded gprim: use it only when the bound
    // surface gave us neither a texture nor a constant diffuseColor. Else the 0.18 default stands.
    if (out.diffuse_tex.empty() && !color_from_surface && displayColor && displayColor.Get(&dcv) && !dcv.empty())
        out.fallback_color = {dcv[0][0], dcv[0][1], dcv[0][2]};
}

// Fill a cage's materials + per-face material index. Each materialBind GeomSubset contributes a
// material and claims its faces. Faces in no subset fall to a default material (the mesh's direct
// binding, or a descendant's for exports that bind on a nested shading-group child). A mesh with no
// subsets, or one whose subsets cover every face (Apple's), ends up with a single clean material.
void read_materials(const pxr::UsdStageRefPtr& stage, const pxr::UsdPrim& meshPrim,
                    const pxr::UsdGeomPrimvar& displayColor, UsdCage& out) {
    using namespace pxr;
    std::vector<UsdGeomSubset> subs = UsdShadeMaterialBindingAPI(meshPrim).GetMaterialBindSubsets();
    if (subs.empty()) {
        UsdShadeMaterial m0 = resolve_direct(meshPrim);
        if (!m0) for (const UsdPrim& ch : meshPrim.GetDescendants()) { m0 = resolve_direct(ch); if (m0) break; }
        out.materials.emplace_back();
        extract_material(stage, m0, displayColor, out.materials[0]);
        return;
    }
    out.face_material.assign(out.counts.size(), -1);
    for (const UsdGeomSubset& sub : subs) {
        UsdShadeMaterial sm = resolve_direct(sub.GetPrim());
        if (!sm) continue;
        int idx = int(out.materials.size());
        out.materials.emplace_back();
        extract_material(stage, sm, displayColor, out.materials[idx]);
        VtIntArray fidx; sub.GetIndicesAttr().Get(&fidx);
        for (int fi : fidx) if (fi >= 0 && fi < int(out.face_material.size())) out.face_material[fi] = idx;
    }
    // Faces no subset claimed (or all faces, if no subset resolved) get a default material.
    bool uncovered = false;
    for (int f : out.face_material) if (f < 0) { uncovered = true; break; }
    if (uncovered) {
        int didx = int(out.materials.size());
        UsdShadeMaterial dm = resolve_direct(meshPrim);
        if (!dm) for (const UsdPrim& ch : meshPrim.GetDescendants()) { dm = resolve_direct(ch); if (dm) break; }
        out.materials.emplace_back();
        extract_material(stage, dm, displayColor, out.materials[didx]);
        for (int& f : out.face_material) if (f < 0) f = didx;
    }
}

// Read one Mesh prim into a cage: points baked to world space (its local-to-world transform) and
// normalized to Z-up (yup), faces, face-varying primvars:st, and its bound material. Returns false
// for an empty mesh (no points).
bool read_mesh(const pxr::UsdStageRefPtr& stage, const pxr::UsdPrim& prim, bool yup, double mm_per_unit, UsdCage& out) {
    using namespace pxr;
    const UsdTimeCode t = UsdTimeCode::EarliestTime();   // earliest authored sample if animated, else default
    UsdGeomMesh mesh(prim);
    VtVec3fArray pts; mesh.GetPointsAttr().Get(&pts, t);
    if (pts.empty()) return false;

    // Bake the prim's world transform, SCALE to mm (stage metersPerUnit -> mm_per_unit), then normalize
    // the up-axis to Z (Y-up -> +90 deg about X: (x,y,z)->(x,-z,y)). Normals are recomputed downstream.
    // Reject a non-finite point rather than emit NaN geometry.
    GfMatrix4d m2w = UsdGeomXformable(prim).ComputeLocalToWorldTransform(t);
    TfToken orient; mesh.GetOrientationAttr().Get(&orient, t);
    const bool flip = (m2w.GetDeterminant() < 0.0) != (orient == UsdGeomTokens->leftHanded);
    out.points.reserve(pts.size());
    for (const auto& p : pts) {
        GfVec3d w = m2w.Transform(GfVec3d(p[0], p[1], p[2])) * mm_per_unit;
        if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) return false;
        out.points.push_back(yup ? std::array<double, 3>{w[0], -w[2], w[1]}
                                 : std::array<double, 3>{w[0], w[1], w[2]});
    }
    VtIntArray counts, indices;
    mesh.GetFaceVertexCountsAttr().Get(&counts, t);
    mesh.GetFaceVertexIndicesAttr().Get(&indices, t);
    out.counts.assign(counts.begin(), counts.end());
    out.indices.assign(indices.begin(), indices.end());
    // Validate topology: every face >= 3 verts, counts sum to the index count, indices in range.
    long long sum = 0; for (int c : out.counts) { if (c < 3) return false; sum += c; }
    if (sum != (long long)out.indices.size()) return false;
    const int np = (int)out.points.size();
    for (int i : out.indices) if (i < 0 || i >= np) return false;

    TfToken scheme; mesh.GetSubdivisionSchemeAttr().Get(&scheme);
    if (!scheme.IsEmpty()) out.subdiv_scheme = scheme.GetString();

    // Semi-sharp subdivision tags. Crease/corner indices are point indices, unaffected by the world
    // bake above. sharpnesses read as float -> stored double. Only Catmull-Clark uses them downstream.
    { VtIntArray ci, cl; VtFloatArray cs;
      mesh.GetCreaseIndicesAttr().Get(&ci); mesh.GetCreaseLengthsAttr().Get(&cl); mesh.GetCreaseSharpnessesAttr().Get(&cs);
      out.crease_indices.assign(ci.begin(), ci.end());
      out.crease_lengths.assign(cl.begin(), cl.end());
      out.crease_sharpnesses.assign(cs.begin(), cs.end()); }
    { VtIntArray ci; VtFloatArray cs;
      mesh.GetCornerIndicesAttr().Get(&ci); mesh.GetCornerSharpnessesAttr().Get(&cs);
      out.corner_indices.assign(ci.begin(), ci.end());
      out.corner_sharpnesses.assign(cs.begin(), cs.end()); }
    { TfToken ib; mesh.GetInterpolateBoundaryAttr().Get(&ib);   // default (unauthored) -> edgeAndCorner
      out.boundary = (ib == UsdGeomTokens->none) ? 0 : (ib == UsdGeomTokens->edgeOnly) ? 1 : 2; }
    { TfToken tr; mesh.GetTriangleSubdivisionRuleAttr().Get(&tr);
      out.triangle_smooth = (tr == UsdGeomTokens->smooth); }

    // primvars:st -> face-varying UV parallel to indices
    UsdGeomPrimvarsAPI pv(prim);
    UsdGeomPrimvar st = pv.GetPrimvar(TfToken("st"));
    if (st && st.GetInterpolation() == UsdGeomTokens->faceVarying) {
        VtVec2fArray vals; st.Get(&vals);
        VtIntArray idx; st.GetIndices(&idx);
        if (!idx.empty()) { out.st.reserve(idx.size()); for (int i : idx) out.st.push_back({vals[i][0], vals[i][1]}); }
        else { out.st.reserve(vals.size()); for (const auto& v : vals) out.st.push_back({v[0], v[1]}); }
    }

    // Left-handed authored orientation, or a mirroring (det<0) world transform: reverse each face's
    // winding (and its face-varying st) so faces stay CCW-outward for the winding-derived normals and
    // the non-zero fill. Crease/corner tags are point indices, unaffected by winding.
    if (flip) {
        size_t o = 0; const bool hasst = out.st.size() == out.indices.size();
        for (int c : out.counts) {
            std::reverse(out.indices.begin() + o, out.indices.begin() + o + c);
            if (hasst) std::reverse(out.st.begin() + o, out.st.begin() + o + c);
            o += (size_t)c;
        }
    }

    read_materials(stage, prim, pv.GetPrimvar(TfToken("displayColor")), out);
    return true;
}

}  // namespace

bool load_usd_scene(const std::string& path, const std::string& plugin_dir,
                    std::vector<UsdCage>& out, int& skipped, std::string& err) {
    using namespace pxr;
    if (!plugin_dir.empty()) PlugRegistry::GetInstance().RegisterPlugins(plugin_dir);
    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage) { err = "could not open USD stage: " + path; return false; }

    TfToken up = UsdGeomGetStageUpAxis(stage);
    const bool yup = (up == UsdGeomTokens->y);
    const std::string upstr = up.GetString();
    // Stage units -> mm. USD authors in metersPerUnit (default 0.01 = cm); mm = unit * metersPerUnit * 1000.
    double mm_per_unit = UsdGeomGetStageMetersPerUnit(stage) * 1000.0;
    if (!(mm_per_unit > 0.0)) mm_per_unit = 10.0;   // guard an unset/zero metersPerUnit (USD default cm)

    skipped = 0;
    // Traverse instance proxies too, so scene-graph-instanced meshes are read (and baked to world)
    // once per instance -- Traverse() alone skips them. Baking to world means each instance's own
    // transform (incl. mirror via the det<0 flip) and its device level (world edge lengths) are
    // correct without a local-frame prototype path. Abstract prototype (class) prims are skipped.
    for (const UsdPrim& p : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies())) {
        if (!p.IsA<UsdGeomMesh>()) continue;
        // Skip non-render geometry: proxy/guide stand-ins and invisible prims are not what we print.
        UsdGeomImageable img(p);
        TfToken purpose = img.ComputePurpose();
        if (purpose == UsdGeomTokens->proxy || purpose == UsdGeomTokens->guide) { ++skipped; continue; }
        if (img.ComputeVisibility() == UsdGeomTokens->invisible) { ++skipped; continue; }
        UsdCage c; c.up_axis = upstr;
        if (read_mesh(stage, p, yup, mm_per_unit, c)) out.push_back(std::move(c));
        else ++skipped;
    }
    if (out.empty()) { err = "no printable Mesh prim in " + path; return false; }
    return true;
}

}  // namespace printman
