# Proposal: fixed-topology mesh brushes

## Why

A mesh layer carries triangles verbatim and never evaluates them. That is what
makes it useful — a retopologized quad mesh survives the round trip with its
edge loops and its uvs intact — and it is also why the mesh is currently
**read-only in every sense that matters**. The only way to change one today is
`Volume.from_mesh`, which resamples it onto a lattice and destroys the topology
somebody just paid a retopo pass for.

The round trip `add-representation-round-trip` and `quad-mesh-export` built is
sculpt SDF → quad export → retopo/UV elsewhere → bake. The step it has no verb
for is the one an artist actually wants next: **refine on the retopologized
mesh**. Fixed-topology brushes are the only operation that can touch that mesh
without throwing away what makes it worth having.

`docs/sculpt_comparison.md` files mesh surface-mode sculpting under deliberate
non-goals, and that stays a non-goal — with its wording tightened. The non-goal
was never "do not move vertices"; it was **do not compete on dynamic
tessellation**. Dyntopo, multires and remeshing are still not this engine's
fight. Moving the vertices that already exist is a different claim and a much
smaller one.

## What

Vertex displacement on a mesh layer's own triangles, with one line held:
**topology never changes.** No polygon is created, split or deleted; positions
move and the index buffer is never rewritten. `Mesh::quads` therefore survives
every verb here, which is precisely why the feature is worth having.

Three prerequisites, none of which exist:

1. **Adjacency** — `mesh::Mesh` is a triangle soup. Smooth, polish and crease
   need one-ring neighbourhoods, and a falloff measured along the SURFACE (the
   Move Topological lesson: a lip must not drag the chin) needs a geodesic
   walk over that ring. Built once per mesh, keyed to a vertex count this
   feature never changes.
2. **Mesh picking** — the pick module raycasts tapes, bricks and voxels; mesh
   layers are invisible to it. The BVH exists and already partitions the
   triangles; it has no ray query.
3. **Vertex-delta undo** — a mesh stroke is destructive vertex displacement,
   not an edit-list node. Sparse per-vertex before/after records, coalesced
   per gesture, restore the pre-stroke mesh bit-exactly without snapshotting it.

Then the brushes, in the two milestones the issue names.

**M1 — the primitives.** `grab` (drag by the stroke delta), `draw` (displace
along the region's averaged normal — one shared direction per stamp),
`inflate` (displace along each vertex's OWN normal), `smooth` (Laplacian over
the one-ring), `pinch` (signed tangential gather/spread — pinch and magnify are
one deformation with one sign, as they are for fields), `flatten` (project
toward a plane, with the same TwoSided / CutOnly / FillOnly mode `FlattenMode`
already established, because CutOnly *is* Trim Dynamic).

**M2 — the compositions.** `clay` (draw's deposit clamped to a plane floating
at the stamp height), `crease` (a tight negative draw and a pinch in ONE stamp),
`scrape` (flatten cut-only and smooth from ONE snapshot), `polish` (smooth
gated by dihedral angle, so noise goes and a hard edge stays), `snakehook`
(grab re-anchored along the drag). Each is cheap once M1 and the snapshot
exist, and each is a distinct stamp rather than a sequence of calls — the
voxel `sculpt_scrape` rule, that calling the two in sequence is not the same
thing, holds here for the same reason.

**M3 (`elastic_deform`) is out of scope** and stays undecided, as the issue
filed it: it is the one entry that is new math rather than new composition.

**Masks reach every verb for free.** `apply_to_mesh` scales each vertex's
weight by `1 - mask`, the rule every voxel verb already follows, so a painted
mask protects polygons from all eleven verbs with no per-verb code — including
grab and snakehook.

**The stroke engine gets its fourth consumer.** `resolve_stroke` already feeds
`apply_to_grid`, `stamps_to_nodes` and `apply_to_mask`; `apply_to_mesh`
inherits spacing, pressure curves, deterministic jitter, taper, steady stroke
and buildup-vs-clamped accumulation without new machinery. Buildup accumulation
is what turns one `clay` stamp into ClayBuildup.

## What this is NOT

- **Not dynamic topology.** No dyntopo, no multires, no remeshing, no
  subdivision. A large grab stretches triangles, and under `snakehook` it
  stretches them to the extreme. That is **stated behaviour, not a defect**: it
  is the artist's signal that the mesh wants retopo, exactly as Blender behaves
  with Dyntopo off. The SDF `snakehook` resolver remains the verb for GROWING
  new volume.
- **Not a change to what a document evaluates to.** The mesh layer's "never
  evaluated" rule is untouched. The mesh still never enters a tape, never
  blends with a field, and exports exactly as its (now edited) vertices say.
- **Not in the parity system.** Not tape-expressible; CPU-side, like the voxel
  verbs. The determinism bar still holds and is asserted: same stroke, same
  mesh, same result, bit for bit.
- **Not a topology repair tool.** A mesh with degenerate or duplicated
  triangles keeps them.

## Impact

- **New:** `mesh/adjacency.h`, `mesh/sculpt.h` and their sources;
  `pick::raycast_mesh` and `Bvh::raycast`; `brush::apply_to_mesh`.
- **Changed:** `mesh::Bvh` retains the original triangle index of each
  partitioned triangle (it did not need one for distance queries; a ray hit
  must name a triangle). No behaviour change to distance or winding.
- **Docs:** `docs/07-brushes-and-features.md` gains a § for mesh brushes;
  `docs/sculpt_comparison.md`'s non-goal is amended from "surface-mode
  sculpting" to "topology-CHANGING sculpting" with a cross-reference, rather
  than deleted — the boundary stays a decision.
- **Bindings:** pyclay and the C ABI both reach every verb, `apply_to_mesh`,
  mesh picking and vertex-delta undo; `tools/check_binding_parity.py` passes.
- **Examples:** new gallery entries with committed renders for the M1 verbs,
  the M2 verbs, geodesic falloff, and masking.
