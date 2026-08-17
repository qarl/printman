#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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
        "  printman --demo [--out DIR] [--aov NAME] [--layer MM] [--res PXMM]\n"
        "                  [--radius MM] [--amp MM]\n"
        "  printman --help\n"
        "\n"
        "  --demo        procedural planet (stands in for a USD cage until the USD front-end lands)\n"
        "  --aov NAME    which shader output to rasterize into the PNG stack (default Cout)\n"
        "                planet AOVs: Cout (albedo), N (normal), disp, height\n"
        "  --layer MM    layer height (default 0.1)\n"
        "  --res PXMM    raster resolution, pixels per mm (default 8)\n");
}

// A blank frame covering the mesh XY extent.
Frame window(const Mesh& m, double ppm, int pad) {
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
    for (auto& p : m.pos) {
        xmin = std::min(xmin, (double)p[0]); xmax = std::max(xmax, (double)p[0]);
        ymin = std::min(ymin, (double)p[1]); ymax = std::max(ymax, (double)p[1]);
    }
    Frame fr;
    fr.ppm = ppm; fr.xmin = xmin - pad / ppm; fr.ymin = ymin - pad / ppm;
    fr.W = (int)std::ceil((xmax - xmin) * ppm) + 2 * pad;
    fr.H = (int)std::ceil((ymax - ymin) * ppm) + 2 * pad;
    fr.rgb.assign((size_t)fr.W * fr.H * 3, 0);
    return fr;
}
}  // namespace

int main(int argc, char** argv) {
    std::string out = "out", aovname = "Cout";
    double layer = 0.1, ppm = 8.0, R = 20.0, amp = 6.0;
    int nu = 180, nv = 90;
    bool demo = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--demo") demo = true;
        else if (a == "--out") out = next();
        else if (a == "--aov") aovname = next();
        else if (a == "--layer") layer = std::atof(next().c_str());
        else if (a == "--res") ppm = std::atof(next().c_str());
        else if (a == "--radius") R = std::atof(next().c_str());
        else if (a == "--amp") amp = std::atof(next().c_str());
        else if (a == "--help") { usage(); return 0; }
    }
    if (!demo) { usage(); return argc < 2 ? 1 : 0; }

    ProceduralPlanet sh; sh.amp = amp;
    Mesh m = dice_sphere(R, nu, nv, sh);
    int aovk = m.aov_index(aovname);
    if (aovk < 0) {
        std::fprintf(stderr, "printman: unknown aov '%s' (have:", aovname.c_str());
        for (auto& n : m.aov_names) std::fprintf(stderr, " %s", n.c_str());
        std::fprintf(stderr, ")\n");
        return 1;
    }
    bool srgb = (aovname == "Cout");

    double zlo, zhi; mesh_z_range(m, zlo, zhi);
    std::vector<double> zs;
    for (double z = layer * 0.5; z < zhi; z += layer) zs.push_back(z);  // layer centres
    auto layers = slice_mesh(m, zs);

    std::error_code ec; std::filesystem::create_directories(out, ec);
    Frame win = window(m, ppm, 2);
    for (size_t li = 0; li < layers.size(); ++li) {
        Frame fr = win;  // fresh black background
        rasterize_layer(fr, layers[li], aovk, srgb);
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/layer_%04zu.png", out.c_str(), li);
        write_png(path, fr);
    }
    Frame rnd = render_mesh(m, m.aov_index("Cout"), true, true, 25, 15, 700);
    write_png(out + "/render.png", rnd);

    std::printf("printman: %zu layers (aov '%s') + render -> %s/\n", layers.size(), aovname.c_str(), out.c_str());
    return 0;
}
