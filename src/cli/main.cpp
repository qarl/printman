#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "printman/band.hpp"
#ifdef PRINTMAN_OSL
#include "printman/osl.hpp"
#include "printman/texshader.hpp"
#endif
#ifdef PRINTMAN_USD
#include "printman/amplify.hpp"
#include "printman/usd.hpp"
#endif
#include "printman/png.hpp"
#include "printman/raster.hpp"
#include "printman/render.hpp"
#include "printman/slicer.hpp"
#include "printman/surface.hpp"

using namespace printman;

namespace {
void usage() {
    std::printf(
        "printman - geometry amplification for 3D printing\n"
        "\n"
        "usage:\n"
        "  printman SCENE.usda [--out DIR] [--stack] [--subdiv-level N] [--aov NAME]\n"
        "                      [--shaderdir DIR] [--reach MM] [--layer MM] [--res PXMM]\n"
        "  printman --gate [--bands N]      self-test: band-count=1 == band-count=N (design gate)\n"
        "  printman --help\n"
        "\n"
        "  SCENE.usda    load a USD mesh + its bound material, subdivide, amplify, render\n"
        "  --stack       also write the per-layer PNG slice stack (off by default; render only)\n"
        "  --subdiv-level N   Catmull-Clark refinement levels (default 2)\n"
        "  --aov NAME    shader output to rasterize per layer (default Cout)\n"
        "  --shaderdir DIR    directory of compiled .oso shaders (the material's OSL shaders)\n"
        "  --bands N     number of memory-bounded Z-bands for the gate (default 8)\n");
}

Frame window_of(const Mesh& m, double ppm, int pad) {
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (auto& p : m.pos) { x0 = std::min(x0, (double)p[0]); x1 = std::max(x1, (double)p[0]); y0 = std::min(y0, (double)p[1]); y1 = std::max(y1, (double)p[1]); }
    Frame fr; fr.ppm = ppm; fr.xmin = x0 - pad / ppm; fr.ymin = y0 - pad / ppm;
    fr.W = (int)std::ceil((x1 - x0) * ppm) + 2 * pad; fr.H = (int)std::ceil((y1 - y0) * ppm) + 2 * pad;
    fr.rgb.assign((size_t)fr.W * fr.H * 3, 0);
    return fr;
}

void write_stack_and_render(const Mesh& m, const std::vector<LayerSegs>& layers, const Frame& win,
                            int aovk, bool srgb, const std::string& out, const std::string& aovname,
                            bool stack) {
    std::error_code ec; std::filesystem::create_directories(out, ec);
    if (stack) {
        for (size_t li = 0; li < layers.size(); ++li) {
            Frame fr = win; rasterize_layer(fr, layers[li], aovk, srgb);
            char p[1024]; std::snprintf(p, sizeof(p), "%s/layer_%04zu.png", out.c_str(), li); write_png(p, fr);
        }
    }
    int rc = m.aov_index("Cout"); bool rs = rc >= 0; if (rc < 0) rc = 0;
    Frame rnd = render_mesh(m, rc, rs, true, 25, 15, 700); write_png(out + "/render.png", rnd);
    std::printf("printman: render%s -> %s/\n",
                stack ? (" + " + std::to_string(layers.size()) + "-layer stack (aov '" + aovname + "')").c_str() : "",
                out.c_str());
}

#ifdef PRINTMAN_USD
int run_usd(const std::string& usd_in, const std::string& out, const std::string& aovname,
            double layer, double ppm, int subdiv_level, const std::string& shaderdir,
            const std::string& osl_disp, const std::string& osl_color, double reach, double amp, bool stack) {
    UsdCage uc; std::string err;
    if (!load_usd_cage(usd_in, PRINTMAN_USD_PLUGINDIR, uc, err)) { std::fprintf(stderr, "printman: %s\n", err.c_str()); return 1; }
    std::printf("printman: loaded %s -- %zu points, %zu faces, uv=%s, scheme=%s, upAxis=%s%s\n", usd_in.c_str(),
                uc.points.size(), uc.counts.size(), uc.st.empty() ? "no" : "yes", uc.subdiv_scheme.c_str(),
                uc.up_axis.c_str(), uc.up_axis == "Y" ? " (rotated to Z-up)" : "");
    std::unique_ptr<Shader> shp;
#ifdef PRINTMAN_OSL
    std::string dn = !osl_disp.empty() ? osl_disp : uc.osl_disp, cn = !osl_color.empty() ? osl_color : uc.osl_color;
    // Only treat the material as ours if a compiled .oso is actually present -- otherwise it names
    // a shader PrintMan can't evaluate (UsdPreviewSurface, a renderer-specific shader, ...), which
    // per USD convention is not an error: we fall back below rather than provoke an OSL load error.
    auto has_oso = [&](const std::string& n) {
        return !n.empty() && std::filesystem::exists(shaderdir + "/" + n + ".oso");
    };
    if ((dn.empty() || has_oso(dn)) && (cn.empty() || has_oso(cn)) && (!dn.empty() || !cn.empty())) {
        double rr = reach > 0 ? reach : (uc.max_magnitude > 0 ? uc.max_magnitude : 10);
        try {
            shp = std::make_unique<OslShader>(shaderdir, dn, cn, rr);
            std::printf("printman: OSL disp='%s' color='%s' reach=%g\n", dn.c_str(), cn.c_str(), rr);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "printman: material '%s'/'%s' not an OSL shader (%s)\n", dn.c_str(), cn.c_str(), e.what());
        }
    }
#else
    (void)shaderdir; (void)osl_disp; (void)osl_color; (void)reach;
#endif
    (void)amp;
#ifdef PRINTMAN_OSL
    if (!shp && !uc.diffuse_tex.empty()) {
        auto ts = std::make_unique<TextureShader>(uc.diffuse_tex, uc.diffuse_tex_name, uc.diffuse_tex_srgb);
        if (ts->ok()) {
            std::printf("printman: no PrintMan shader; UsdPreviewSurface diffuseColor texture '%s' (%dx%d), no displacement\n",
                        uc.diffuse_tex_name.c_str(), ts->W, ts->H);
            shp = std::move(ts);
        } else {
            std::fprintf(stderr, "printman: could not decode diffuseColor texture '%s'; using flat colour\n", uc.diffuse_tex_name.c_str());
        }
    }
#endif
    if (!shp) {
        auto fl = std::make_unique<FlatShader>();
        fl->color = {uc.fallback_color[0], uc.fallback_color[1], uc.fallback_color[2]};
        std::printf("printman: no PrintMan shader; USD fallback color (%.3g %.3g %.3g), no displacement\n",
                    uc.fallback_color[0], uc.fallback_color[1], uc.fallback_color[2]);
        shp = std::move(fl);
    }
    Mesh m = amplify_usd(uc, *shp, subdiv_level);
    if (m.aov_names.empty()) { std::fprintf(stderr, "printman: shader produced no AOVs\n"); return 1; }
    int aovk = -1; for (size_t k = 0; k < m.aov_names.size(); ++k) if (m.aov_names[k] == aovname) aovk = (int)k;
    if (aovk < 0) { std::fprintf(stderr, "printman: aov '%s' not found, using '%s'\n", aovname.c_str(), m.aov_names[0].c_str()); aovk = 0; }
    bool srgb = (m.aov_names[aovk] == "Cout");
    double zlo, zhi; mesh_z_range(m, zlo, zhi);
    std::vector<double> zs; for (double z = layer * 0.5; z < zhi; z += layer) zs.push_back(z);
    auto layers = slice_mesh(m, zs);
    Frame win = window_of(m, ppm, 2);
    write_stack_and_render(m, layers, win, aovk, srgb, out, aovname, stack);
    return 0;
}
#endif

