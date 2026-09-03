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
