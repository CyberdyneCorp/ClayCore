# Design: add-voxel-remesher

## Decisions

### 1. A sparse signed narrow-band `field::FieldVolume` is the canonical intermediate

Not the persistent palette `voxel::VoxelGrid`. That grid carries palette
entries, levels and edit semantics that a transient remesh has no use for, and
committing to it would couple a mesh operation to voxel sculpt state. The
sampled field gives sub-voxel surface placement, an existing mesh-to-field path,
the generalized winding sign, gradients and three existing extractors, at
O(surface x band) storage.

### 2. The sampling domain is marked from the source triangles, not walked over the bounding box

`mesh::to_field` routes through `FieldVolume::sample_parallel`, which calls the
caller's function for EVERY brick of the region — 32^3 bricks at longest-axis
256, 128^3 at 1024. Each brick is 729 samples and each sample is a BVH
signed-distance query with a generalized winding number. That is 24M queries for
a resolution the feature has to hit in 400 ms, and 1.5 BILLION at 1024. The
converter is right for an import, where the caller chose the cell size for the
model; it is the wrong shape for a resolution dial.

So the remesh supplies its own `BrickBlockFill`:

```text
mark   every brick whose sample box, dilated by the band, meets a source
       triangle's AABB                          -> ACTIVE
sign   flood-fill the complement; one winding-number query per connected
       component of INACTIVE bricks
fill   active  -> the full 729 BVH signed distances, across the pool
       inactive -> a constant of that component's sign, magnitude 4 x band
```

**The stored samples are bit-identical to the dense path's.** A brick the dense
path stores is one holding a sample within `band` of the surface; that sample is
within `band` of some triangle, hence within `band` of that triangle's AABB,
hence the brick is marked ACTIVE and evaluated identically. A brick this path
marks active but the dense path would not store is classified by the same
`scan_block` on the same values and is not stored either. The two stored sets
are equal, and every stored value is the same query at the same position.

The far-brick SIGNS are equal too, for a closed source: sign is constant on a
connected region no triangle passes through, so one query at one brick's centre
answers for the whole component. On an OPEN source the winding number's 1/2
crossing can sit away from any triangle, and a brick straddling it is mixed;
the dense path resolves that per brick and this path does not. That is stated
in the requirement rather than hidden, it only affects far bricks (which store
nothing either way), and the fixtures cover it.

### 3. World-unit voxel size is canonical; longest-axis resolution maps onto it

```text
voxel_size = longest_axis_extent / longest_axis_resolution
```

resolved BEFORE padding is applied, so the number an artist sets means the same
thing whatever the padding is. The report and the estimate both carry the
resolved voxel size, because that is the number that predicts what survives.

### 4. Padding and band are internal constants

Four voxels of padding and three of band. They exist so the zero crossing,
the trilinear interpolation, the gradient and the extraction ring all have room;
they are not artist controls and exposing them would be exposing the
implementation. They are named constants in the header so a test can reason
about them, not parameters.

### 5. Cancellation is a `parallel::CancelToken`, not a callback

The guide this change came from proposed progress/cancel callbacks. This
repository's `parallel/cancel.h` states the opposite decision and states why:
`clay.h` contains no function pointers, a callback would fire on a pool worker,
and every FFI consumer would have to marshal it. The token inverts that — the
engine writes progress, the host reads it, both sides plain atomics — and it is
what `sample_blocks` already takes. Voxel Remesh uses it and adds nothing.

Cancellation is checked at the window boundary `sample_blocks` already has, per
brick inside the fill, per marching wave, and per projection and transfer block.
A cancelled remesh returns `Cancelled` with an empty mesh; the source is a
`const&` and was never written, so "the source is unchanged" is a property of
the signature rather than a promise about a rollback.

### 6. The committed extractor is the existing watertight marcher

`mesh_lattice` over the volume's own lattice, reading stored samples exactly
(`sample_at`) and falling back to the volume's far bound elsewhere. Marching
tetrahedra has no ambiguous configuration, so watertight and 2-manifold are
structural — which is what lets `Close` promise a watertight result rather than
hope for one.

`mesh_lattice` is serial and a global remesh marches the whole lattice, so
`mesh_lattice_parallel` — already in `marching.cpp`, already byte-identical by
construction, already used by `mesh_tape` — is given a public declaration. The
sample function this passes it is a pure read of an immutable volume, which is
that function's stated precondition.

