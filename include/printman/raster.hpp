#pragma once
//
// Rasterize one layer's segments to an RGB8 image via NON-ZERO winding scanline fill, colouring
// each pixel by a selected AOV channel interpolated across the span. Non-zero winding makes
// self-intersecting folds read as solid. Background is black (empty voxel).
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "printman/geom.hpp"
#include "printman/slicer.hpp"

namespace printman {

struct Frame {
    int W = 0, H = 0;
    double xmin = 0, ymin = 0, ppm = 1;  // pixel = (world - min) * ppm
    std::vector<std::uint8_t> rgb;       // W*H*3, row 0 = top (+y up)
};

inline std::uint8_t enc(double lin, bool srgb) {
    double c = std::min(1.0, std::max(0.0, lin));
    if (srgb) c = (c <= 0.0031308) ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return (std::uint8_t)std::lround(c * 255.0);
}

// Fill one layer's segments into fr, colouring by AOV channel k (srgb-encode if it's colour).
inline void rasterize_layer(Frame& fr, const LayerSegs& segs, int k, bool srgb) {
    struct X { double x; int w; V3 c; };
    for (int py = 0; py < fr.H; ++py) {
        double yc = fr.ymin + (fr.H - 1 - py + 0.5) / fr.ppm;  // +y world is up in the image
        std::vector<X> xs;
        for (const auto& s : segs) {
            double ay = s.a[1], by = s.b[1];
            bool up = by > ay;
            double y0 = up ? ay : by, y1 = up ? by : ay;
            if (yc < y0 || yc >= y1) continue;
            double t = (yc - ay) / (by - ay);
            double x = s.a[0] + (s.b[0] - s.a[0]) * t;
            const V3& ca = s.va[k]; const V3& cb = s.vb[k];
            V3 c{{ca[0] + (cb[0] - ca[0]) * t, ca[1] + (cb[1] - ca[1]) * t, ca[2] + (cb[2] - ca[2]) * t}};
            xs.push_back({x, up ? +1 : -1, c});
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end(), [](const X& a, const X& b) { return a.x < b.x; });
        int wind = 0;
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            wind += xs[i].w;
            if (wind == 0) continue;
            int pxa = (int)std::floor((xs[i].x - fr.xmin) * fr.ppm);
            int pxb = (int)std::ceil((xs[i + 1].x - fr.xmin) * fr.ppm);
            for (int px = std::max(0, pxa); px < std::min(fr.W, pxb); ++px) {
                double wx = fr.xmin + (px + 0.5) / fr.ppm;
                double span = xs[i + 1].x - xs[i].x;
                double f = span > 1e-9 ? (wx - xs[i].x) / span : 0.0;
                f = std::min(1.0, std::max(0.0, f));
                V3 c{{xs[i].c[0] + (xs[i + 1].c[0] - xs[i].c[0]) * f,
                      xs[i].c[1] + (xs[i + 1].c[1] - xs[i].c[1]) * f,
                      xs[i].c[2] + (xs[i + 1].c[2] - xs[i].c[2]) * f}};
                std::uint8_t* p = &fr.rgb[(py * fr.W + px) * 3];
                p[0] = enc(c[0], srgb); p[1] = enc(c[1], srgb); p[2] = enc(c[2], srgb);
            }
        }
    }
}

}  // namespace printman
