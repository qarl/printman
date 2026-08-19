# Landing printman's engine improvements into OrcaSlicer — the plan (rev. 2)

Status: **SUPERSEDED ON DIRECTION (2026-08-19).** This rev. 2 concluded "the fork stays canonical;
direct-port the improvements in" — adopting the review's strategic lens. **Karl overruled that**: the
**standalone is the source of truth**, and *in the end the fork will be entirely rewritten to use the
standalone's engine* — once the standalone is a dead-right **superset** of the fork (see
`PARITY-PLAN.md`). So the source-of-truth direction argued below (§0's "converge onto the standalone is
backwards", §5 "the fork is canonical", §7) is **RETRACTED**. What stays valid and is carried forward:
(a) the **capability-gap analysis** — the fork engine features the standalone lacks — which now drives
`PARITY-PLAN.md`; and (b) the **Orca integration surface** (§6 — the `slice_scene`→`ExPolygons` seam,
the `SLIC3R_USD` opt-out, the "why merge not bake" upstream argument), which is how the rewritten fork
will eventually host the standalone's engine. Do not read this doc's fork-canonical conclusion as
current. Original rev.-1/rev.-2 history retained below for the gap analysis only.

Grounded against `~/src/printman` @ `b28b582` and `~/src/OrcaSlicer` @ `e91af61948`
(branch `feature/usd-displacement`).

## 0. The correction (read first)

Rev. 1 proposed making the standalone the **single source of truth**, vendoring its core into the
fork, refactoring the fork's `Engine.cpp` into a thin adapter, and policing sync with a CI diff-gate.
Ten reviews converged that this is backwards on three counts:

1. **The fork's engine is *richer* than the standalone core, not poorer.** `src/libslic3r/PrintMan/`
   already does instancing (build-once prototype + N `Transform3d` placements, per-placement device
   level via the world map's largest singular value), semi-sharp **creases/corners/boundary/
   triangleSubdivisionRule**, **footprint derivatives** (`eval_d`, `dPdx/dPdy` → texture band-
   limiting), **per-filament colour segmentation** (`out_segmentation`, palette DeltaE2000, dither),
   and TBB-parallel slicing with progress/cancel/memory-cap. The standalone core expresses **none**
   of these (its `UsdCage` has no crease fields and its amplify path hard-codes `build_cage(...,{},{},
   {},{},{},EDGE_AND_CORNER,false,...)`; its `Shader` has no derivative slot; it has no placement/
   transform concept; it emits an *unwelded* soup and raw `LayerSegs`, never assembling the
   `ContourStack` its own header declares). Vendoring the core *in* would **regress** all of that.
2. **The sync apparatus is unmergeable and mis-scoped.** `~/src/printman` has no git remote, so a
   fork CI diff-gate against a pinned SHA cannot run; libslic3r vendors plain in-tree copies (glad,
   libvgcode, Clipper) with no external pin; "verbatim copy" contradicts the required `Slic3r::
   PrintMan` namespace + include-guard rewrite; and a symmetric diff cannot enforce *which* side is
   authoritative. It is also the standing cross-repo tooling tax that is out of scope for a one-host,
   one-improvement task.
3. **It inverts the settled principle.** The standalone is the incidental oracle/dev-loop; Orca is
   the deliverable. "Engine changes land in the standalone first" makes the incidental artifact the
   mandatory upstream of the product, and §9's "same core serves Prusa/Cura" imported the *parked*
   multi-host library ambition under the banner "minimum patchwork."

**The actual minimum-patchwork task:** get the two engine improvements the standalone proved this
session — **adaptive dicing** (per-face polygon center-split + adaptive Catmull-Clark) and the
**halo-inclusive band-independent normals** (which also fix the fork's own band-seam caveat,
`Engine.cpp:405-411`) — **into the fork's existing engine**, in-tree, preserving everything the fork
already does. The fork stays canonical; the standalone stays the oracle.

## 1. Scope

- **In:** land adaptive dicing + halo-inclusive normals in `src/libslic3r/PrintMan/`, as a direct
  port into the existing band loop. Reconcile the one shared file (the subdivider). Add the end-to-end
  gates the current suite is missing.
- **Out:** the three-tier vendored-core architecture, a `SliceBand` callback rewrite, `UsdCage`↔
  `SubdivCage` unification, `.upstream-ref`/`sync-printman.sh`/CI-diff-gate, and any Prusa/Cura work.
  All parked (§7, §8).

## 2. What is shared, and what is not (measured, not assumed)

- **Shared, genuinely: the subdivider.** `printman/subdiv.hpp` and the fork's `Format/USDSubdiv.hpp`
  are byte-identical (`subdivide`, `device_level`, `build_cage`, `refined_limit_aabb`, crease/corner
  rules) **except** one pure-function extraction: the standalone factored `build_region_subcage()`
  out of `refine_region()`. This is the only reconcilable unit, and the reconciliation is a 15-line
  factor — behavior-preserving, so the fork's 14-case OpenSubdiv golden stays bit-identical.
- **Not shared: the engine, the scene struct, the shader interface, colour, instancing.** The fork's
  `slice_cage_placement` and the standalone's `amplify_usd_*` are different computations even in
  uniform mode (world/welded/triangle-normal vs local/unwelded/quad-Newell-normal). `SubdivCage`
  (creases, boundary, winding, refined AABB) and `UsdCage` (materials, up_axis) each carry ~6 fields
  the other lacks; neither is a superset. The fork's colour path is ~130 lines of libslic3r-2D-boolean
  code with no core analogue. Do not try to unify these — port *into* them.

## 3. The direct port (~200–300 lines, one call site)

1. **Reconcile the subdivider in-tree.** Factor `build_region_subcage()` out of `refine_region()` in
   the fork's `Format/USDSubdiv.hpp` (adopt the standalone's split verbatim, in `Slic3r::usd_subdiv`,
   no rename). *Gate:* the 14-case OpenSubdiv golden + `test_usd.cpp` structural asserts unchanged.
2. **Add the adaptive tessellator beside the fork's `triangulate_cage`.** Port the self-contained
   adaptive-grid logic from `band_usd.hpp` — the parametric-`fvar` **template**, the per-control-edge
   `side_seg`, the boundary `snap`, and the grid emit — into a new `amplify_cage_adaptive(region, …)
   -> indexed_triangle_set` (CC) and the polygon center-split path (`amplify_poly_banded`) for
   `scheme=none`/`bilinear`. **Critical:** thread the fork's crease/corner/boundary/triangle_smooth/
   flip_winding through `build_cage` exactly as the fork's uniform path does (`Engine.cpp:118-121`) —
   do **not** copy the standalone's hard-coded-empty crease args. Emit the fork's welded
   `indexed_triangle_set`, so displacement, colour, `slice_mesh_ex`+`union_ex`, and accumulation all
   run **unchanged** downstream.
3. **Adopt the halo-inclusive normals** in the displacement step so band-count independence holds
   through `process()` (this is what removes the fork's seam-continuity caveat).
4. **Swap at the one call site** (`Engine.cpp:433-438`, where uniform `refine_region`→`triangulate_
   cage` runs today) behind the existing `params.subdiv_tol` (or an explicit `adaptive` flag). Nothing
   else in the band loop, the importer, the seam, or the colour path changes.

Instancing, creases, colour segmentation, footprint `eval_d`, per-placement device level, and the
TBB/progress/cancel machinery are all **left intact** — the port is only the tessellation primitive.

## 4. Gates (fix the mis-specified ones)

- **Not "byte-identical."** The fork's own equivalence tests use area-xor / `WithinRel` within `1e-3`,
  never equality; the two tessellators legitimately differ sub-tolerance. Every gate here is
  **area-equivalence within the suite's 1e-3**, matching Stage-3's oracle standard.
- **Band-count invariance through `process()`** — the standalone's pixel-exact property, re-asserted
  end-to-end (band=1 vs band=N area-equivalent). The standalone's `--gate` harnesses remain the fast
  unit proof.
- **Triangle-count drop** on a heterogeneous cage (the adaptive win; a 40×8×8 CC box drops ~5×).
- **Two fixtures the current suite is missing** (this is a real gap the reviews found): an end-to-end
  `slice_scene` test on a **creased** cage and on a **displaced** cage. Today no end-to-end test uses
  either (crease tests are subdivider-unit-only; `test_printman_slice.cpp` uses plain-mesh cubes that
  never enter the cage path), so a crease/normal regression would pass undetected. Add these before
  claiming the port is equivalent.

## 5. Source-of-truth direction

The **fork is canonical** for the shipping engine. The standalone stays the **oracle / clean-env
validation harness** — it is where adaptive dicing was prototyped and proven (zero-dep, clang-only,
OpenSubdiv-validated gates), which is its legitimate value. Prototyping a feature there and then
**porting it into the fork** is fine; what rev. 1 got wrong was making the fork *unable to receive a
change except through* the standalone. No `.upstream-ref`, no sync script, no CI diff-gate. Keep the
subdivider aligned by hand when either side touches it (a one-line note in each header naming the
other); it has diverged by ~38 lines once in the project's life, which hand-reconciliation handles.

## 6. Upstream posture (for when/if this goes to SoftFever)

Three things rev. 1 lacked, that a maintainer will demand on the first pass:

- **Make USD optional.** Add `option(SLIC3R_USD)` defaulting OFF, gating `find_package(pxr)`,
  `Format/USD.cpp`, and the reader. The tiering makes this cheap — the seam and engine are pxr-free;
  only `Format/USD.cpp` touches pxr. A mandatory 98 MB force-loaded OpenUSD dep for a USD-only feature
  is the single most likely rejection; "USD stays non-optional" is a fork-local convenience, not an
  upstream-defensible stance. (Mind the known two-cache `SLIC3R_USD` trap.)
- **Answer "why merge an engine, not bake a displaced/painted 3MF?"** Displacement and colour are both
  bakeable, so they do **not** justify an in-libslic3r engine on their own. The real justification is
  **per-point slicer-parameter fields** — shader-driven infill density / flow / material blend that
  are not geometry-or-paint and cannot ride a static mesh. That is currently vision (`DESIGN.md`
  §5), not wired; the honest upstream story leads with it and marks it the reason, with displacement
  as the first shipping increment.
- **Ship a normal in-tree component**, owned in-repo (no external pin, no cross-repo CI).

## 7. Portability, honestly

Deferred, but stated straight so it is not oversold later: the reusable part is the *engine*
(subdivider + adaptive dicing + band iteration), which is genuinely zero-dep and transplants cheaply
across the **libslic3r family** (Prusa/Bambu/QIDI/SuperSlicer — shared `Model`/`ExPolygon`/`union_ex`/
`Transform3d`). **Cura is a real port, not a thin adapter:** it has no per-band `slice(mesh,z)`
primitive (must be synthesized from its whole-mesh `Slicer`/`SliceDataStorage`), uses integer
ClipperLib `Polygons` not `ExPolygon`, has no `Transform3d`/3MF-via-libslic3r, and needs its own USD
reader. "One core, N thin adapters" conflates "no engine re-port" with "thin adapter." None of this
is in scope now.

## 8. What rev. 1 got wrong (so it is not repeated)

- Called the fork's engine the thing that "lacks the better engine" — it is **richer** for Orca.
- Claimed `UsdCage`/`SubdivCage` "near-identical" and unifiable onto `UsdCage` — `UsdCage` is the
  **weaker** carrier; unifying on it **drops creases** the fork uses.
- Claimed the core "emits neutral contours" — it emits **raw `LayerSegs`**; `ContourStack` is declared
  but never produced.
- Claimed a single "contours" seam — colour cannot ride it (`union_ex` is field-discarding), and the
  `Shader` interface has **no footprint** slot, so a shader-eval swap regresses texture filtering.
- Claimed Stage B "byte-identical when disabled" — impossible (different normal model/frame/weld) and
  the wrong gate (suite uses 1e-3 area-xor).
- Claimed "verbatim vendored core + CI diff-gate + `.upstream-ref`" — unpublished repo, namespace/
  guard contradiction, direction-blind gate; and it is the standing tooling tax that is out of scope.
- Let §9 ("serves Prusa/Cura") import parked LIBRARY-PLAN scope, inverting "standalone is incidental."
