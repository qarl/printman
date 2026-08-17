#pragma once
//
// Shader interface: the complete definition of displacement AND of every output channel (AOV).
// Per the PrintMan rule, displacement is entirely the shader's (amplitude/gamma/etc. are its
// parameters). Beyond geometry, a shader emits an arbitrary set of named AOVs -- "Cout" (albedo)
// is just one; normals, the displacement field, custom masks, ids, scalar fields are equally
// first-class. The core carries them all; a sink picks which to emit.
//
#include <cmath>
#include <string>
#include <vector>

#include "printman/geom.hpp"

namespace printman {

struct Shader {
    virtual ~Shader() = default;
    // Displacement in mm along the surface normal. Drives geometry.
    virtual double displace(const V3& p, const V3& n, const V2& uv) const = 0;
    // The AOV channels this shader produces (order fixed for the run).
    virtual std::vector<std::string> aov_names() const = 0;
    // Fill out[k] (a vec3) for each aov_names()[k]. Scalar AOVs replicate into x,y,z.
    virtual void shade(const V3& p, const V3& n, const V2& uv, V3* out) const = 0;
    // The shader declares its own max displacement -- the bound the band loop needs to
    // bound-and-split before it evaluates.
    virtual double max_reach() const = 0;
};

// A procedural "planet": value-noise relief with hypsometric albedo. Emits several AOVs so the
// PNG stage can be exercised on colour, normals, and scalar fields alike.
struct ProceduralPlanet : Shader {
    double amp = 6.0;   // mm at the highest peak
    double sea = 0.52;  // relief below this -> flat ocean (~ earth land fraction)

    static double hash(long x, long y, long z) {
        unsigned h = (unsigned)(x * 374761393) ^ (unsigned)(y * 668265263) ^ (unsigned)(z * 2147483647);
        h = (h ^ (h >> 13)) * 1274126177u;
        return double((h ^ (h >> 16)) & 0xFFFFFFu) / double(0xFFFFFFu);
    }
    static double vnoise(double x, double y, double z) {
        long ix = (long)std::floor(x), iy = (long)std::floor(y), iz = (long)std::floor(z);
        double fx = x - ix, fy = y - iy, fz = z - iz;
        auto s = [](double t) { return t * t * (3 - 2 * t); };
        fx = s(fx); fy = s(fy); fz = s(fz);
        double c = 0;
        for (int o = 0; o < 8; ++o) {
            double wx = (o & 1) ? fx : 1 - fx, wy = (o & 2) ? fy : 1 - fy, wz = (o & 4) ? fz : 1 - fz;
            c += wx * wy * wz * hash(ix + (o & 1), iy + ((o >> 1) & 1), iz + ((o >> 2) & 1));
        }
        return c;
    }
    double relief(const V3& n) const {
        double f = 0, a = 0.5, s = 1.6;
        for (int oct = 0; oct < 5; ++oct) { f += a * vnoise(n[0] * s, n[1] * s, n[2] * s); a *= 0.5; s *= 2.03; }
        return f;
    }
    double land(const V3& n) const {
        double r = relief(n);
        return r > sea ? (r - sea) / (1.0 - sea) : 0.0;
    }
    static V3 mix(const V3& a, const V3& b, double t) {
        return V3{{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}};
    }
    V3 hypso(double h) const {
        const V3 ocean{{0.05, 0.20, 0.55}}, low{{0.15, 0.45, 0.15}}, high{{0.45, 0.35, 0.20}}, snow{{0.92, 0.92, 0.95}};
        if (h <= 0.0) return ocean;
        if (h < 0.35) return mix(low, high, h / 0.35);
        return mix(high, snow, std::min(1.0, (h - 0.35) / 0.5));
    }
    double displace(const V3&, const V3& n, const V2&) const override { return amp * land(n); }
    std::vector<std::string> aov_names() const override { return {"Cout", "N", "disp", "height"}; }
    void shade(const V3&, const V3& n, const V2&, V3* out) const override {
        double h = land(n);
        out[0] = hypso(h);                                                   // Cout: albedo
        out[1] = V3{{0.5 * (n[0] + 1), 0.5 * (n[1] + 1), 0.5 * (n[2] + 1)}}; // N: normal 0..1
        double d = (amp > 0 ? (amp * h) / amp : 0);                          // disp normalized 0..1
        out[2] = V3{{d, d, d}};
        out[3] = V3{{h, h, h}};                                             // height 0..1
    }
    double max_reach() const override { return amp; }
};

}  // namespace printman
