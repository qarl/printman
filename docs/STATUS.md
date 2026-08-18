# printman — status (2026-08-17)

Standalone geometry-amplification tool. Design in `DESIGN.md`. This tracks what's built vs next.

## Working (committed, `~/src/printman`, branch main)

1. **Repo + integrated design** (`DESIGN.md`, incl. AOVs).
2. **Neutral core**: `geom.hpp` (V2/V3/Ring/Region/LayerContours/ContourStack/Mesh), zero host types.
3. **REYES band loop + invariance gate** (`band.hpp`): bound-split → dice (per band) → displace →
   slice → emit → discard. `printman --gate --bands N` proves **band-count=1 == band-count=N**
   (PASS, pixel-identical). This is the design's keystone. The built-in sphere is now *only* the
   gate's controlled test surface — it is no longer a render/input mode.
4. **AOV PNG stacks + z-buffer render** (`slicer.hpp` own tri-slicer + non-zero winding fill in
   `raster.hpp`; `render.hpp`). A render is written by default; the per-layer PNG stack is opt-in
   via `--stack`. Any shader AOV rasterizes: `--aov Cout|N|disp|height`.
5. **Procedural planet shader** (`shader.hpp`) — zero-dep; drives the gate.
6. **OSL backend** (`osl.hpp`, option `PRINTMAN_OSL`) — real user OSL shaders drive it.
7. **USD front-end** (`usd.hpp`/`usd_read.cpp` + `subdiv.hpp`, option `PRINTMAN_USD`) — loads a
   Mesh + face-varying `st` + bound material, Catmull-Clark subdivides, amplifies.
8. **Renders any USD input** — `printman scene.usda`. The **combined build links `PRINTMAN_OSL`
   + `PRINTMAN_USD`** (no tbb/Imath clash at link or runtime), so a USD that binds our OSL shaders
   runs them (earth USD → its own `printman_earth*`). A material we can't evaluate is **not an
   error**: fall back per USD convention — our OSL shader (only if its `.oso` exists) → sampled
   UsdPreviewSurface `diffuseColor` texture → constant `diffuseColor` → `primvars:displayColor` →
   0.18 gray; always zero displacement.
9. **Up-axis normalized to Z at load** (`usd_read.cpp`). USD default is Y-up; Y-up assets are
   rotated +90° about X to PrintMan's Z-up slicing frame. `upAxis="Z"` assets untouched.
10. **UsdPreviewSurface `diffuseColor` textures sampled** (`texshader.hpp`, behind `PRINTMAN_OSL`
    for OIIO). Walk `diffuseColor` → `UsdUVTexture` → `file`; pull bytes via the USD asset resolver
    (`.usdz`-embedded or loose); decode with OIIO; sRGB→linear; sample per corner at `st`.
    Validated: Apple `teapot.usdz` (52k pts, PBR, Y-up) loads, stands upright, renders its baseColor.

## Build

```
cmake -S . -B build -G Ninja -DPRINTMAN_OSL=ON -DPRINTMAN_USD=ON   # canonical combined build
cmake --build build
```
- OSL: local build at `~/tmp/osl`; brew OIIO/Imath/fmt. Link `liboslexec.dylib` + OIIO(+_Util).
- USD: fork's monolithic install `.../OrcaSlicer_dep/usr/local`. **Must** `-Wl,-force_load,libusd_m.a`
  (else USD static registries are stripped → segfault) + all `libboost_*.a` + `libtbb.a` +
  `-framework CoreFoundation -framework Cocoa`; register plugins via
  `PlugRegistry::RegisterPlugins(<prefix>/lib/usd)` (env var alone does NOT work for monolithic).

## Usage

```
printman scene.usda --out out                              # render any USD (its own shaders/material)
printman scene.usda --shaderdir <dir> --subdiv-level 3 --stack   # + the OSL shaders + slice stack
printman --gate --bands 8                                  # band-invariance self-test
```

## NEXT (in priority order — do NOT start mid-context; these are fresh units)

1. **Per-band subdivision for the USD path.** `amplify_usd` materializes the whole refined cage
   (not memory-bounded). The gate/sphere path IS banded; port that structure to subdivided cages
   (dice only a band's faces + halo). Keep the gate green.
2. **Mesh local-to-world transform.** The loader reads a mesh's *local* points; a parent Xform
   (translate/rotate/scale) is ignored. Apply `ComputeLocalToWorldTransform` (composes cleanly with
   the up-axis step). Not needed for origin-centred assets (earth, teapot); will bite on arbitrary scenes.
3. **UV-seam displacement weld.** Per-corner displacement can crack at *non-periodic* UV seams
   (earth is periodic, fine). Reference: the fork's per-face-average one-position-per-vertex weld.
4. **Texture fidelity follow-ups** (`texshader.hpp`): honor the `st` transform (scale/bias/rotate),
   wrap modes beyond repeat, and non-`st` primvar names. Albedo only by design (no roughness/metallic/normal).
5. **Colour→filament is a sink/host step** (design §5): the core carries albedo RGB (Cout AOV);
   FDM dither/quantize + J55 tank separation are per-sink, not built.
6. **Not started**: FDM contour export, J55 VoxelPrint bitmaps, resin `.sl1`, Orca/CuraEngine
   adapters, adaptive dicing, self-supporting clamp. See `DESIGN.md` §10–13.

## Vendored / copied
- `third_party/stb_image_write.h` (public domain).
- `include/printman/subdiv.hpp` — copied from the fork's `Format/USDSubdiv.hpp` (std-only,
  OpenSubdiv-validated), namespace `printman::subdiv`.
