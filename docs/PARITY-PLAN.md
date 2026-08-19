# Bringing the standalone up to a superset of the fork — parity plan (rev. 2)

Status: PLAN rev. 2 (2026-08-19), after a 6-lens review of rev. 1 (gap-completeness, host-agnosticism,
oracle-diff, scope, colour-depth, regression). Decision (Karl, standing): **the standalone
`~/src/printman` is the source of truth**; close the gaps to the fork so it becomes a strict superset,
then the fork (and later the libslic3r family) adopts *from* it. Grounded at `printman@b28b582` /
`OrcaSlicer@e91af61948`, and the Python oracle at `~/project/printman` (`shade.py`/`harness.py`).

Rev. 1's biggest error: it treated validation as "diff each ported feature against the fork + Python
oracle at 1e-3." The review showed that reference is untrustworthy for exactly the two features that
matter most (normal model, colour) — the oracle *is* the fork's model, and 1e-3 conflates "faithful
port" with "deliberate redefinition." §5 fixes this.

## 1. Where the standalone already leads (confirmed; do not regress)

Adaptive per-face dicing (fork dices one uniform level/placement); pixel-exact band-invariance +
halo-inclusive band-independent normals (fork normals are per-band core-only, with the seam caveat it
flags at `Engine.cpp:405-411`); per-face multi-material via UsdGeomSubsets (the fork reads **no**
GeomSubsets — one material/prim); per-output auto-dicing + the CLI oracle. Parity absorbs the fork's
extras **without** losing these.

## 2. Stage 0 — build the regression net FIRST (foundational; was missing entirely)

The review found the standalone has **zero committed USD fixtures, no `add_test`, no CI** — both
"gates" are manual CLI runs and the sphere gate is procedural (no USD/creases/transforms). "Gate
stays green every stage" is currently fiction. Before any parity feature:

- Commit USD fixtures and wire an automated gate (`add_test` + a band-count sweep + a CI lane):
  a **creased** cage (crease near but not through a band boundary; a companion whose crease far
  endpoint lands just outside a mid-band 1-ring — see G2), an **instanced** set (non-uniform scale,
  shear, and a **mirror `det<0`**), and a **textured-displacement** face straddling a band boundary.
- Add a **signed-area / fill-parity** assertion to the gate (pixel-`Cout` equality alone will not
  catch an inverted layer from a mirrored placement — G1).
- (Decoupled, optional, the only Orca-facing value here:) port the standalone's **§1 leads** —
  adaptive dicing, band-invariance, per-face materials — into the fork now. This is a standalone→fork
  flow (the direction Karl wants) and needs none of G1–G7; it is what actually improves the
  deliverable while parity proceeds.

## 3. The gaps to close (revised)

### G7 — USD front-end parity (NEW; rev. 1 audited only the engine, not the reader)
The fork's `Format/USD.cpp` reads much the standalone's `usd_read.cpp` does not, and the fork would
*regress* these when it adopts from the standalone:
- **metersPerUnit → mm scaling** — highest impact; its absence is a silent 1000×-too-big bug. The
  standalone reads up-axis but not `metersPerUnit`.
- **Intrinsic gprim tessellation** (Cube/Sphere/Cylinder/Cone) — the fork notes 351/3142 files are
  gprim-only; the standalone reads only `UsdGeomMesh`, so those import empty.
- **Earliest time-sample reading** — the standalone reads `Default()`; Houdini/USD-ROP exports read
  back zero meshes.
- **Import validation** — NaN/inf, holeIndices, counts-vs-indices, out-of-range/overflow indices,
  degenerate-prim guards; the standalone reader has none.
- **Reader-side orientation** — `leftHanded` + `det<0` winding flip (the read half of the mirror
  problem in G1).
- **Instance-proxy reading** — `UsdTraverseInstanceProxies` + `GetPrimInPrototype` dedup (the read
  half of G1).

### G2 — creases (small LOC, but a real band-invariance trap)
`subdiv.hpp` already carries the full crease machinery and `build_region_subcage` already propagates
it; the port is ~fields on `UsdCage` + read the USD attrs + un-hardcode 3 `build_cage(...,{},{})`
sites + pass `max_crease` into device level. **But** the adaptive gate rests on "a CC patch is
supported by its 1-ring"; a semi-sharp crease of sharpness *s* propagates influence ~⌈s⌉ levels
**wider** than one ring, so a crease just outside a band's 1-ring is present at band=1 and dropped at
band=N → the core face's limit positions differ → **gate fails**. Requirement: `build_region_subcage`
must **widen the halo by ⌈max_crease⌉** around creased edges, and the device-level crease flooring
must stay **global** (a region-local crease max would give a multi-band face different segment counts
per band → crack). Gate: the creased fixture at multiple band counts.

