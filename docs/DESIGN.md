# PrintMan Core — Integrated Architecture

*2026-08-17. Integration of two designs: my clean-room re-derivation (**v2**, banded/REYES +
the RGB-fidelity correction) and the prior **`docs/LIBRARY.md`** (rev 2). They agree on the
spine and are complementary at the edges: LIBRARY is stronger on ecosystem/delivery/target
realism; v2 is stronger on band-loop correctness and colour-information flow. One fact
supersedes part of LIBRARY: it predates the **OSL** decision, which resolves its single largest
risk (the "unbuilt evaluator"). This doc merges them and marks provenance: [L]=from LIBRARY,
[v2]=from my re-derivation, [★]=update/new from integrating them.*

## 1. Goal

One host-independent PrintMan **core source**, delivered two ways: a standalone `printman` CLI
(the only vehicle for hostless targets, and the dev loop/oracle) and a library merged natively
into open C++ slicers. The per-host layer is **not** uniform — thin for the libslic3r family
(Prusa), a large port for Cura. USD is an unconditional dependency. [L]

## 2. The non-negotiable: banded (REYES-for-slicing), never a whole mesh [v2]

Amplification multiplies a coarse cage into detail no single mesh can hold. The only path is
**bound-and-split into Z-bands; per band: dice → displace → shade → slice → emit → discard.**
Bounded memory, arbitrary detail. A whole-mesh/3MF export is at most an optional, clearly-marked
convenience that betrays the principle — never the core path. (v1's "materialize a mesh" is
rejected; this was the correction that produced v2.)

## 3. The shape [L, refined by v2]

```
  +--------------------------------------------------------------+
  | PrintMan host-INDEPENDENT core (same source everywhere)      |
  |   pxr -> PrintManScene   |  BAND LOOP (Eigen+std+TBB):       |
  |   (pxr fenced here)      |   bound-split -> dice(+halo) ->    |
  |   subdiv evaluator       |   OSL displace -> OSL shade ->     |
  |   (zero-dep std)         |   slice(own 2D geom) -> partition  |
  |   OSL evaluator (edge)   |   -> emit LayerContours -> discard |
  +--------------------------------------------------------------+
        |                                   |
 PER-HOST: PrintManScene <- host model      | adapter primitives the loop calls:
 (libslic3r Model/…; Cura has none)         |  - slice(band_mesh, zs) -> contours
                                            |  - union/accumulate contours
                                            |  - receive per-layer output (pipeline | writer)
                                            |  - diagnostics sink (warn -> host UI)
                                            |  - threading context (whose TBB arena?)
```

Two things a naive "thin line above a finished core" hides [L]:
- **The importer is two pieces.** `pxr → PrintManScene` is host-independent; `PrintManScene →
  host-model` (builds libslic3r `Model`/`ModelVolume`, hooks load dispatch) is **per-host and
  does not port** — Cura has no `ModelVolume`.
- **The adapter is not thin.** Host slicer, 2D type, mesh container, threading, diagnostics all
  reach *into* the band loop. Porting = lifting those to adapter primitives.

**[★] Reconciliation:** for the **standalone**, the core supplies its *own* slice primitive (a
vendored raw-Clipper 2D layer + a compact tri-slicer, neutral types), so it needs no host at
all. For an **in-host** build, the adapter supplies the host's slicer instead. Same band loop,
two providers of the `slice`/`union` primitives. This is how "the standalone links zero
libslic3r" (v2's invariant) and "the library reuses the host's slicer" (L) coexist.

## 4. The band loop (core of the core) [v2, with L's seam facts]

Per Z-band:
1. **Bound-and-split** — select cage faces whose displaced reach can enter this band. Reach =
   cage extent grown by the shader's declared **maxReach** (§6).
2. **Dice, + one-ring halo** — refine to the dice rate carrying face-varying UV; dice the band's
   core faces *plus* a halo into neighbours. The halo makes displacement average over the same
   neighbourhood in adjacent bands → fixes the band-seam bug (§9). [v2]
3. **Displace** — OSL displacement per refined vertex along normal, per-face-average → one
   position per shared vertex (watertight), now band-invariant.
4. **Shade** — OSL colour per refined vertex → **albedo RGB** (unlit) [L]. No filament
   quantization here (§5).
5. **Slice** — the band mesh against the band's Z-planes via the core's own tri-slicer + 2D
   booleans, **non-zero / positive winding** so self-intersecting folds resolve to *solid
   union*, not floating islands [both: v2 symptom, L "positive-fill-rule union at slice time"].
6. **Partition** (FDM only) — claim the outer-wall shell to `line_width` depth, partition among
   filaments; base = leftover. `line_width` is a host-supplied scalar, not a host type [v2].
7. **Emit** the band's `LayerContours` (carrying albedo RGB, not yet filaments); optionally
   splat the band mesh into the render framebuffer; **discard**. Next band.

Only one band of geometry is ever live — the whole memory argument [L: band-streaming bound].

## 5. Colour: carry albedo RGB losslessly; quantize at the sink/host [★ — the key integration]

