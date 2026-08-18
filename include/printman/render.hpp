#pragma once
//
// A preview render of the amplified mesh: orthographic projection, z-buffer, per-triangle
// barycentric fill, an AOV interpolated per pixel, optional lambert lighting from the geometry
// normal. Honest 3D of the real displaced geometry (silhouette shows the true relief). Colour
// AOVs are sRGB-encoded; data AOVs written raw.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "printman/geom.hpp"
#include "printman/raster.hpp"

namespace printman {

// An orthographic camera fixed to a bounding sphere (centre + radius), so many meshes -- e.g. the
// Z-bands of a REYES slice -- render into ONE frame with a shared framing instead of each finding
// its own. f is toward the camera; r/u the screen basis; scale maps world to pixels.
struct Camera { V3 c, f, r, u; double scale; int N; };

inline Camera make_camera(const V3& center, double radius, double az_deg, double el_deg, int N) {
    const double PI = 3.14159265358979323846;
    double az = az_deg * PI / 180, el = el_deg * PI / 180;
    V3 f{{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)}};
    V3 up{{0, 0, 1}};
    V3 r{{up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]}};
    double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); r = {{r[0] / rl, r[1] / rl, r[2] / rl}};
    V3 u{{f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]}};
    return Camera{center, f, r, u, (N * 0.46) / std::max(1e-6, radius), N};
}

// Rasterize a mesh INTO an existing frame + z-buffer (neither cleared), so bands accumulate.
inline void render_into(Frame& fr, std::vector<double>& zbuf, const Mesh& m, const Camera& cam,
                        int aov_k, bool srgb, bool lit) {
    const int N = cam.N;
    const V3 &c = cam.c, &r = cam.r, &u = cam.u, &f = cam.f;
    const double scale = cam.scale;
    auto proj = [&](const std::array<float, 3>& p, double& sx, double& sy, double& dep) {
        V3 q{{p[0] - c[0], p[1] - c[1], p[2] - c[2]}};
        sx = N * 0.5 + (q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) * scale;
        sy = N * 0.5 - (q[0] * u[0] + q[1] * u[1] + q[2] * u[2]) * scale;
        dep = q[0] * f[0] + q[1] * f[1] + q[2] * f[2];
    };
    V3 L{{-0.4, 0.5, 0.75}}; double ll = std::sqrt(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]); for (auto& x : L) x /= ll;
    for (const auto& t : m.tri) {
        double sx[3], sy[3], dep[3];
        for (int i = 0; i < 3; ++i) proj(m.pos[t[i]], sx[i], sy[i], dep[i]);
        int x0 = std::max(0, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
        int x1 = std::min(N - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
        int y0 = std::max(0, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
        int y1 = std::min(N - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::abs(area) < 1e-9) continue;
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px) {
                double w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                double w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                double d = w0 * dep[0] + w1 * dep[1] + w2 * dep[2];
                int idx = py * N + px;
                if (d <= zbuf[idx]) continue;
                zbuf[idx] = d;
                auto& a0 = m.aov[aov_k][t[0]]; auto& a1 = m.aov[aov_k][t[1]]; auto& a2 = m.aov[aov_k][t[2]];
                V3 col{{w0 * a0[0] + w1 * a1[0] + w2 * a2[0], w0 * a0[1] + w1 * a1[1] + w2 * a2[1], w0 * a0[2] + w1 * a1[2] + w2 * a2[2]}};
                double sh = 1.0;
                if (lit) {
                    auto& n0 = m.nrm[t[0]]; auto& n1 = m.nrm[t[1]]; auto& n2 = m.nrm[t[2]];
                    V3 nn{{w0 * n0[0] + w1 * n1[0] + w2 * n2[0], w0 * n0[1] + w1 * n1[1] + w2 * n2[1], w0 * n0[2] + w1 * n1[2] + w2 * n2[2]}};
                    double nl = std::sqrt(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]); if (nl > 1e-9) for (auto& x : nn) x /= nl;
                    sh = 0.35 + 0.75 * std::max(0.0, nn[0]*L[0]+nn[1]*L[1]+nn[2]*L[2]);
                }
                std::uint8_t* p = &fr.rgb[idx * 3];
                p[0] = enc(col[0] * sh, srgb); p[1] = enc(col[1] * sh, srgb); p[2] = enc(col[2] * sh, srgb);
            }
    }
}

// Bounding sphere of a mesh's positions.
inline void mesh_sphere(const Mesh& m, V3& c, double& rad) {
    c = {{0, 0, 0}}; for (auto& p : m.pos) { c[0] += p[0]; c[1] += p[1]; c[2] += p[2]; }
    for (auto& x : c) x /= std::max<size_t>(1, m.pos.size());
    rad = 0; for (auto& p : m.pos) { double dx = p[0]-c[0], dy = p[1]-c[1], dz = p[2]-c[2]; rad = std::max(rad, std::sqrt(dx*dx+dy*dy+dz*dz)); }
}

inline Frame render_mesh(const Mesh& m, int aov_k, bool srgb, bool lit, double az, double el, int N, int bg = 16) {
    V3 c; double rad; mesh_sphere(m, c, rad);
    Frame fr; fr.W = fr.H = N; fr.rgb.assign((size_t)N * N * 3, (std::uint8_t)bg);
    std::vector<double> zbuf((size_t)N * N, -1e30);
    render_into(fr, zbuf, m, make_camera(c, rad, az, el, N), aov_k, srgb, lit);
    return fr;
}

// The four turntable azimuths (90 deg apart) + elevation the 4-up sheet uses; shared so an
// incremental (per-band) renderer can build the same views.
inline const double* quad_azimuths() { static const double az[4] = {30, 120, 210, 300}; return az; }
constexpr double kQuadElevation = 18;

// Tile four same-size views 2x2 with a gutter of the background colour.
inline Frame tile_quad(const Frame v[4], int bg) {
    const int N = v[0].W;
    const int g = std::max(2, N / 40);
    const int W = 2 * N + 3 * g, H = 2 * N + 3 * g;
    Frame out; out.W = W; out.H = H; out.rgb.assign((size_t)W * H * 3, (std::uint8_t)bg);
    for (int i = 0; i < 4; ++i) {
        const int ox = g + (i % 2) * (N + g), oy = g + (i / 2) * (N + g);
        for (int y = 0; y < N; ++y)
            std::copy(&v[i].rgb[(size_t)y * N * 3], &v[i].rgb[(size_t)(y + 1) * N * 3],
                      &out.rgb[((size_t)(oy + y) * W + ox) * 3]);
    }
    return out;
}

// Four views tiled 2x2 -- a turntable contact sheet showing every side at once.
inline Frame render_quad(const Mesh& m, int aov_k, bool srgb, bool lit, int N, int bg) {
    const double* az = quad_azimuths();
    Frame v[4];
    for (int i = 0; i < 4; ++i) v[i] = render_mesh(m, aov_k, srgb, lit, az[i], kQuadElevation, N, bg);
    return tile_quad(v, bg);
}

}  // namespace printman