### G1 — instancing (medium-large, structural, transform-correctness-critical)
A neutral scene = prototypes (local frame) + placements (`Transform3d`) + per-placement device level
(from the transform's largest singular value); build cage topology once per prototype. Correctness
the review flagged (the current world-baked reader is safe under any transform; keeping prototypes
local reintroduces all of this): **mirror `det<0`** must flip micropolygon winding (else the slicer
fills the complement — an inside-out layer); normals are **inverse-transpose**-covariant (displace
must use `M⁻ᵀ·N`, not `M·N`, under shear/anisotropy); band membership/`zs` are **world-Z**, so a
locally-kept prototype under rotation/shear needs its **world** hull for band selection or faces drop
→ band-seam cracks. Pairs with G7's instance-proxy read side. Gate: scale/shear/mirror fixtures,
N-placements area-equivalent to N bakes **plus** the fill-parity check.

### G3 — footprint derivatives (deferred; band-independence is the trap)
Widen `Shader::displace/shade` with a footprint (dPdu/dPdv or two edge vectors); feed OSL `sg.dPdx/
dPdy`. **Deferred:** its only payoff is OSL `texture()` anti-aliasing, and the standalone's shaders
are procedural (no textures) — nothing exercises it until a textured-colour fixture exists; it is
**not** a precursor to colour for procedural sources (rev. 1's Stage-2-before-colour gate was
gold-plating). When built: the footprint **must** come from **analytic lattice spacing / the
halo-inclusive refined region**, never finite-differenced against **emitted-core adjacency** — a
core-boundary micropolygon has a neighbour at band=1 that is absent at band=N, which would break the
gate at every band seam.

### G4 — colour → filament (the monster; re-architected per §4)
Bigger than G1+G2+G3 combined. The pipeline is three stages, and the split is the whole design:
- **Classify (into the core — the novel, host-neutral part):** Cout → filament via palette (linear→
  sRGB8→CIELAB **DeltaE2000**) `nearest_filament` or Frank-Wolfe `dither_filament`. This is pure
  scalar colour math (`Palette.hpp` is a near-verbatim port; only `RGBColor` is a trivial neutral
  swap). Carry the result as a **filament tag on the slicer's `Seg`** (the natural extension of the
  per-segment `Cout` it already carries band-invariantly). Also port the **exactly three** scene hints
  the fork reads as `inputs:printman:*` on the RESOLVED SURFACE shader (verified 2026-08-19 vs fork
  `USD.cpp:428-430` / `PrintManScene.hpp:66-92`): `objectSpace` (object-Z-normalized colour-input
  remap, 0=bottom-layer..1=top), `dither` (spatial dither across nearest filaments), `allFilaments`
  (dither across the whole loaded palette). The fork reads **NO** scene-authored filament palette —
  `resolved_filaments` derives from Orca's LOADED filament config at slice time, not the USD, so there
  is nothing palette-side to match. (The standalone already reads MORE on the fallback side —
  `primvars:displayColor` + `UsdPreviewSurface` `diffuseColor` — which the fork does not.) `dither`/
  `allFilaments` ARE the dither feature: **gated behind a physical swatch print** (preview != print),
  so do NOT build them before Karl validates a swatch. `objectSpace` is a deterministic input remap
  (no §4.2/print dependency) and could land independently if colour is prioritized.
- **Coverage / wall-claim (sink-native — do NOT put a clipper in the core):** the `wall_depth`
  outer-wall claim is **host-perimeter geometry** — its depth is the print's Flow
  (`0.5·ext.width + ext.spacing + peri.spacing·(loops−1)`), and it runs on ordered, disjoint 2D
  `offset_ex`/`diff_ex`/`intersection_ex`. It is FDM-specific and **undefined on a raster/J55 sink**
  (no perimeter). So it stays at the sink, fed a Flow-like depth *from* the sink. The core owns
  "which filament, at what field coverage"; the sink owns "claim the wall in its native 2D/voxel
  kernel." Do **not** produce `Region`/`ContourStack` + a neutral Clipper in Tier-0 (segment→loop
  assembly + offset-with-join-styles + boolean diff/intersect — a mini-Clipper, months, duplicates
  every host, and is the *wrong primitive* for the raster sink).
- **Realise:** the host/sink turns tagged segments + the wall-claim into per-filament contours
  (`out_segmentation`) in its own type.

Sequencing (host-agnosticism reviewer): land **classify-in-core now**; do the wall-claim **host-side
for FDM** for now (the standalone loses nothing — classification is the novel part); introduce a tiny
abstract `Clip2D` seam (≈union/offset/diff/intersect) **only when a second sink actually exists**
(YAGNI until there are two implementers).

### G6 — parallel/progress/cancel/cap → a SEAM, not engine work (demoted)
`tbb::parallel_for` in a Tier-0 header violates host-agnosticism. The fork already exposes progress/
cancel as `std::function` **seams** (`throw_on_cancel`, `report_progress`); the memory cap exists only
because the fork accumulates colour ribbons into shared cross-band buckets, whereas the standalone's
REYES loop writes **disjoint** per-band slots and holds one band at a time — it has no shared-
accumulation problem to cap. So G6 = a `std::function` progress/cancel pair + keep the (already
disjoint) band loop parallelizable. TBB and the cap are host concerns.

