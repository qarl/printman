#include "printman/usd.hpp"

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
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

    VtVec3fArray pts; mesh.GetPointsAttr().Get(&pts);
    out.points.reserve(pts.size());
    for (const auto& p : pts) out.points.push_back({p[0], p[1], p[2]});
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
        UsdShadeShader surf = shaderOf(mat.GetSurfaceOutput());
        out.osl_disp = idOf(disp);
        out.osl_color = idOf(surf);
        if (disp) {
            UsdShadeInput mm = disp.GetInput(TfToken("printman:maxMagnitude"));
            if (mm) { float v = 0; if (mm.Get(&v)) out.max_magnitude = v; }
        }
    }
    return true;
}

}  // namespace printman