Sharp mode selects `mesh_lattice_dc` and stays EXPERIMENTAL: dual contouring is
flagged in the meshing spec and its output is not guaranteed manifold. The
watertight result contract is not enforced for Sharp, and the requirement says
so rather than letting a caller discover it.

### 7. Source projection is optional, clamped twice, and never a hard snap

Clamped by distance (`max_projection_distance_voxels x voxel_size`) and by
normal compatibility: a candidate whose source triangle faces away from the
reconstructed vertex's own normal is rejected outright. Nearest-point alone
jumps between nearby sheets — lips, fingers, cloth folds, a mechanical gap — and
a jump is not a small error, it is a hole pulled through the surface. Strength
is a lerp, never a snap, so a mis-clamped projection degrades toward "no
projection" rather than toward "corrupted".

### 8. Attributes are transferred spatially or not at all

Topology replacement destroys vertex identity, so anything preserved must be
resampled from the source's geometry. Colour goes through the existing
`transfer_attributes`. Mask does not live on `mesh::Mesh` — it is a caller-owned
per-vertex gate — so the primitive it needs is
`mesh::transfer_vertex_scalar`, added to `mesh/transfer.h` beside the transfer
it belongs with rather than invented inside the remesher. UVs are DROPPED, and
the requirement says dropped rather than "best effort": a spatially reprojected
UV across a seam is a stretched layout that looks like a preserved one.

### 9. The pure operation ships without the document

No layer, no history record, no revision token. `voxel_remesh(const Mesh&) ->
Mesh` is testable, reusable and has no state to corrupt. The undo story it
enables is a before/after pair, which the existing unified history already holds
and which the host can commit as one record; a `scene-model` command with stale-
revision protection is a later change with its own contract.

### 10. Failure is a typed status, never a silently reduced request

`ExceedsBudget`, `InvalidResolution`, `OpenSurfaceRejected`,
`ResultNotWatertight`, `Cancelled`, `EmptySource`, `Unsupported`. The library
does not lower a resolution it was asked for: an engine that quietly halves the
request produces a result the artist did not ask for and cannot explain. Fitting
a resolution to a memory budget is a host policy built out of repeated
`voxel_remesh_estimate` calls, and the estimate exists so that policy is cheap.

### 11. Determinism is a property of the decomposition, not of single-threading

Every parallel stage writes disjoint outputs from position-only inputs: the
brick fill, the marching waves (which replay their recorded edges through one
`Builder` in slab order), the per-vertex projection and the per-vertex transfer.
The same source and parameters therefore produce a bit-identical mesh on every
run, and the requirement says bit-identical rather than "equal to tolerance".

## Rejected alternatives

**Occupancy-only triangle voxelization.** Faster to rasterize, but it gives no
sign for an open mesh, no sub-voxel placement, no gradient and nothing for
projection to clamp against — and this library already owns the
signed-distance/winding infrastructure that answers all four.

**Reusing `mesh::to_field` unchanged.** It would work and it would be O(bounding
box) in BVH queries, which is the one scaling property the feature is defined
against. The converter is not forked: the remesh calls the same
`FieldVolume::sample_blocks` entry point the converter calls, with a different
fill.

**A temporary SDF document item.** Pollutes document semantics with an
intermediate nobody asked to see, and drags in tape compilation, layers and
undo for a transient field.

**Decimating the result.** Uniform spatial resolution is the feature. A caller
who wants fewer triangles has `mesh::decimate` and should be seen to choose it.

**A `DynamicSurface` overload.** `DynamicSurface::to_mesh` and `from_mesh` are
the documented boundary and the round trip is two lines at the call site. An
overload would add API surface to hide a conversion the caller should see.

## Open questions

- **Where the document command lives** — `scene-model` as a history kind, or a
  session command owning both meshes. Not decided here because this change does
  not ship it.
- **Whether the thin-feature warning should localise.** V1 reports a boolean
  from a deterministic sampling of the source; returning at-risk REGIONS is more
  useful and needs a representation this change does not want to invent.
- **Whether volume correction earns its default.** It ships on, clamped to 2%
  linear, and skipped when a hole was closed or a component removed. If the
  fixtures show it moving surfaces more than it recovers volume, the default
  should flip before release.
