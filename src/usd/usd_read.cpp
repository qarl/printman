#include "printman/usd.hpp"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

namespace printman {

bool load_usd_cage(const std::string& path, const std::string& plugin_dir, UsdCage& out, std::string& err) {
    using namespace pxr;
    if (!plugin_dir.empty()) PlugRegistry::GetInstance().RegisterPlugins(plugin_dir);
    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage) { err = "could not open USD stage: " + path; return false; }

    UsdGeomMesh mesh; UsdPrim meshPrim;
    for (const UsdPrim& p : stage->Traverse())
        if (p.IsA<UsdGeomMesh>()) { mesh = UsdGeomMesh(p); meshPrim = p; break; }
    if (!mesh) { err = "no Mesh prim in " + path; return false; }

    // PrintMan slices along Z, so normalize the stage's up axis to Z at the boundary. USD's default
    // is Y-up (Apple/RealityKit, most interchange); a +90 deg rotation about X maps Y-up to Z-up
    // ((x,y,z)->(x,-z,y)). A stage authored upAxis="Z" (our own assets) is left untouched.
    TfToken up = UsdGeomGetStageUpAxis(stage);
    out.up_axis = up.GetString();
    const bool yup = (up == UsdGeomTokens->y);

    VtVec3fArray pts; mesh.GetPointsAttr().Get(&pts);
    out.points.reserve(pts.size());
    for (const auto& p : pts)
        out.points.push_back(yup ? std::array<double, 3>{p[0], -p[2], p[1]}
                                 : std::array<double, 3>{p[0], p[1], p[2]});
    VtIntArray counts, indices;
    mesh.GetFaceVertexCountsAttr().Get(&counts);
    mesh.GetFaceVertexIndicesAttr().Get(&indices);
    out.counts.assign(counts.begin(), counts.end());
    out.indices.assign(indices.begin(), indices.end());

    TfToken scheme; mesh.GetSubdivisionSchemeAttr().Get(&scheme);
    if (!scheme.IsEmpty()) out.subdiv_scheme = scheme.GetString();

    // primvars:st -> face-varying UV parallel to indices
    UsdGeomPrimvarsAPI pv(meshPrim);
    UsdGeomPrimvar st = pv.GetPrimvar(TfToken("st"));
    if (st && st.GetInterpolation() == UsdGeomTokens->faceVarying) {
        VtVec2fArray vals; st.Get(&vals);
        VtIntArray idx; st.GetIndices(&idx);
        if (!idx.empty()) { out.st.reserve(idx.size()); for (int i : idx) out.st.push_back({vals[i][0], vals[i][1]}); }
        else { out.st.reserve(vals.size()); for (const auto& v : vals) out.st.push_back({v[0], v[1]}); }
    }

    // bound material: displacement + surface shader info:id, and printman:maxMagnitude
    UsdShadeShader surf;
    UsdShadeMaterialBindingAPI bind(meshPrim);
    UsdShadeMaterial mat = bind.ComputeBoundMaterial();
    if (mat) {
        auto shaderOf = [](UsdShadeOutput o) -> UsdShadeShader {
            UsdShadeConnectableAPI src; TfToken n; UsdShadeAttributeType t;
            if (o && o.GetConnectedSource(&src, &n, &t)) return UsdShadeShader(src.GetPrim());
            return UsdShadeShader();
        };
        auto idOf = [](const UsdShadeShader& s) -> std::string {
            if (!s) return "";
            TfToken id; s.GetIdAttr().Get(&id); return id.GetString();
        };
        UsdShadeShader disp = shaderOf(mat.GetDisplacementOutput());
        surf = shaderOf(mat.GetSurfaceOutput());
        out.osl_disp = idOf(disp);
        out.osl_color = idOf(surf);
        if (disp) {
            UsdShadeInput mm = disp.GetInput(TfToken("printman:maxMagnitude"));
            if (mm) { float v = 0; if (mm.Get(&v)) out.max_magnitude = v; }
        }
    }

    // Fallback albedo per USD convention, for a material we cannot evaluate as an OSL shader:
    // authored primvars:displayColor wins (USD's "colorSet even without a shader"); else a
    // UsdPreviewSurface constant diffuseColor; else the schema's 0.18 neutral gray (the default).
    UsdGeomPrimvar dc = pv.GetPrimvar(TfToken("displayColor"));
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
    if (out.diffuse_tex.empty() && !color_from_surface && dc && dc.Get(&dcv) && !dcv.empty())
        out.fallback_color = {dcv[0][0], dcv[0][1], dcv[0][2]};
    return true;
}

}  // namespace printman