## 4. The two reconciliation DECISIONS — must be settled *before* building, not diff-gated

These are genuine behavioural forks, not sub-tolerance nuances, and the oracle must be rewritten to
the chosen model afterward:

1. **Displacement normal model — SETTLED (Karl, 2026-08-19), and implemented at `printman@67c46ee`.**
   The rev.1 framing (smooth *vs* per-face-average, one must win) was a false dichotomy. Karl's rule
   keys on surface type: a **subdivision surface is smooth**, so the CC path (`amplify_usd_adaptive`)
   keeps the halo-inclusive **limit normal**; a **simple polygon mesh is faceted**, so the poly path
   (`amplify_poly_banded`) uses the **per-face-average of `d·n` at the flat face normal** — weld the
   diced micro-mesh by position, shade each micro-triangle at its corners with that triangle's flat
   normal, average the displacement vectors per shared vertex (auto-damping at concave/hard edges).
   Both stay **band-invariant**: the poly path adds a 1-ring halo so a shared vertex's average is
   band-independent (gate `poly_face_average` proves damping + band1==bandN). So band-independence and
   edge-damping are **not** in tension — we get both. Oracle: **no rewrite needed** — `harness.py`
   already implements per-face-average (flat per-face normals, averaged `d·n`, `harness.py:55-68`), so
   the new poly path already matches it; the only remaining oracle work would be a *committed*
   poly-displacement diff case (the rule agrees; coverage is what's missing).
2. **Cout sampling: per-face centroid (fork) vs per-vertex/per-segment (standalone).** Load-bearing:
   it relocates which filament wins the sub-line-width outer wall — exactly the sphere/cube coverage
   that was tuned. The architecture review argues per-segment is the neutral-native, more-precise
   form; but it must be an explicit decision with a print-validated reference (below), not a side
   effect of the existing AOV.

## 5. Validation strategy (rev. 1's was broken)

- **Separate two gate kinds.** For a feature being *faithfully ported* (creases, instancing geometry,
  gprim reading), gate a **tight** area-equivalence (1e-3) against a trustworthy reference. For a
  feature being *deliberately redefined* (the §4 normal/Cout decisions), there is a **sanctioned
  reconciliation delta** — do not read it as a regression, and pin the *new* behaviour with its own
  golden. One 1e-3 number cannot do both jobs.
- **Rewrite the oracle to the chosen models.** `harness.py` currently encodes the fork's normal rule
  and has **no** displacement-crease-or-colour coverage beyond instancing geometry; once §4.1 is
  decided, rewrite it, and add crease + displacement cases.
- **Colour needs an independent, print-validated golden.** `shade.py` has no colour and the fork has
  only qualitative directional checks (no area golden). Author the G4 reference from a
  **print-validated** fixture, not by freezing either engine's live output (which would silently
  pre-decide §4.2). Because the standalone dices adaptively and the fork uniformly (plus the fork's
  8-bit sRGB match bottleneck), per-channel area **cannot** be area-equivalent to the fork by
  construction — so the fork is explicitly *not* the colour oracle.

## 6. Cross-cutting invariants (every stage)

- The band-invariance gate and crack-free harnesses stay green — with the **new fixtures** (Stage 0)
  that actually exercise creases/transforms/textures; the old gate is blind to all of them.
- Tier-0 stays host-agnostic: no `ExPolygon`/`union_ex`/pxr/Eigen/TBB in core headers. New capability
  enters as neutral types (a `Seg` filament tag), fenced edge modules, or `std::function` seams.

## 7. Sequencing

0. **Regression net** (§2) — fixtures + `add_test` + CI + fill-parity check. Optionally ship §1 leads
   to the fork in parallel (decoupled, the Orca-facing value).
1. **G7 (reader) + G2 (creases) + G1 (instancing)** — *minimal parity to slice real USD correctly*;
   the highest-value, correctness-critical block. Gate on the new fixtures.
2. **§4.1 normal decision** — SETTLED + implemented (`printman@67c46ee`); still owe the oracle rewrite.
3. **G4 colour** — classify-in-core + host-side wall-claim; blocked on §4.2 + a print-validated
   golden. G3 footprint folded in **only** when a textured-colour fixture appears.
4. **G6 seam + G5 weld-check** — cheap, any time.
5. Then `PACKAGING.md` realigns to standalone-canonical (the fork adopts the now-superset engine);
   vendoring gets an honest generator/rename form (no verbatim, no external-repo CI gate).

## 8. Decisions for Karl

1. **Normal model (§4.1)** — SETTLED by you 2026-08-19 (subdivs smooth-limit, simple meshes
   per-face-average) and now implemented + gated. No longer open.
2. **Cout sampling (§4.2)** — per-segment (neutral-native, my lean) vs per-face centroid.
3. **Parity scope** — minimal (G7+G2+G1: slice real models correctly) now, with colour (G4) deferred?
   Or is colour in the near-term parity target?
4. **Ship §1 leads to the fork now** (Stage 0, decoupled) — yes/no. It's the only near-term
   Orca-facing value while parity proceeds.