Both designs converge here; state it cleanly:
- The core carries the shader's **continuous albedo RGB** (OSL `Cout`) all the way to emit —
  **palette-free, lossless.** [v2]
- **Continuous→discrete is a per-sink/host step**, and it is inherently lossy [L: "the
  dithering step we must build", OpenVCAD centres on it]:
  - **FDM**: RGB → dither → filament, with the *host's* palette + line width; carried as vendor
    `mmu_segmentation`/`paint_color` (lossy, host-family-specific) [L, and the format review].
  - **PolyJet/J55**: RGB → per-tank dither to the voxel/bitmap sink (the faithful-colour
    target) [L]. This is *why* the core must not pre-quantize to 8 FDM filaments — the J55 wants
    the colour, not a filament index [v2, and the "without loss?" catch].
  - **Resin `.sl1`**: monochrome exposure — **not a colour target** [L]; geometry only.
- Colour space is pinned: OSL `Cout` is linear; palettes are sRGB; linearize once, match/dither
  in linear, encode to sRGB only at PNG/render write [v2 — the repeat sRGB bug].

## 6. Shading evaluator = OSL — LIBRARY's biggest risk is already retired [★]

LIBRARY's #1 risk was "the evaluator is the largest unbuilt subsystem": MaterialX `ShaderGen`
emits renderer *source*, not values, and the only turnkey network evaluator is a Hydra GPU bake
we reject (it lights). **That analysis predates Karl's decision to use OSL, not MaterialX**
("I don't want a network, I want a shader. OSL."). OSL's `ShadingSystem` **is** a shipped CPU
evaluator that returns values (`Disp`, `Cout`) per shading point — already integrated,
oracle-matched, TSan-clean. So:
- The evaluator is **built** (OSL), not a subproject to budget. LIBRARY's largest unknown is
  closed.
- `maxReach` (§6/§5.1 of v2) is a declared OSL shader parameter — the shader owns its own bound;
  the core validates per band and widens rather than dropping geometry (fixes the silent-drop
  bug).
- OSL lives at an **edge** (`printman-osl`, pulls OSL+OIIO); the core proper stays evaluator-
  agnostic behind a `Shader` interface, so a future GPU/Metal backend slots in.

## 7. Neutral types, the two invariants, and the Eigen/TBB reality [v2 + L]

- **Two invariants are the portability proof** [v2]: (1) the core exposes no host type; (2) the
  standalone links zero libslic3r. Enforce both as build assertions.
- **But the internal math is Eigen and TBB is a hard core dep** [L] — pxr is fenced to the
  importer; the subdiv evaluator is zero-dep std; the engine math is Eigen; TBB (`parallel_for`,
  `enumerable_thread_specific`, the load-bearing `task_arena::isolate()`) is woven in.
- **[★] Boundary vs internals:** the *boundary* uses neutral doubles (`V2/V3`), so a host need
  not vendor Eigen; but the shared dep across Orca/Prusa **and** Cura is **oneTBB, not Eigen**
  (Cura has no Eigen). Threading-arena ownership when the core runs inside a host's live TBB
  pipeline is an explicit **adapter contract**, not an assumption.
- The core owns its 2D geometry with a **private fixed** integer scale — never libslic3r's
  mutable global `SCALING_FACTOR` [v2]. Transient band mesh pinned to float for shared-corner
  watertightness [v2].

## 8. The adapter primitives [L, canonical]

A host provides: (1) `slice(band_mesh, zs) -> contours` — libslic3r has `slice_mesh_ex`;
**CuraEngine does not** (whole-mesh `Slicer`), it must synthesise it; (2) union/accumulate; (3)
hand per-layer output back before perimeters/infill, or to an image writer; (4) a **diagnostics
sink** (severity + route to host UI — today only `boost::log`); (5) a **threading contract**;
(6) the per-host `PrintManScene → host-model` construction. The standalone provides 1–3 itself
(vendored) and routes 4 to the CLI.

## 9. Geometry correctness — the realities to face [v2 + L]

- **Band-seam continuity** — cause is the *averaging domain*, not normals; fixed by halo dice
  (§4.2). Gate: **band-count=1 == band-count=N** contour equality before anything ships. [v2]
- **Self-intersection at high amplitude** — offsetting past local curvature radius folds the
  surface; non-zero/positive winding at slice makes folds solid, not floating islands [both]. A
  self-supporting/curvature clamp is a later, optional shader/config choice.
- **Manifoldness is the unstated precondition** of the crack-free (dice==slice) proof — add
  **import-time conditioning** (the parked torus weld) upstream of the band loop [L].
- **Per-vertex UV sampling** — evaluate the texture per refined vertex (not per-face centroid),
  derivatives driving the filter — kills the hillshade/DEM aliasing [v2].

## 10. Two builds of one core [L, refined]

- **`printman` standalone CLI** — USD → amplify → output; backends compile-gated by linked libs:
  vendored mesh slicer → **FDM contours / PNG stacks**; per-tank PNG/BMP → **J55 Voxel Print**
  bitmaps; libpng+zip → resin `.sl1` (mono); OpenVDB → `.vdb`. USD always in. The only path to
  hostless targets, plus the dev loop and oracle. Produces the **PNGs** (layer stacks + renders)
  Karl requires.
- **Library into a host** — same core; host supplies the slice primitive and consumes contours
  in its pipeline (Orca: inject at `slice_scene`/`apply_segmentation`, fed Orca's `Flow`).

Both requirements are first-class and share one engine: the band loop emits one neutral
`LayerContours` stream; the CLI rasterizes it to PNGs, the library injects it into a host.

## 11. Target matrix — corrected lineage [L]

- **libslic3r family** — Orca & Prusa share `ExPolygon`+Clipper+`TriangleMesh`+`Model`; per-host
  layer is thin, injection point near-identical between them. But **not** "land once, many free":
  Bambu is Orca's *parent* (closed to external PRs), QIDI forks Bambu; only **Creality Print**
  genuinely inherits an Orca change downstream (and still eats the OpenUSD build burden).
  SuperSlicer is a separate real port.
- **CuraEngine** — a **large port, not a thick adapter**: no `ExPolygon` model, no Eigen (oneTBB
  shared instead), no `ModelVolume`/import, no per-band slice primitive, Conan not CMake. Second
  tier, expensive, after the Prusa family.
- **Hostless (resin, PolyJet/J55, MJF)** — the standalone is the vehicle regardless.
- **Out:** Slic3r (moribund), JS/C# tools, closed sets.

## 12. Delivery & CI [L]

USD **always-on** (no `SLIC3R_USD` flag — the two-cache trap is settled against). Merged upstream
code is **host-owned and edited**, not a frozen vendored copy. "Same source" byte-identity covers
only the host-independent core subset; a per-host `.upstream-ref` sync-check lives in **PrintMan's
CI**, not coupling independent OSS projects' build health. The real gate is the **OpenUSD build**
(`deps/OpenUSD/OpenUSD.cmake`, PR #15123 — libslic3r-family template; Cura needs its own Conan
recipe). CI pillars are about dep *weight*: cached deps artifact, path-filtered dep-rebuild,
nightly full build. USD schema/ABI lifecycle across N hosts folds into `.upstream-ref`.

## 13. Sinks / technologies [L]

FDM contour sink (done in Orca); resin `.sl1` grayscale (mono; UVtools transcodes to walled
formats); **PolyJet J55 per-tank voxel bitmap → GrabCAD Voxel Print → `.GCVF`** (300×300 DPI,
18.75 µm Z, ≤5 tanks — the faithful-colour target); OpenVDB/NanoVDB voxel intermediate. SVX is
**dead** (Shapeways bankruptcy) — drop it as an interop target [v2/format review; supersedes L's
listing of `.svx` as a live field carrier]. Prior art to mine: **OpenVCAD** (PNG-voxel + VDB for
PolyJet, centres on the continuous→discrete dither) and the MIT voxel-printing method.

## 14. Risks (merged, re-ranked)

1. **Cura is a separate large port** — the portability thesis really means "the libslic3r
   family"; every shared-structure sub-claim fails on Cura. [L]
2. **Self-intersection / manifoldness** — extreme amplitude self-intersects (non-zero-winding
   handles it), and the crack-free proof needs manifold input (import-time weld). [v2+L]
3. **Voxel memory at J55 resolution is unquantified** — whole-stack O(10s GB); band-streaming
   covers FDM contours only; the voxel sink needs Z-major layer-streaming-with-eviction and may
   force the band loop wider. [L]
4. **Band-seam continuity** — must pass the band-count=1==N gate; widen halo if not. [v2]
5. **Threading-arena ownership** in a host's live TBB pipeline — undesigned; adapter contract. [L]
6. **Untrusted USD/OSL input** — bounded eval resource-limit + asset-path confinement. [L]
7. ~~Unbuilt evaluator~~ — **retired**: OSL is the evaluator. [★]

## 15. Open questions

- Adapter granularity: is `slice(mesh, zs) -> contours` the right cut, or does the **voxel raster
  sink** force the sink deeper into the band loop (memory)? [L]
- Non-CC schemes / plain meshes: today only CC bands amplify and only CC carries UV — make
  Loop/bilinear/plain first-class or reject them loudly. [v2]
- Adaptive dicing to printer resolution (true REYES) vs a fixed dice rate + a coarse preview rate.
  [v2]
- Colour: exact per-family FDM paint encoding and the palette-index↔extruder contract. [both]
- Smallest first extraction that proves the seam. [L]

## 16. First slice of work (once agreed — NOT yet)

Standalone-only, one path, zero libslic3r: USD → `PrintManScene` → **band loop** (bound-split →
dice+halo → OSL displace → OSL albedo → own non-zero-winding slicer → FDM wall-partition) →
**PNG layer stack + per-band-splat renders**. Gate on **band-count=1 == band-count=N** contour
equality first. The library/Orca adapter and the J55 voxel sink follow once the core boundary is
proven by the standalone.
