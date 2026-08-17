#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "printman/band.hpp"
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
        "                  [--radius MM] [--amp MM] [--bands N]\n"
        "  printman --gate [--bands N]      verify band-count=1 == band-count=N (design gate)\n"
        "  printman --help\n"
        "\n"
        "  --aov NAME    shader output to rasterize per layer (default Cout); planet AOVs:\n"
        "                Cout (albedo), N (normal), disp, height\n"
        "  --bands N     number of memory-bounded Z-bands (default 8)\n");
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
}  // namespace

int main(int argc, char** argv) {
    std::string out = "out", aovname = "Cout";
    double layer = 0.1, ppm = 8.0, R = 20.0, amp = 6.0;
    int nu = 180, nv = 90, bands = 8;
    bool demo = false, gate = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--demo") demo = true;
        else if (a == "--gate") gate = true;
        else if (a == "--out") out = next();
        else if (a == "--aov") aovname = next();
        else if (a == "--layer") layer = std::atof(next().c_str());
        else if (a == "--res") ppm = std::atof(next().c_str());
        else if (a == "--radius") R = std::atof(next().c_str());
        else if (a == "--amp") amp = std::atof(next().c_str());
        else if (a == "--bands") bands = std::atoi(next().c_str());
        else if (a == "--help") { usage(); return 0; }
    }
    if (!demo && !gate) { usage(); return argc < 2 ? 1 : 0; }

    ProceduralPlanet sh; sh.amp = amp;
    SphereCage cage(R, nu, nv, sh);
    std::vector<double> zs;
    for (double z = layer * 0.5; z < cage.top; z += layer) zs.push_back(z);

    // XY window: the object spans ~2*(R+amp); centre at origin (cage is centred in XY).
    double span = 2 * (R + amp);
    Frame win = window_for(span, span, 0, 0, ppm, 2);
    int aovk = 0; for (size_t k = 0; k < cage.aov_names.size(); ++k) if (cage.aov_names[k] == aovname) aovk = (int)k;
    bool srgb = (aovname == "Cout");

    if (gate) {
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

    // demo: banded PNG stack for the selected AOV + a whole-mesh preview render
    auto layers = amplify_banded(cage, zs, bands);
    std::error_code ec; std::filesystem::create_directories(out, ec);
    for (size_t li = 0; li < layers.size(); ++li) {
        Frame fr = win;
        rasterize_layer(fr, layers[li], aovk, srgb);
        char path[1024]; std::snprintf(path, sizeof(path), "%s/layer_%04zu.png", out.c_str(), li);
        write_png(path, fr);
    }
    Mesh m = dice_sphere(R, nu, nv, sh);
    Frame rnd = render_mesh(m, m.aov_index("Cout"), true, true, 25, 15, 700);
    write_png(out + "/render.png", rnd);

    std::printf("printman: %zu layers (aov '%s', %d bands) + render -> %s/\n",
                layers.size(), aovname.c_str(), bands, out.c_str());
    return 0;
}
