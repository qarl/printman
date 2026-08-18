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
#include "printman/band_usd.hpp"
#include "printman/usd.hpp"
#endif
#include "printman/png.hpp"
#include "printman/raster.hpp"
#include "printman/render.hpp"
#include "printman/slicer.hpp"

using namespace printman;

namespace {
void usage() {
    std::printf(
        "printman - geometry amplification for 3D printing\n"
        "\n"
        "usage:\n"
        "  printman SCENE.usda [--out DIR] [--stack] [--quad] [--white] [--subdiv-level N]\n"
        "                      [--aov NAME] [--shaderdir DIR] [--reach MM] [--layer MM] [--res PXMM]\n"
        "  printman --gate [--bands N]      self-test: band-count=1 == band-count=N (design gate)\n"
        "  printman --help\n"
        "\n"
        "  SCENE.usda    load a USD mesh + its bound material, subdivide, amplify, render\n"
        "  --quad        render a 2x2 contact sheet from four angles (on a white background)\n"
        "  --white       render on a white background instead of dark\n"
        "  --stack       also write the per-layer PNG slice stack (off by default; render only)\n"
        "  --subdiv-level N   force a Catmull-Clark level; default AUTO -- the slice dices to the\n"
        "                bead width, the render to ~1px, each computed from its own resolution\n"
        "  --line-width MM    bead/extrusion width the slice dices to (default 0.4)\n"
        "  --rres PX     render image resolution (default 700); the render level follows it\n"
        "  --aov NAME    shader output to rasterize per layer (default Cout)\n"
        "  --shaderdir DIR    directory of compiled .oso shaders (the material's OSL shaders)\n"
        "  --bands N     number of memory-bounded Z-bands for the gate (default 8)\n");
}

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

#ifdef PRINTMAN_USD
enum class ShaderKind { Osl, Texture, Flat };

// Pick the shader for one material, following the USD convention ladder: our OSL shader (only if
// its .oso exists) -> a diffuseColor texture -> the flat convention colour.
std::unique_ptr<Shader> make_shader(const UsdMaterial& mtl, const std::string& shaderdir,
                                    const std::string& osl_disp, const std::string& osl_color,
                                    double reach, ShaderKind& kind) {
#ifdef PRINTMAN_OSL
    std::string dn = !osl_disp.empty() ? osl_disp : mtl.osl_disp;
    std::string cn = !osl_color.empty() ? osl_color : mtl.osl_color;
    auto has_oso = [&](const std::string& n) {
        return !n.empty() && std::filesystem::exists(shaderdir + "/" + n + ".oso");
    };
    if ((dn.empty() || has_oso(dn)) && (cn.empty() || has_oso(cn)) && (!dn.empty() || !cn.empty())) {
        double rr = reach > 0 ? reach : (mtl.max_magnitude > 0 ? mtl.max_magnitude : 10);
        try {
            auto s = std::make_unique<OslShader>(shaderdir, dn, cn, rr);
            std::printf("printman: material uses OSL disp='%s' color='%s' reach=%g\n", dn.c_str(), cn.c_str(), rr);
            kind = ShaderKind::Osl;
            return s;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "printman: material '%s'/'%s' not an OSL shader (%s)\n", dn.c_str(), cn.c_str(), e.what());
        }
    }
    if (!mtl.diffuse_tex.empty()) {
        auto ts = std::make_unique<TextureShader>(mtl.diffuse_tex, mtl.diffuse_tex_name, mtl.diffuse_tex_srgb);
        if (ts->ok()) {
            std::printf("printman: material uses diffuseColor texture '%s' (%dx%d)\n", mtl.diffuse_tex_name.c_str(), ts->W, ts->H);
            kind = ShaderKind::Texture;
            return ts;
        }
        std::fprintf(stderr, "printman: could not decode diffuseColor texture '%s'; using flat colour\n", mtl.diffuse_tex_name.c_str());
    }
#else
    (void)shaderdir; (void)osl_disp; (void)osl_color; (void)reach;
#endif
    auto fl = std::make_unique<FlatShader>();
    fl->color = {mtl.fallback_color[0], mtl.fallback_color[1], mtl.fallback_color[2]};
    kind = ShaderKind::Flat;
    return fl;
}

