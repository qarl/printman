# printman

### RenderMan for 3D printers.

You write a shader. `printman` turns a coarse surface into printable micro‑detail by *amplifying*
it — dicing the surface fine, running your shader to **displace** and **colour** every micro‑facet,
and slicing the result straight into per‑layer contours. It is Pixar's REYES pipeline pointed at a
print bed instead of a camera: **bound → split → dice → displace → shade → slice**.

The point is the shader. Relief, lattices, textures, colour fields, hypsometric planets — anything
you can express as *displacement along the normal* and *colour out* becomes geometry the printer
lays down, at a resolution no hand‑built mesh could hold.

```
  coarse USD cage  +  OSL displacement/colour shader
          │
          ▼   bound-and-split into Z-bands, one band in memory at a time
   ┌─────────────────────────────────────────────┐
   │  dice adaptively → displace → shade → slice  │   ← REYES, per band, then discard
   └─────────────────────────────────────────────┘
          │
          ▼
   per-layer contours  +  preview renders
```

## Why REYES

Amplification multiplies a coarse cage into more detail than any single mesh can hold, so the
engine never materialises the whole surface. It **bounds and splits the model into Z‑bands, and per
band dices → displaces → shades → slices → emits → discards** — bounded memory, arbitrary detail.

The correctness keystone is **band‑count invariance**: slicing in one band must be *pixel‑identical*
to slicing in N bands. Every feature is gated against it, so the memory bound is free of artifacts at
the seams.

## What it does today

- **USD front‑end.** Reads `UsdGeomMesh` (polygon and Catmull‑Clark cages, with semi‑sharp
  creases/corners), intrinsic gprims (Cube/Sphere/Cylinder/Cone), `metersPerUnit` scaling, up‑axis,
  and orientation — validated on load.
- **OSL shading.** Your compiled `.oso` displacement and surface shaders drive the relief and the
  colour (`Cout`). A built‑in procedural planet shader runs the pipeline with no OSL install.
- **Subdivision surfaces — device‑perfect smoothing.** A Catmull‑Clark cage becomes its true smooth
  limit surface (validated to float precision against OpenSubdiv, semi‑sharp creases and corners
  included). Crucially, each output dices that limit surface to *its own* resolution — the slice to
  the bead/extrusion width, the render to ~1 pixel — so the surface is as smooth as the device can
  actually print or show, never faceted by a fixed mesh you picked in advance. Dicing is adaptive:
  each control face refines to its own edge scale (fine where it needs to be, coarse where it
  doesn't), crack‑free across shared edges. **Any** cage works — a non‑quad one is made all‑quad by
  one Catmull‑Clark step, so its limit surface is unchanged.
- **Instancing — low‑memory model replication.** A prototype cage referenced by many scene‑graph
  placements is amplified *per instance* through the band loop, so replicating a model to arbitrary
  detail costs one band of geometry in memory — not N fully‑amplified meshes. Mirrored
  (negative‑determinant) placements are handled, with the winding flipped so the relief still points
  outward.
- **Displacement that respects the surface.** Subdivision surfaces displace along the smooth limit
  normal; polygon meshes use a **per‑face‑average** rule — evaluate the shader per incident face with
  that face's flat normal and average the displacement vectors at each shared vertex, so the mesh
  stays watertight and hard edges auto‑damp instead of folding over.
- **Colour → filament.** Continuous shader colour is carried losslessly, then classified to the
  nearest loaded filament by perceptual CIEDE2000, or dithered across the palette (Frank‑Wolfe over
  the simplex) so a spatial mix reproduces an out‑of‑gamut colour.
- **Outputs.** Preview PNG renders (composited band‑by‑band) and per‑layer slice contours ready for
  a slicer's perimeter → infill → support → G‑code pipeline, or a `--stack` of per‑layer PNGs.

## Two deliveries, one core

The engine is a **host‑independent core** (header‑first, standard C++ and its own neutral geometry
types — no host clipper, no `pxr`/`Eigen`/`TBB` leaking into the core headers). It ships two ways:

1. **The `printman` standalone CLI** — USD in, renders and contours out. The dev loop, the oracle,
   and the only vehicle for host‑less targets.
2. **A library merged natively into open C++ slicers** — OrcaSlicer first. The slicer supplies its
   slice primitive and consumes the per‑layer contours; the amplification engine is the same source.
   Getting the PrintMan capability *into a real slicer* is the deliverable; the standalone is the
   canonical engine it is built and validated against.

## Build

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest)          # the crack-free + band-invariance gates
```

The core and its gates are pure standard C++. The heavy front‑ends are compile‑gated edge modules:

```sh
cmake -S . -B build -DPRINTMAN_USD=ON -DPRINTMAN_OSL=ON \
      -DUSD_ROOT=/path/to/openusd -DOSL_ROOT=/path/to/OpenShadingLanguage
```

USD (OpenUSD) is the scene format; OSL (OpenShadingLanguage) is the shading backend.

## Run

```sh
printman scene.usda                         # auto-dice: slice to bead width, render to pixels
printman scene.usda --stack                 # also write the per-layer PNG slice stack
printman scene.usda --osl-disp NAME --osl-color NAME --shaderdir DIR
printman scene.usda --palette rrggbb,rrggbb,...   # quantize the colour preview to a filament set
printman --gate --bands 8                    # band-count invariance self-test
```

## Design

The architecture, the REYES‑for‑slicing rationale, and the colour‑information model are in
[`docs/DESIGN.md`](docs/DESIGN.md); current capabilities in [`docs/STATUS.md`](docs/STATUS.md).
