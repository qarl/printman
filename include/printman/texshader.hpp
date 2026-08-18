#pragma once
//
// TextureShader: honor a UsdPreviewSurface diffuseColor that is painted by a texture. The loader
// (usd_read.cpp) resolves the encoded image bytes out of the .usdz / off disk; here OIIO decodes
// them once into a linear RGB buffer and we sample it per vertex at the surface's st. Albedo only
// (Cout) -- PrintMan colours slices, it is not a full UsdPreviewSurface renderer -- and no
// displacement (interchange assets author none). OIIO is linked with the OSL backend, so this
// lives behind PRINTMAN_OSL; without it a textured material falls back to a flat colour.
//
#include <cmath>
#include <string>
#include <vector>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

#include "printman/geom.hpp"
#include "printman/shader.hpp"

namespace printman {

struct TextureShader : Shader {
    int W = 0, H = 0;
    std::vector<float> pix;  // W*H*3, linear

    // bytes: encoded image (png/jpg/exr/...). name: gives OIIO the format via its extension.
    TextureShader(const std::vector<unsigned char>& bytes, const std::string& name, bool srgb) {
        OIIO::Filesystem::IOMemReader mem(OIIO::cspan<unsigned char>(bytes.data(), (long)bytes.size()));
        std::string hint = name.empty() ? "tex.png" : name;
        OIIO::ImageBuf buf(hint, 0, 0, nullptr, nullptr, &mem);
        if (!buf.read(0, 0, 0, 3, /*force*/ true, OIIO::TypeDesc::FLOAT)) return;
        const OIIO::ImageSpec& s = buf.spec();
        W = s.width; H = s.height;
        pix.assign((size_t)W * H * 3, 0.0f);
        // first 3 channels, top-down rows, into a tight RGB float buffer
        if (!buf.get_pixels(OIIO::ROI(0, W, 0, H, 0, 1, 0, 3), OIIO::TypeDesc::FLOAT, pix.data())) {
            W = H = 0; pix.clear(); return;
        }
        if (srgb)
            for (float& c : pix) c = (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    bool ok() const { return W > 0 && H > 0 && !pix.empty(); }

    V3 sample(double u, double v) const {
        u -= std::floor(u); v -= std::floor(v);          // repeat wrap
        double x = u * W - 0.5, y = (1.0 - v) * H - 0.5;  // st origin is bottom-left; rows top-down
        int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        double fx = x - x0, fy = y - y0;
        auto at = [&](int xi, int yi, int k) -> double {
            xi = ((xi % W) + W) % W; yi = ((yi % H) + H) % H;
            return pix[((size_t)yi * W + xi) * 3 + k];
        };
        V3 out;
        for (int k = 0; k < 3; ++k) {
            double a = at(x0, y0, k) * (1 - fx) + at(x0 + 1, y0, k) * fx;
            double b = at(x0, y0 + 1, k) * (1 - fx) + at(x0 + 1, y0 + 1, k) * fx;
            out[k] = a * (1 - fy) + b * fy;
        }
        return out;
    }

    double displace(const V3&, const V3&, const V2&) const override { return 0.0; }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3&, const V3&, const V2& uv, V3* out) const override { out[0] = sample(uv[0], uv[1]); }
    double max_reach() const override { return 0.0; }
};

}  // namespace printman