Frame window_for(double xspan, double yspan, double cx, double cy, double ppm, int pad) {
    Frame fr;
    fr.ppm = ppm;
    fr.W = (int)std::ceil(xspan * ppm) + 2 * pad;
    fr.H = (int)std::ceil(yspan * ppm) + 2 * pad;
    fr.xmin = cx - xspan / 2 - pad / ppm;
    fr.ymin = cy - yspan / 2 - pad / ppm;
    fr.rgb.assign((size_t)fr.W * fr.H * 3, 0);
    return fr;
}
}  // namespace

int main(int argc, char** argv) {
    std::string out = "out", aovname = "Cout", osl_disp, osl_color, shaderdir, usd_in;
    double layer = 0.1, ppm = 8.0, R = 20.0, amp = 6.0, reach = 0;
    int nu = 180, nv = 90, bands = 8, subdiv_level = 2;
    bool gate = false, stack = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--gate") gate = true;
        else if (a == "--stack") stack = true;
        else if (a == "--out") out = next();
        else if (a == "--aov") aovname = next();
        else if (a == "--layer") layer = std::atof(next().c_str());
        else if (a == "--res") ppm = std::atof(next().c_str());
        else if (a == "--radius") R = std::atof(next().c_str());
        else if (a == "--amp") amp = std::atof(next().c_str());
        else if (a == "--bands") bands = std::atoi(next().c_str());
        else if (a == "--osl-disp") osl_disp = next();
        else if (a == "--osl-color") osl_color = next();
        else if (a == "--shaderdir") shaderdir = next();
        else if (a == "--reach") reach = std::atof(next().c_str());
        else if (a == "--subdiv-level") subdiv_level = std::atoi(next().c_str());
        else if (a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') usd_in = a;  // positional: a USD scene
    }

    if (!usd_in.empty()) {
#ifdef PRINTMAN_USD
        return run_usd(usd_in, out, aovname, layer, ppm, subdiv_level, shaderdir, osl_disp, osl_color, reach, amp, stack);
#else
        std::fprintf(stderr, "printman: built without USD (configure -DPRINTMAN_USD=ON)\n"); return 1;
#endif
    }
    if (!gate) { usage(); return argc < 2 ? 1 : 0; }

    // --gate: prove the banded pipeline is band-count invariant (band-count=1 must equal
    // band-count=N, pixel-identical). Driven over a built-in parametric sphere as a controlled
    // banded surface -- the only banded path today; the USD path is materialized, not yet banded.
    ProceduralPlanet planet; planet.amp = amp;
    Shader& sh = planet;
    SphereCage cage(R, nu, nv, sh);
    std::vector<double> zs;
    for (double z = layer * 0.5; z < cage.top; z += layer) zs.push_back(z);
    double span = 2 * (R + amp);
    Frame win = window_for(span, span, 0, 0, ppm, 2);
    if (cage.aov_names.empty()) { std::fprintf(stderr, "printman: shader produced no AOVs\n"); return 1; }
    int aovk = -1; for (size_t k = 0; k < cage.aov_names.size(); ++k) if (cage.aov_names[k] == aovname) aovk = (int)k;
    if (aovk < 0) { std::fprintf(stderr, "printman: aov '%s' not found, using '%s'\n", aovname.c_str(), cage.aov_names[0].c_str()); aovk = 0; }
    bool srgb = (cage.aov_names[aovk] == "Cout");

    auto A = amplify_banded(cage, zs, 1);
    auto B = amplify_banded(cage, zs, bands);
    long worst = 0, total_diff = 0;
    for (size_t li = 0; li < zs.size(); ++li) {
        Frame fa = win, fb = win;
        rasterize_layer(fa, A[li], aovk, srgb);
        rasterize_layer(fb, B[li], aovk, srgb);
        long d = 0;
        for (size_t p = 0; p < fa.rgb.size(); ++p) d += (fa.rgb[p] != fb.rgb[p]);
        worst = std::max(worst, d); total_diff += d;
    }
    std::printf("gate: band-count=1 vs band-count=%d over %zu layers @ %dx%d px\n",
                bands, zs.size(), win.W, win.H);
    std::printf("gate: %s  (total differing subpixels=%ld, worst layer=%ld)\n",
                total_diff == 0 ? "PASS" : "FAIL", total_diff, worst);
    return total_diff == 0 ? 0 : 2;
}