int run_usd(const std::string& usd_in, const std::string& out, const std::string& aovname,
            double layer, double ppm, int subdiv_level, const std::string& shaderdir,
            const std::string& osl_disp, const std::string& osl_color, double reach, double amp,
            bool stack, bool quad, int bg, int rres, double line_width) {
    (void)amp;
    std::vector<UsdCage> meshes; std::string err; int skipped = 0;
    if (!load_usd_scene(usd_in, PRINTMAN_USD_PLUGINDIR, meshes, skipped, err)) { std::fprintf(stderr, "printman: %s\n", err.c_str()); return 1; }
    size_t np = 0, nf = 0, nmat = 0; for (const auto& u : meshes) { np += u.points.size(); nf += u.counts.size(); nmat += u.materials.size(); }
    const std::string& up = meshes.front().up_axis;
    char sk[64] = ""; if (skipped) std::snprintf(sk, sizeof(sk), ", skipped %d proxy/guide/hidden", skipped);
    std::printf("printman: loaded %s -- %zu mesh(es), %zu material(s), %zu points, %zu faces, upAxis=%s%s%s\n",
                usd_in.c_str(), meshes.size(), nmat, np, nf, up.c_str(),
                up == "Y" ? " (rotated to Z-up)" : "", sk);

    // A shader per bound material, per mesh. Built up front so max_reach is known before we size zs.
    std::vector<std::vector<std::unique_ptr<Shader>>> owned(meshes.size());
    std::vector<std::vector<const Shader*>> shaders(meshes.size());
    int nosl = 0, ntex = 0, nflat = 0; double max_disp = 0;
    for (size_t i = 0; i < meshes.size(); ++i)
        for (const UsdMaterial& mtl : meshes[i].materials) {
            ShaderKind kind = ShaderKind::Flat;
            owned[i].push_back(make_shader(mtl, shaderdir, osl_disp, osl_color, reach, kind));
            shaders[i].push_back(owned[i].back().get());
            (kind == ShaderKind::Osl ? nosl : kind == ShaderKind::Texture ? ntex : nflat)++;
            max_disp = std::max(max_disp, shaders[i].back()->max_reach());
        }
    if (meshes.size() > 1 || nmat > 1)
        std::printf("printman: %zu material(s) -- %d OSL, %d textured, %d flat (no displacement on non-OSL parts)\n",
                    nmat, nosl, ntex, nflat);
    if (shaders.empty() || shaders[0].empty()) { std::fprintf(stderr, "printman: no material\n"); return 1; }

    // World bounds (grown by max displacement) fix the layer set, the slice window, and the camera --
    // all up front, so the band loop and the incremental render share one framing and nothing is held whole.
    double x0=1e30,x1=-1e30,y0=1e30,y1=-1e30,z0=1e30,z1=-1e30;
    for (const auto& u : meshes) for (const auto& p : u.points) {
        x0=std::min(x0,p[0]); x1=std::max(x1,p[0]); y0=std::min(y0,p[1]); y1=std::max(y1,p[1]); z0=std::min(z0,p[2]); z1=std::max(z1,p[2]); }
    if (x0 > x1) { std::fprintf(stderr, "printman: empty scene\n"); return 1; }
    std::vector<double> zs; for (double z = z0 - max_disp + layer * 0.5; z < z1 + max_disp; z += layer) zs.push_back(z);
    const double xspan = (x1-x0) + 2*max_disp, yspan = (y1-y0) + 2*max_disp, zspan = (z1-z0) + 2*max_disp;
    Frame win = window_for(xspan, yspan, (x0+x1)/2, (y0+y1)/2, ppm, 2);

    // AOV index for the slice stack, Cout index for the render (assume a shared schema across meshes).
    auto an0 = shaders[0][0]->aov_names();
    int aovk = -1, cout_k = 0;
    for (size_t k = 0; k < an0.size(); ++k) { if (an0[k] == aovname) aovk = (int)k; if (an0[k] == "Cout") cout_k = (int)k; }
    if (aovk < 0) { std::fprintf(stderr, "printman: aov '%s' not found, using '%s'\n", aovname.c_str(), an0.empty()?"":an0[0].c_str()); aovk = 0; }
    const bool srgb = (aovk < (int)an0.size() && an0[aovk] == "Cout");

    // Render frame(s) + z-buffer(s), one per view, that every band paints into.
    const int RN = rres, ncam = quad ? 4 : 1;
    const V3 ctr{{(x0+x1)/2, (y0+y1)/2, (z0+z1)/2}};
    double prad = 0;  // tight bounding-sphere radius: farthest control point, grown by displacement
    for (const auto& u : meshes) for (const auto& p : u.points) {
        double dx = p[0]-ctr[0], dy = p[1]-ctr[1], dz = p[2]-ctr[2]; prad = std::max(prad, std::sqrt(dx*dx+dy*dy+dz*dz)); }
    const double rad = prad + max_disp;
    (void)zspan;

    // Per-output dicing: the SLICE dices to the bead width (its XY tolerance), the RENDER to ~1
    // pixel of the framebuffer. Each is the level at which the coarsest control edge refines below
    // that world tolerance (device_level, clamped [2,8]). --subdiv-level, if given, overrides both.
    double max_edge = 0;
    for (const auto& u : meshes) {
        size_t off = 0;
        for (int n : u.counts) {
            for (int k = 0; k < n; ++k) {
                const auto& a = u.points[u.indices[off + k]];
                const auto& b = u.points[u.indices[off + ((k + 1 == n) ? 0 : k + 1)]];
                double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
                max_edge = std::max(max_edge, std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            off += n;
        }
    }
    const double render_tol = rad / (RN * 0.46);   // one framebuffer pixel, in world mm
    const int slice_level  = (subdiv_level >= 0) ? subdiv_level : subdiv::device_level(1.0, max_edge, line_width, 0.0);
    const int render_level = (subdiv_level >= 0) ? subdiv_level : subdiv::device_level(1.0, max_edge, render_tol, 0.0);

    std::vector<Frame> frames(ncam); std::vector<std::vector<double>> zbufs(ncam); std::vector<Camera> cams(ncam);
    const double* qaz = quad_azimuths();
    for (int i = 0; i < ncam; ++i) {
        cams[i] = make_camera(ctr, rad, quad ? qaz[i] : 25.0, quad ? kQuadElevation : 15.0, RN);
        frames[i].W = frames[i].H = RN; frames[i].rgb.assign((size_t)RN*RN*3, (std::uint8_t)bg);
        zbufs[i].assign((size_t)RN*RN, -1e30);
    }

    // Slice each mesh -- Catmull-Clark through the memory-bounded band loop, other schemes whole (no
    // amplification blow-up) -- rendering each band into the shared frames, then merge per-layer segments.
    // Unique refined resolution (a CC face of n corners -> n*4^(level-1) quads; other schemes stay
    // as fan-triangulated faces). The band loop re-emits faces that span several bands, so this is
    // the real triangle count, not the banded workload.
    size_t unique_tri = 0;
    for (const auto& u : meshes) {
        size_t corners = 0; for (int c : u.counts) corners += (size_t)c;
        if (u.subdiv_scheme == "catmullClark" && slice_level >= 1)
            unique_tri += corners * (size_t(1) << (2 * (slice_level - 1))) * 2;
        else
            unique_tri += corners - 2 * u.counts.size();
    }

    std::vector<LayerSegs> layers(zs.size());
    size_t nbands_total = 0; bool any_banded = false;
    for (size_t i = 0; i < meshes.size(); ++i) {
        auto renderhook = [&](const Mesh& bm) {
            for (int ci = 0; ci < ncam; ++ci) render_into(frames[ci], zbufs[ci], bm, cams[ci], cout_k, true, true);
        };
        std::vector<LayerSegs> L;
        if (meshes[i].subdiv_scheme == "catmullClark") {
            int nb = std::max(1, usd_band_count(meshes[i], zs));
            any_banded = true;
            if (slice_level == render_level) {
                nbands_total += nb;
                L = amplify_usd_banded(meshes[i], shaders[i], slice_level, zs, nb, renderhook, /*do_slice*/ true);
            } else {  // the slice and the render want different dicing -> a pass each, each memory-bounded
                nbands_total += 2 * nb;
                L = amplify_usd_banded(meshes[i], shaders[i], slice_level, zs, nb, {}, true);
                amplify_usd_banded(meshes[i], shaders[i], render_level, zs, nb, renderhook, /*do_slice*/ false);
            }
        } else {
            Mesh m = amplify_usd(meshes[i], shaders[i], slice_level);
            renderhook(m);
            L = slice_mesh(m, zs);
        }
        for (size_t li = 0; li < zs.size(); ++li)
            layers[li].insert(layers[li].end(), L[li].begin(), L[li].end());
    }

    std::error_code ec; std::filesystem::create_directories(out, ec);
    if (stack)
        for (size_t li = 0; li < layers.size(); ++li) {
            Frame fr = win; rasterize_layer(fr, layers[li], aovk, srgb);
            char p[1024]; std::snprintf(p, sizeof(p), "%s/layer_%04zu.png", out.c_str(), li); write_png(p, fr);
        }
    Frame rnd = quad ? tile_quad(frames.data(), bg) : std::move(frames[0]);
    write_png(out + "/render.png", rnd);

    std::printf("printman: %s%zu layers%s%s -- slice L%d (%zu tri, bead %.2fmm), render L%d (%.3fmm/px) -> %s/\n",
                quad ? "4-up render + " : "render + ", zs.size(),
                any_banded ? (" via " + std::to_string(nbands_total) + " Z-bands (memory-bounded)").c_str() : " (materialized)",
                stack ? (" + stack (aov '" + aovname + "')").c_str() : "",
                slice_level, unique_tri, line_width, render_level, render_tol, out.c_str());
    return 0;
}

// USD REYES gate: slice the first mesh's banded path at band-count=1 and band-count=N and prove the
// per-layer Cout rasters are pixel-identical -- the memory-bounded band loop must not change output.
int run_usd_gate(const std::string& usd_in, const std::string& shaderdir, const std::string& osl_disp,
                 const std::string& osl_color, double reach, double layer, double ppm, int subdiv_level, int bands) {
    std::vector<UsdCage> meshes; std::string err; int skipped = 0;
    if (!load_usd_scene(usd_in, PRINTMAN_USD_PLUGINDIR, meshes, skipped, err)) { std::fprintf(stderr, "printman: %s\n", err.c_str()); return 1; }
    const UsdCage& uc = meshes.front();
    if (uc.subdiv_scheme != "catmullClark") { std::fprintf(stderr, "printman: gate needs a catmullClark mesh (got '%s')\n", uc.subdiv_scheme.c_str()); return 1; }

    std::vector<std::unique_ptr<Shader>> owned;
    std::vector<const Shader*> shaders;
    for (const UsdMaterial& mtl : uc.materials) {
        ShaderKind kind = ShaderKind::Flat;
        owned.push_back(make_shader(mtl, shaderdir, osl_disp, osl_color, reach, kind));
        shaders.push_back(owned.back().get());
    }
    int cout_k = 0; { auto an = shaders[0]->aov_names(); for (size_t k = 0; k < an.size(); ++k) if (an[k] == "Cout") cout_k = (int)k; }
    const int level = subdiv_level >= 0 ? subdiv_level : 3;   // fixed level for the self-test when auto

    double max_disp = 0; for (const Shader* s : shaders) max_disp = std::max(max_disp, s->max_reach());
    double zmin = 1e30, zmax = -1e30, x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (const auto& p : uc.points) { zmin = std::min(zmin, p[2]); zmax = std::max(zmax, p[2]); x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
    std::vector<double> zs; for (double z = zmin - max_disp + layer * 0.5; z < zmax + max_disp; z += layer) zs.push_back(z);
    double span = std::max(x1 - x0, y1 - y0) + 2 * max_disp;
    Frame win = window_for(span, span, (x0 + x1) / 2, (y0 + y1) / 2, ppm, 2);

    auto A = amplify_usd_banded(uc, shaders, level, zs, 1);
    auto B = amplify_usd_banded(uc, shaders, level, zs, bands);
    long worst = 0, total_diff = 0;
    for (size_t li = 0; li < zs.size(); ++li) {
        Frame fa = win, fb = win;
        rasterize_layer(fa, A[li], cout_k, true);
        rasterize_layer(fb, B[li], cout_k, true);
        long d = 0; for (size_t p = 0; p < fa.rgb.size(); ++p) d += (fa.rgb[p] != fb.rgb[p]);
        worst = std::max(worst, d); total_diff += d;
    }
    std::printf("usd-gate: band-count=1 vs band-count=%d over %zu layers @ %dx%d px (subdiv %d)\n",
                bands, zs.size(), win.W, win.H, level);
    std::printf("usd-gate: %s  (total differing subpixels=%ld, worst layer=%ld)\n",
                total_diff == 0 ? "PASS" : "FAIL", total_diff, worst);
    return total_diff == 0 ? 0 : 2;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string out = "out", aovname = "Cout", osl_disp, osl_color, shaderdir, usd_in;
    double layer = 0.1, ppm = 8.0, R = 20.0, amp = 6.0, reach = 0;
    int nu = 180, nv = 90, bands = 8, subdiv_level = -1, rres = 700;   // subdiv -1 = auto (per-output)
    double line_width = 0.4;
    bool gate = false, stack = false, quad = false, white = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--gate") gate = true;
        else if (a == "--stack") stack = true;
        else if (a == "--quad") quad = true;
        else if (a == "--white") white = true;
        else if (a == "--out") out = next();
        else if (a == "--aov") aovname = next();
        else if (a == "--layer") layer = std::atof(next().c_str());
        else if (a == "--res") ppm = std::atof(next().c_str());
        else if (a == "--radius") R = std::atof(next().c_str());
        else if (a == "--amp") amp = std::atof(next().c_str());
        else if (a == "--bands") bands = std::atoi(next().c_str());
        else if (a == "--rres") rres = std::atoi(next().c_str());
        else if (a == "--osl-disp") osl_disp = next();
        else if (a == "--osl-color") osl_color = next();
        else if (a == "--shaderdir") shaderdir = next();
        else if (a == "--reach") reach = std::atof(next().c_str());
        else if (a == "--subdiv-level") subdiv_level = std::atoi(next().c_str());
        else if (a == "--line-width") line_width = std::atof(next().c_str());
        else if (a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') usd_in = a;  // positional: a USD scene
    }

    int bg = (white || quad) ? 255 : 16;  // 4-up shots default to a white background
    if (!usd_in.empty()) {
#ifdef PRINTMAN_USD
        if (gate)
            return run_usd_gate(usd_in, shaderdir, osl_disp, osl_color, reach, layer, ppm, subdiv_level, bands);
        return run_usd(usd_in, out, aovname, layer, ppm, subdiv_level, shaderdir, osl_disp, osl_color, reach, amp, stack, quad, bg, rres, line_width);
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
