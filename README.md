# printman

**Geometry amplification for 3D printing.** A coarse subdivision-surface cage carries OSL
shaders (displacement + colour). printman amplifies it — dicing, displacing, and shading
**per Z-band** (REYES-for-slicing, never materializing the whole surface) — and emits per-layer
output.

Two deliveries of one host-independent core:

- **`printman` standalone CLI** — USD in → **PNG layer stacks** + **renders**, plus (via
  compile-gated backends) FDM contours, resin `.sl1`, and J55 Voxel-Print bitmaps. The only
  vehicle for hostless targets (resin, PolyJet/J55), and the dev loop / oracle.
- **A portable library** merged into open C++ slicers (OrcaSlicer first; the libslic3r family,
  then CuraEngine as a larger port). Same core; the host supplies its slice primitive and
  consumes the per-layer contours.

The core carries the shader's **continuous albedo RGB losslessly**; converting colour to a
printer's discrete gamut (FDM filaments, J55 tanks) is a per-host/per-sink step.

Status: **greenfield**, started 2026-08-17. Design and rationale (including the comparison of
two independent designs that produced it) are in [`docs/DESIGN.md`](docs/DESIGN.md).
