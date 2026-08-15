# Proposal: a voxel sculpt should not have to look like Minecraft

## Why

Issue #108, found by running the gallery at v0.30.0 and looking at it. Every
voxel render is cubes; every SDF render is clay. The brushes are not the
problem — `15_smudge` puts its nubs in the right place and `36_levels` shows
its fine ribs — but `VoxelGrid::mesh_greedy` emits axis-aligned quads, so
occupancy is displayed as boxes.

The timing makes this the right moment. v0.30.0 measured the voxel display
path on the reference iPad: a dab and the frame that shows it cost ~0.54 ms
together, **13% of the engine's frame share**. So the two representations have
swapped problems:

- **voxel** — fits the frame with ~7x to spare, and renders as blocks
- **SDF** — looks like clay, and misses the frame by 1.15x at 1000 stamps

Speed cannot fix the first, and it is the largest visible gap between what
claycore produces and what a sculptor expects.

## What

`mesh/surface_nets.h` already meshes any lattice — `mesh_lattice_nets` takes a
`std::function<float(int,int,int)>` sampler rather than a tape — so what is
missing is a sampler over the grid, not a mesher.

**The mechanism is the dual mesh, not a blur.** Surface nets places one vertex
per sign-changing cell at the **centroid of that cell's edge crossings**. Over
binary occupancy sampled at voxel centres, that centroid is already a
smoothing operation: a corner voxel's vertex is pulled toward the average of
its crossings, which rounds the corner. Nothing has to be filtered first, and
— the property that decides the default — **nothing can vanish**, because a
lone occupied voxel still has sign changes on all six of its edges.

An optional occupancy blur is offered on top, and is NOT the default, because
a blur is what makes thin features disappear: a 3x3x3 tent puts an isolated
voxel at ~0.3 occupancy, below any sensible isolevel, and it is gone.

**Measured rather than predicted, once the picture existed:** the default
rounds corners but still terraces visibly, because every crossing over binary
occupancy interpolates to the same midpoint. One blur pass is what actually
reads as clay. So the honest recommendation is `blur=1` for an organic sculpt
and `blur=0` when thin features matter, and the DEFAULT stays 0 — a default
that silently deletes a sculptor's thin detail is the wrong default however
good it looks. `examples/41_voxel_smooth_display.py` shows all three side by
side, which is how this was found.

`clay_voxel_mesh` and `clay_voxel_mesh_chunks` keep their exact current
behaviour. The blocky mesh is not deprecated: it is correct for hard-surface
voxel work, it is what export currently guarantees, and per-chunk greedy
meshing has a seam argument (axis-aligned quads split rather than crack) that
the smooth mesher does not inherit.

## What this is not

**Not a conversion.** The grid stays binary occupancy and the document is
untouched; this is a display path. Converting a sculpt into an operand a host
can boolean against is #90, which this deliberately does not do and which
settles its own questions about tolerance, colour and layer semantics.

**Not incremental yet.** This change delivers whole-grid smooth meshing and
measures it. Per-chunk smooth meshing is a second step for the same reason
#86 was two: the seam argument has to be made again from scratch, because it
is a different argument. Greedy quads are exact and axis-aligned, so clamping
the merge to a chunk boundary splits a quad. A surface-nets vertex is the
centroid of its cell's crossings, so the two sides agree **only** if both
compute it from the same global sampler — which is arrangeable, and is
exactly what has to be specified and tested rather than assumed.

## The costs, stated rather than discovered

- **Corners round.** That is the feature. A voxel cube meshed smooth is not a
  cube, and a host that wants the cube has `clay_voxel_mesh`.
- **Triangle count rises.** One vertex per surface cell, against greedy's
  merged runs — a flat face that greedy emitted as one quad becomes one quad
  per cell.
- **Colour becomes per-vertex and blends.** A vertex sits between up to eight
  voxels; its colour is the average of the occupied ones among them. Two
  palette entries meeting therefore gradate over a cell rather than meeting at
  a hard line, which is what a smooth surface implies and what the greedy
  mesher's per-quad palette colour cannot express.
- **Not manifold.** `surface_nets.h` states it: a cell crossed twice by the
  surface gets one vertex and the sheets pinch. This is the preview path; the
  header says so, and `clay_voxel_mesh` remains the export-grade one.
- **It costs ~10x greedy, measured.** On one Linux desktop over an identical
  8,082-cell blob: `mesh_greedy` 2.79 ms, `mesh_smooth` 26.9 ms — **9.6x**.
  Scaling the device's 4.72 ms whole-grid greedy figure by that puts a smooth
  whole-grid mesh near 45 ms on the reference iPad, which is an OPERATION and
  not a frame. So this change delivers a display path that a host uses on a
  settled sculpt, and **interactive smooth display needs the per-chunk half**
  that is deliberately not here.
- **Cost follows the occupied BOUNDING BOX, not the material.** The sampler
  materialises a dense span of floats over the occupied extent, because the
  mesher reads each lattice point up to eight times and a blur pass reads
  twenty-seven. That is the shape #86 part 1 removed from the greedy sweep,
  and it is reintroduced here in a smaller way: two voxels far apart on two
  axes make the box enormous while the material stays small. Bounded rather
  than solved — the span is checked in 64-bit and a grid whose box exceeds
  ~256M samples returns an empty mesh instead of asking for an allocation
  nothing can satisfy. The per-chunk path is what fixes it properly, which is
  one more reason it is the next step rather than an optimisation.
