# Proposal: mesh multiresolution

## Why

Dynamic topology answers "make geometry where the brush needs it". It does not
answer the other half of a sculpting workflow, and the two are genuinely
different problems: **an artist must be able to add wrinkles at a fine level,
go back and change the skull underneath them, and return to find the wrinkles
still there and still attached.**

Nothing in this library can do that on a mesh. The voxel side has a level stack
— block out coarse, `add_level` to refine where the detail goes, refine over a
region with watertight transitions by construction — and it is the right
precedent to read first. The SDF side does not need one, because resolution is
an evaluation parameter there and there is nothing to stack. A mesh layer is
the one representation where resolution is neither evaluated nor stacked: it is
fixed by the import, and the old non-goal ("resolution is an evaluation
parameter here, so the whole multires apparatus has nothing to attach to") was
true of the field and was never tested against a mesh.

The consequence today is that fine detail and coarse form are the same edit. A
pass that changes proportions destroys the detail on top of it, because the
detail IS the vertex positions and there is no record of what part of a
position was form and what part was wrinkle.

## What changes

- **`mesh::MultiresSurface`** — a base mesh plus a deterministic subdivision
  hierarchy plus per-level detail, where a level's positions are
  `Subdivide(parent) + Detail(level)` rather than an unrelated mesh.
- **Detail in a transported local frame** — tangent, bitangent and normal
  coefficients rather than world-space deltas, so that bending the cheek
  carries the wrinkles with it instead of shearing them off it.
- **A sculpt level independent of a display level**, so broad edits can be
  made while inspecting the fine surface.
- **Local propagation** — a low-level dab dirties the child stencils that
  depend on it and no others.
- **`mesh::MultiresSculptor`** over the shared brush kernels, with sparse
  per-level undo.
- **Projection** — a separate `project_surface` for shape and detail, leaving
  `transfer_attributes` attribute-only as it is.
- **Preflight** — adding a level reports its predicted cost and refuses
  rather than allocating half of it, because the device this is for kills an
  app for memory rather than warning it twice.

## Approach

Topology-stable, which is what makes it much cheaper than dynamic topology: the
existing `Adjacency` and `Bvh` are reusable per level, lazily built and
droppable under memory pressure. A hierarchy is built from a base whose
connectivity is then fixed; arbitrary topology edits into a hierarchy that
already carries detail are refused rather than silently invalidating it.

The lifecycle is explicit and one-way per stage: free-form construction on an
adaptive surface, freeze the topology, initialize the hierarchy, then fine
detail. That ordering is what the two representations are for, and blurring it
is what makes multires implementations fragile elsewhere.

Detail storage is designed here for the layer stack that comes next, because
building the two independently is how a library ends up with two displacement
systems that disagree.

## Open questions

- **Which subdivision rule first.** Catmull-Clark matches the quad character
  topology an imported retopologised mesh has, and the quad list this library
  already carries; Loop covers arbitrary triangles including anything an
  adaptive surface produces. The hierarchy abstraction should not depend on the
  answer, but the first implementation does.
- **Face-varying UV rules.** A seam averaged across its own boundary is
  destroyed silently, and Catmull-Clark makes that easy to do.
- **Whether levels are cached as index buffers.** Topology under subdivision is
  deterministic, so a cache is an optimisation; a P0 that caches per level is
  simpler and the memory question is real on the target device.
- **What refines over a REGION.** The voxel level stack refines a level over a
  world-space region and keeps the lattice complete outside it. The mesh
  equivalent — a level that exists only where the detail went — is the same
  artist request and a harder problem on an irregular surface.

## Impact

A new `mesh-multires` capability. `meshing` gains the relationship between a
hierarchy and the flat interchange mesh; `scene-model` gains a history kind and
memory rows; `file-io` gains a versioned format; `c-abi` and `python-bindings`
gain the surface. Additive: a document with no hierarchy is unchanged.
