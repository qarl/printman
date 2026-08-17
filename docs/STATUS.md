# printman — status (2026-08-17)

Standalone geometry-amplification tool. Design in `DESIGN.md`. This tracks what's built vs next.

## Working (committed, `~/src/printman`, branch main)

1. **Repo + integrated design** (`DESIGN.md`, incl. AOVs).
2. **Neutral core**: `geom.hpp` (V2/V3/Ring/Region/LayerContours/ContourStack/Mesh), zero host types.
3. **REYES band loop + invariance gate** (`band.hpp`): bound-split → dice (per band) → displace →
   slice → emit → discard. `printman --gate --bands N` proves **band-count=1 == band-count=N**
   (PASS, pixel-identical; passes with the OSL earth shader too). This is the design's keystone.
4. **AOV PNG stacks + z-buffer render** (`slicer.hpp` own tri-slicer + non-zero winding fill in
   `raster.hpp`; `render.hpp`). Any shader AOV rasterizes: `--aov Cout|N|disp|height`.
5. **Procedural planet shader** (`shader.hpp`) — zero-dep, the default `--demo`.
6. **OSL backend** (`osl.hpp`, option `PRINTMAN_OSL`) — real user OSL shaders drive it. Earth
   disp+colour shaders + textures → coloured displaced globe as PNG stack + render, ~1.3s.
7. **USD front-end** (`usd.hpp`/`usd_read.cpp` + `subdiv.hpp`, option `PRINTMAN_USD`) — loads a
   Mesh + face-varying `st` + bound material (shader info:id, `printman:maxMagnitude`), Catmull-
   Clark subdivides (fork's validated evaluator), amplifies. `printman scene.usda` works.

## Build

```
cmake -S . -B build -G Ninja [-DPRINTMAN_OSL=ON] [-DPRINTMAN_USD=ON]
cmake --build build
```
- OSL: local build at `~/tmp/osl`; brew OIIO/Imath/fmt. Link `liboslexec.dylib` + OIIO(+_Util).
- USD: fork's monolithic install `.../OrcaSlicer_dep/usr/local`. **Must** `-Wl,-force_load,libusd_m.a`
  (else USD static registries are stripped → segfault) + all `libboost_*.a` + `libtbb.a` +
  `-framework CoreFoundation -framework Cocoa`; register plugins via
  `PlugRegistry::RegisterPlugins(<prefix>/lib/usd)` (env var alone does NOT work for monolithic).

## Usage

```
printman --demo --out out --aov Cout                       # procedural planet
printman --gate --bands 8                                  # band-invariance gate
printman --demo --osl-disp printman_earth_disp --osl-color printman_earth \
         --shaderdir <fork>/build/arm64/osl_shaders --reach 20 --radius 20 --out out
printman scene.usda --subdiv-level 3 --out out             # load + amplify a USD cage
```

## NEXT (in priority order — do NOT start mid-context; these are fresh units)

1. **USD + OSL together (untested link).** Today the USD path runs the *procedural* shader (or OSL
   separately); the USD material's OSL shader names are read but running them needs both deps
   linked at once. Risk: OSL's OIIO-tbb vs USD's tbb / Imath clash. Configure `-DPRINTMAN_OSL=ON
   -DPRINTMAN_USD=ON`, resolve any duplicate-tbb/Imath issue, then `printman scene.usda` uses the
   material's shaders → the *real* earth from its own USD.
2. **Per-band subdivision for the USD path.** `amplify_usd` materializes the whole refined cage
   (not memory-bounded). The `SphereCage` path IS banded; port that structure to subdivided cages
   (dice only a band's faces + halo). Keep the gate green.
3. **UV-seam displacement weld.** Per-corner displacement can crack at *non-periodic* UV seams
   (earth is periodic, fine). Reference: the fork's per-face-average one-position-per-vertex weld.
4. **Colour→filament is a sink/host step** (design §5): the core carries albedo RGB (Cout AOV);
   FDM dither/quantize + J55 tank separation are per-sink, not built.
5. **Not started**: FDM contour export, J55 VoxelPrint bitmaps, resin `.sl1`, Orca/CuraEngine
   adapters, adaptive dicing, self-supporting clamp. See `DESIGN.md` §10–13.

## Vendored / copied
- `third_party/stb_image_write.h` (public domain).
- `include/printman/subdiv.hpp` — copied from the fork's `Format/USDSubdiv.hpp` (std-only,
  OpenSubdiv-validated), namespace `printman::subdiv`.
