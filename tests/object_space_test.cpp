// Object-space colour remap (inputs:printman:objectSpace parity). ObjectSpaceColor wraps a shader so
// the COLOUR (shade) sees object-normalized Z -- z mapped to [0,1] across the object's layer range,
// 0 at the bottom layer and 1 at the top -- while displacement still sees the true world point. This
// checks the fork's exact remap (PrintObjectSlice.cpp): shade's p.z becomes (z - z0)/(z1 - z0), x/y
// and the normal/uv pass through, and displace/max_reach/AOV names are untouched. Std-only.
#include <cmath>
#include <cstdio>
#include <vector>

#include "printman/geom.hpp"
#include "printman/shader.hpp"

using namespace printman;

// A probe whose Cout encodes the height it was handed (Cout = {p.z, p.z, p.z}) and whose displacement
// is the raw world z -- so we can read back exactly which z each terminal received.
struct ZProbe : Shader {
    double displace(const V3& p, const V3&, const V2&) const override { return p[2]; }
    std::vector<std::string> aov_names() const override { return {"Cout"}; }
    void shade(const V3& p, const V3&, const V2&, V3* out) const override { out[0] = V3{{p[2], p[2], p[2]}}; }
    double max_reach() const override { return 7.0; }
};

static bool close(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    int fails = 0;
    ZProbe probe;
    const double z0 = 5.0, z1 = 25.0;                 // object layer range spans 20 mm
    ObjectSpaceColor os(&probe, z0, z1);
    const V3 n{{0, 0, 1}}; const V2 uv{{0, 0}};

    // shade: p.z is remapped to (z - z0)/(z1 - z0). Bottom -> 0, top -> 1, midpoint -> 0.5, and a
    // world point ABOVE the top (displaced relief) maps past 1 -- linear, uniform over the object.
    struct Case { double zin, want; } cases[] = {{5.0, 0.0}, {25.0, 1.0}, {15.0, 0.5}, {10.0, 0.25}, {30.0, 1.25}};
    for (const auto& c : cases) {
        V3 out; os.shade(V3{{3.0, 4.0, c.zin}}, n, uv, &out);
        bool ok = close(out[0], c.want);
        if (!ok) ++fails;
        std::printf("shade z=%.1f -> Cout=%.4f (want %.4f)  %s\n", c.zin, out[0], c.want, ok ? "ok" : "*** FAIL ***");
    }

    // displace is UNTOUCHED: it still sees the true world z (relief is world-space).
    double d = os.displace(V3{{3.0, 4.0, 15.0}}, n, uv);
    bool dok = close(d, 15.0);
    std::printf("displace z=15 -> %.4f (want 15, world space)  %s\n", d, dok ? "ok" : "*** FAIL ***");
    fails += !dok;

    // max_reach and AOV names pass through unchanged (band sizing + schema must not shift).
    bool mok = close(os.max_reach(), 7.0) && os.aov_names().size() == 1 && os.aov_names()[0] == "Cout";
    std::printf("passthrough: max_reach=%.1f aovs=%zu  %s\n", os.max_reach(), os.aov_names().size(), mok ? "ok" : "*** FAIL ***");
    fails += !mok;

    // Degenerate range (z1==z0): inv clamps to 0, so every height maps to 0 (no divide-by-zero).
    ObjectSpaceColor flat(&probe, 5.0, 5.0);
    V3 fo; flat.shade(V3{{0, 0, 99.0}}, n, uv, &fo);
    bool fok = close(fo[0], 0.0);
    std::printf("degenerate range -> Cout=%.4f (want 0, no NaN)  %s\n", fo[0], fok ? "ok" : "*** FAIL ***");
    fails += !fok;

    std::printf("%s\n", fails == 0 ? "PASS: object-space colour remaps Z, leaves relief in world space" : "*** object-space FAILED ***");
    return fails ? 1 : 0;
}
