// Validate the neutral colour-classify math. DeltaE2000 is checked against the canonical Sharma et
// al. (2005) reference pairs (Table 1), not against the fork -- so the core owns a provably-correct
// metric. Plus sRGB byte round-trip and a nearest-filament sanity check.
#include <cstdio>
#include <cmath>
#include <vector>
#include "printman/palette.hpp"
using namespace printman;

int main() {
    int fails = 0;
    // Sharma CIEDE2000 reference: Lab1, Lab2, expected dE00 (published to 4 decimals).
    struct P { LABColor a, b; double e; };
    const P ref[] = {
        {{50.0000,2.6772,-79.7751},{50.0000,0.0000,-82.7485},2.0425},
        {{50.0000,-1.3802,-84.2814},{50.0000,0.0000,-82.7485},1.0000},
        {{60.2574,-34.0099,36.2677},{60.4626,-34.1751,39.4387},1.2644},
        {{63.0109,-31.0961,-5.8663},{62.8187,-29.7946,-4.0864},1.2630},
        {{22.7233,20.0904,-46.6940},{23.0331,14.9730,-42.5619},2.0373},
        {{2.0776,0.0795,-1.1350},{0.9033,-0.0636,-0.5514},0.9082},
    };
    for (const auto& p : ref) {
        double got = delta_e(p.a, p.b);
        double err = std::fabs(got - p.e);
        bool ok = err < 1e-3;
        if (!ok) ++fails;
        printf("dE2000  got=%.4f  ref=%.4f  err=%.1e  %s\n", got, p.e, err, ok ? "ok" : "*** FAIL ***");
    }
    // sRGB byte round-trip must be exact for every byte.
    int rt = 0; for (int v = 0; v < 256; ++v) if (linear_to_srgb8(srgb8_to_linear((unsigned char)v)) != v) ++rt;
    printf("srgb round-trip mismatches: %d  %s\n", rt, rt == 0 ? "ok" : "*** FAIL ***");
    fails += (rt != 0);
    // nearest_filament: a near-red linear colour picks the red palette entry (index 0 here).
    std::vector<RGBColor> pal = {{220,30,30},{30,220,30},{30,30,220},{230,230,230}};
    int nf = nearest_filament(V3{{0.8,0.02,0.02}}, pal);
    bool nfo = (nf == 0);
    printf("nearest_filament(red)=%d (expect 0)  %s\n", nf, nfo ? "ok" : "*** FAIL ***");
    fails += !nfo;

    printf("%s\n", fails == 0 ? "PASS: colour-classify math validated" : "*** colour math FAILED ***");
    return fails ? 1 : 0;
}
