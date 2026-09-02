# Proposal: add global voxel remeshing

## Why

This library can rebuild a surface from a field, sample a mesh into a field,
sign a dirty mesh with a generalized winding number, march a watertight
isosurface, validate the result and transfer its attributes. Every piece of a
voxel remesher is already here and has been for releases.

What is missing is the *operation*. There is no single verb whose contract is
"take this polygonal surface, rebuild it through a volumetric representation at
an explicit spatial resolution, fuse the overlaps, close what policy says to
close, reconstruct a clean surface, keep the attributes that can be resampled,
validate it, and hand back one replacement". A caller who wants that today
assembles it from six primitives and inherits every decision those primitives
did not make for them: what resolution means, how much memory the request will
cost, what happens to an open surface, whether the result is watertight, what
"the same input twice" is allowed to produce.

Those are not host decisions. They are geometry semantics, and a host that
answers them is a second engine. The 2026-08-29 decision that put local remesh
inside the engine rather than beside it applies here with more force: a global
topology rebuild discards vertex identity, invalidates UV seams, fuses shells,
can delete a feature thinner than the sample spacing and can close a hole the
artist meant. Every one of those is a contract, and contracts live in
`openspec/specs/`.

The gap is also the one artists notice. `DynamicSurface` gives local adaptive
topology and `MeshSculptor` gives fixed topology; neither can recover a mesh
that has been stretched, kitbashed or self-intersected past usefulness. Voxel
Remesh is the global reset those two workflows are missing, and it is the
operation ZBrush calls DynaMesh and Nomad calls Voxel Remesh.

## What changes

- **`mesh::voxel_remesh`** — one Mesh -> Mesh operation composing the existing
  BVH, mesh-to-field sampling, watertight marcher, validator and transfer.
- **World-unit voxel size** as the canonical resolution, with longest-axis
  resolution as a convenience mapping onto it.
- **A sparse active-brick sampling domain** so the expensive per-sample work
  follows the surface and its band rather than the bounding box's volume.
- **`mesh::voxel_remesh_estimate`** — resolved voxel size, grid dimensions,
  active samples, memory, triangle range, open boundaries, component count and
  a thin-feature warning, computed before anything large is allocated.
- **A resource guard** — a request over the caller's budget, or over the
  library's own ceilings, fails with a typed status instead of allocating.
- **An explicit open-surface policy** — `Reject`, `Close`, `BestEffort` — so an
  open source is never silently treated as though it had been watertight.
- **Constrained source reprojection** — clamped by distance and rejected on
  normal incompatibility, so detail returns without vertices jumping sheets.
- **Spatial attribute transfer** — vertex colour through the existing
  closest-point transfer, and `mesh::transfer_vertex_scalar` for a caller-owned
  per-vertex mask or weight, because mask lives outside `mesh::Mesh`.
- **Small-component and volume policies** — floating components preserved by
  default and removable by volume; a clamped optional volume correction.
- **Cancellation and progress** through `parallel::CancelToken`, cancelling to
  a state where the source is untouched.
- **A validation report** — both meshes' vertices, triangles, volume, boundary
  edges and components, and what the operation actually did.
- **`mesh::mesh_lattice_parallel`** promoted to the public meshing surface,
  because a global remesh marches a lattice the serial marcher cannot finish
  inside the operation's own latency budget.
- **C ABI and pyclay surfaces**, a numbered example with a committed render,
  and benchmark rows.

## Approach

Compose, never fork. The remesh layer selects existing algorithms and owns only
the decisions between them: it does not gain a second signing method, a second
mesher, a second BVH or a fork of the mesh-to-field converter. Where the
existing converter could not serve — it evaluates every brick of the bounding
box, which is the scaling the feature exists to avoid — the fix is a sparse
active-brick fill through `FieldVolume::sample_blocks`, the batched entry point
that already exists for exactly this, and the stored samples come out
BIT-IDENTICAL to what the dense path would have stored. That equivalence is a
requirement and a test, not a hope.

The pure geometry operation ships first and alone. It has no document, no
layer, no history and no revision token, which is what makes it testable and
reusable; a document-level command that adds undo and stale-result protection
is a separate change against `scene-model`, and the host can hold the before
and after meshes in one history record in the meantime.

`MeshSculptor`'s fixed-topology guarantee is untouched. Voxel Remesh is an
explicit replacement operation, never a brush and never a mode.

## Non-goals

- semantic quad retopology (ZRemesher-class edge-loop inference);
- local dynamic topology, which `dynamic-topology` already owns;
- production UV preservation — UVs are dropped and the requirement says so;
- automatic multires construction;
- automatic representation switching;
- real-time remesh per brush dab;
- GPU-accelerated remeshing;
- decimating the result by default.

## Impact

`meshing` gains the operation, the resolution convention, the resource rules,
the open-surface policy, the determinism promise and the transfer primitive.
`c-abi` and `python-bindings` gain the surface. `examples` gains an entry.
Nothing existing changes behaviour: a caller who never calls `voxel_remesh`
gets byte-identical results from every other verb, and `mesh_lattice_parallel`
is an existing internal function given a declaration, not a new algorithm.
