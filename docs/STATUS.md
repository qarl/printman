# printman — status (2026-08-18)

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
   via `--stack`. Any shader AOV rasterizes: `--aov Cout|N|disp|height`. `--quad` renders a 2×2
   contact sheet from four angles; `--white` (implied by `--quad`) uses a white background.
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
11. **Whole-scene load: every mesh, its own material + world transform** (`load_usd_scene`). Walks
    all Mesh prims (skips proxy/guide/invisible), bakes each mesh's `ComputeLocalToWorldTransform`,
    merges the amplified parts, drops the assembled model to the plate once. Material resolution is
    sturdy: `ComputeSurface/DisplacementSource`, the `preview`/`full` binding purposes before
    allPurpose, and a nested shading-group/GeomSubset-child fallback. Apple's 12-mesh robot prints
    whole, in its real colours.
12. **Multiple materials per mesh via `UsdGeomSubset`s.** A cage carries a material list + per-face
    index; the tag rides through subdivision (CC/bilinear one child per corner, loop corner-then-
    centre triangles), and each face is emitted with its own shader. Validated: a two-subset cube
    slices with both colours (a gradient across the fill) and holds through 3 Catmull-Clark levels.
13. **Full-REYES band loop for the USD path + its own invariance gate.** `amplify_usd_banded`
    (subdiv cages) and `amplify_poly_banded` (polygons) subdivide → displace → slice **one Z-band at
    a time and discard** — memory-bounded, never materializing the whole refined surface. Localized
    Catmull-Clark refinement (a core face + its 1-ring halo refines bit-identically to a whole-cage
    refinement) plus **halo-inclusive per-vertex normals** make displacement band-independent.
    `printman scene.usda --gate --bands N` proves **band-count=1 == band-count=N** pixel-identically
    (catmullClark meshes, 0 differing subpixels) — the USD counterpart of the sphere gate.
14. **Incremental per-band render.** `render_into` accumulates into a shared frame/z-buffer, so the
    preview composites band-by-band as the loop runs — the render never holds the whole surface either.
15. **Per-output auto-dicing** — each output picks its own shading rate. With `--subdiv-level` AUTO
    (default) the **slice** dices to the bead/extrusion width (`--line-width`, default 0.4 mm) and the
    **render** dices to ~1 framebuffer pixel (from `--rres`, default 700), via `device_level`.
    Different tolerances → two independent banded passes (slice + render); equal → one shared pass. A
    fixed `--subdiv-level N` overrides both. Layer width and bead width are genuinely different scales.
16. **Displacement on polygon meshes, RenderMan-style** (`subdivisionScheme = none`/`bilinear` with a
    displacement shader) — non-subdiv polygons are **diced into micropolygons and displaced**, not left
    flat. **Quad meshes dice adaptively** (`amplify_poly_banded`): each face splits to its own edge
    scale (`edge_segments(len, tol)`), crack-free — segment counts are **edge-intrinsic** so shared
    edges agree, boundary params snap to the shared count, and per-control-vertex normals are shared
    across faces so a displaced boundary matches on both sides. Verified watertight on a 2/18/20 mm quad
    strip (thin quad diced 8×64, wide 64×64, no gaps). Non-quad polygons use a **uniform bilinear** band
    loop.

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
printman scene.usda                                        # auto-dice: slice to bead, render to pixels
printman scene.usda --quad                                 # 2×2 contact sheet, four angles, white bg
printman scene.usda --shaderdir <dir> --stack              # + OSL shaders + per-layer slice PNG stack
printman scene.usda --line-width 0.3 --rres 1200           # override bead width / render resolution
printman scene.usda --osl-disp NAME --osl-color NAME       # force shaders onto the scene's material
printman --gate --bands 8                                  # sphere band-invariance self-test
printman scene.usda --gate --bands 8                       # USD band-invariance self-test (catmullClark)
```

## NEXT (in priority order — do NOT start mid-context; these are fresh units)

1. **Adaptive dicing for triangle / n-gon polygon meshes.** Today only all-quad polygon meshes dice
   adaptively; triangle/n-gon meshes fall back to a uniform bilinear level. Barycentric adaptive
   tessellation with the same crack-free discipline (edge-intrinsic segment counts, shared vertex
   normals, boundary snapping).
2. **Adaptive dicing for Catmull-Clark surfaces.** CC cages currently dice at one uniform level per
   output. True adaptive CC needs arbitrary-(u,v) limit-surface evaluation (OpenSubdiv-style) so each
   face refines to its own screen/bead scale — a bigger piece than the quad tessellator.
3. **UV-seam / material-boundary displacement weld.** Per-corner displacement can crack at
   *non-periodic* UV seams (earth is periodic, fine), and a boundary between two subsets with
   *different* displacement can gap (colour-only subsets are crack-free). Reference: the fork's
   per-face-average one-position-per-vertex weld.
4. **Texture fidelity follow-ups** (`texshader.hpp`): honor the `st` transform (scale/bias/rotate),
   wrap modes beyond repeat, and non-`st` primvar names. Albedo only by design (no roughness/metallic/normal).
5. **Colour→filament is a sink/host step** (design §5): the core carries albedo RGB (Cout AOV);
   FDM dither/quantize + J55 tank separation are per-sink, not built.
6. **Not started**: FDM contour export, J55 VoxelPrint bitmaps, resin `.sl1`, Orca/CuraEngine
   adapters, self-supporting clamp. See `DESIGN.md` §10–13.

## Vendored / copied
- `third_party/stb_image_write.h` (public domain).
- `include/printman/subdiv.hpp` — copied from the fork's `Format/USDSubdiv.hpp` (std-only,
  OpenSubdiv-validated), namespace `printman::subdiv`.
