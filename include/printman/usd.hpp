#pragma once
//
// USD front-end (declaration). The core is not USD-bound; this edge module turns a USD stage into
// a neutral cage the amplifier consumes. Implemented in src/usd/usd_read.cpp, compiled only under
// PRINTMAN_USD (pxr is a fenced heavy dep).
//
#include <array>
#include <string>
#include <vector>

namespace printman {

struct UsdCage {
    std::vector<std::array<double, 3>> points;
    std::vector<int> counts;    // per-face vertex count
    std::vector<int> indices;   // face-vertex indices (CSR w/ counts)
    std::vector<std::array<double, 2>> st;  // face-varying UV, parallel to indices (empty if none)
    std::string subdiv_scheme = "catmullClark";
    std::string up_axis = "Z";   // stage upAxis as authored; points below are already Z-up normalized
    // material (from the bound UsdShadeMaterial, if present)
    std::string osl_disp;       // info:id of the displacement shader
    std::string osl_color;      // info:id of the surface shader
    double max_magnitude = 0;   // inputs:printman:maxMagnitude
    // USD convention fallback for a material we cannot evaluate: primvars:displayColor, else a
    // UsdPreviewSurface constant diffuseColor, else the 0.18 neutral gray. Zero displacement.
    std::array<float, 3> fallback_color{{0.18f, 0.18f, 0.18f}};
    // When UsdPreviewSurface's diffuseColor is texture-connected (UsdUVTexture), the encoded image
    // bytes (resolved out of the .usdz / off disk via the asset resolver), for sampling as albedo.
    std::vector<unsigned char> diffuse_tex;
    std::string diffuse_tex_name;          // asset basename -- gives OIIO the format (extension)
    bool diffuse_tex_srgb = true;          // decode sRGB->linear on read (false if sourceColorSpace=raw)
};

// Load the first Mesh prim (+ its bound material) from a USD file. plugin_dir is the OpenUSD
// plugInfo tree to register. Returns false with err set on failure.
bool load_usd_cage(const std::string& path, const std::string& plugin_dir, UsdCage& out, std::string& err);

}  // namespace printman
