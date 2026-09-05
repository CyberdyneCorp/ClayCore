## Why

`add-mesh-multires` shipped a hierarchy that refines uniformly, and recorded the
gap in its own row: "Region-scoped levels (the mesh analogue of
`add_level_region`) are deferred and the reason is recorded in the change."

The cost of uniform refinement is the whole reason a sculptor wants a hierarchy:
a face wants level 5 and the boots do not, and today asking for level 5 anywhere
means level 5 everywhere. That is `4^3` times the vertices of level 2 across a
whole model to get detail on a nose, and on a device with a memory ceiling it is
the difference between a session and a refusal.

**The precedent is in the tree and should be read before the API is designed.**
`clay_voxel_add_level_region` already refines a voxel grid over a region, with
the rule that outside a refined region a level has no storage and reads from the
one below. The mesh hierarchy should mirror those semantics where it can rather
than inventing a second vocabulary for one idea.

## Audited against `main`, 2026-09-05, and it moves the design

The change was written before anyone measured where a level's memory goes. It
does now, on a 1,600-patch cage:

| level | faces | topology | detail | evaluated | chunk index | total |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 6,400 | 0.1 MB | **0.0** | 0.5 MB | 0.3 MB | 2.1 MB |
| 3 | 102,400 | 2.6 MB | **0.0** | 9.4 MB | 3.1 MB | 30.8 MB |
| 4 | 409,600 | 10.4 MB | **0.0** | 37.6 MB | 10.8 MB | **121.4 MB** |

**`DetailField` is already sparse, and it is not the prize.** Its own header
says so — "a block of `block_size()` vertices exists only once something in it is
non-zero" — and the measurement confirms it: a level nobody has sculpted costs
0.0 MB of detail. The intuitive reading of "level 5 everywhere is expensive" is
that the coefficients are expensive. They are not, until they are authored.

What a level actually costs is **topology, the evaluated buffers and the chunk
index**, all of which scale with FACE COUNT and none of which is avoided today.
That is 58.8 MB of the 121.4 above, with the remainder in runtime index and
composed detail — every byte of it proportional to faces this hierarchy may
never sculpt.

So the sparsity this change adds SHALL be in the topology and the derived
buffers, keyed by patch. Making detail sparser would deliver nothing, and a
proposal that led with the detail argument would have been measured against the
wrong number.

### Two facts that make it tractable

**Faces are patch-major at every level.** `src/mesh/multires_chunks.cpp` states
it — "already patch-major everywhere subdivision produced it" — so one patch's
faces at one level are a CONTIGUOUS RUN. Storing a subset of patches is a
question of which runs to build, not of a scattered index.

**`LevelTopology` already carries the identity.** `face_patch[]` names the
level-0 face each face descends from, `patch_count` is beside it, and the header
already calls that identity stable for the life of the hierarchy. Regional
refinement extends an existing per-patch structure rather than introducing one.

### What the voxel precedent settles

Task 0.1 says to read `clay_voxel_add_level_region` first "so the two are one
idea rather than two vocabularies", and read, it decides the shape:

> "Outside the region the new level has no storage and reads its parent's value,
> so the lattice is still uniform and complete — only what is STORED changes, and
> meshing, bounds and neighbour indexing are as they were."

Taken across, that is: a patch not refined to level L has no storage at L and is
READ at its own effective level. Task 2.2's watertightness then holds by
construction rather than by repair — a boundary vertex on the fine side is the
coarse edge's exact subdivision because that is literally what the unrefined
neighbour returns, not because a rule reconciles two authored values.

## What Changes

- **Depth becomes per base patch**, not per surface. A `patch_max_level[]` beside
  the existing base topology, which is authoritative — the refined region is
  never a loose set of fine vertices, because a set of vertices cannot be
  subdivided deterministically and cannot be balanced.
- **Selection is separate from refinement.** The core API takes a patch list;
  region and sphere helpers sit on top. A test that names patches is
  deterministic; a test that names a world box is a test of the selection.
- **2:1 balance, enforced and deterministic.** Adjacent resident patches differ
  by at most one level, with balancing rings built automatically and the queue
  processed in stable patch-id order — an unordered queue produces a different
  balanced surface on two runs, which the mesh verbs' determinism promise
  forbids.
- **Watertight transitions by construction.** A fine patch's boundary is
  constrained to the exact Catmull-Clark subdivision of the shared coarse edge,
  so the midpoint is DERIVED rather than authored and a T-junction cannot open.
  Transition polygons for display are derived data and never the authoritative
  sculpt state.
- **Refining changes no shape.** New levels start at zero detail, so adding one
  is invisible until something is authored into it — asserted numerically, not
  claimed.
- **The existing runtime is reused, not forked**: one `ChunkTable` with the
  identity it already has (regional refinement just means some
  `(base patch, quadrant)` chunks do not exist), `SculptLayerStack` inheriting
  sparsity, the existing preflight refusing before it allocates.
- **Monotonic in v1**: no level removal. Removing a region needs a policy for the
  detail that lived there — discard, bake, project or refuse — and picking one
  silently is worse than not offering the call.
- Serialization version, C ABI mirroring the voxel call, pyclay, a numbered
  example, and the benchmark that is the actual claim: **memory and update cost
  follow the refined area, not the finest level times the whole surface.**

## Capabilities

### Modified Capabilities
- `mesh-multires`: depth is a property of a base patch rather than of the
  surface, with the balance, watertightness and shape-invariance that makes that
  safe.

## Impact

- `include/clay/mesh/multires*.h`, `src/mesh/multires*.cpp`, `detail_field.*`.
- Serialization version; `bindings/c/`, `bindings/python/`, `tests/`,
  `benchmarks/`, `examples/`, `docs/09`.
- ABI grows.
