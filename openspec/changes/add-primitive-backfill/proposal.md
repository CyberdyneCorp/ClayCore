# Proposal: primitive backfill — the remaining kernel shapes in documents

## Why

`prim3d.h` implements 30 primitives; the tape exposes 14. The rest are tested, parity-checked kernel code that no document can use: plane, tetrahedron, dodecahedron, icosahedron, link, capped torus, solid angle, cut sphere, cut hollow sphere, the exact cone, the infinite cylinder, triangular prism, the cheap octahedron, and the L-norm sphere.

This is the last of the three gaps identified in the roadmap review and the most mechanical: the math, the tests, and the parity contract already exist. What is missing is opcodes, scene descriptors, bounds, and bindings.

## What Changes

- **14 new primitive opcodes**, each with its scene constructor, local bound, and Python class.
- **The `_ab` endpoint variants stay kernel-only, deliberately.** `sd_capped_cylinder_ab` and `sd_round_cone_ab` take two endpoints and two radii — eight parameters, one more than the tape's primitive block holds. Widening that block would change the document format for no new capability, since an oriented cylinder or round cone is already the axis-aligned primitive plus the item's transform. They remain available to direct kernel users.
- **Unbounded primitives are marked as such.** Plane and the infinite cylinder have no finite extent, so items using them report **infinite influence** and are never culled — the same treatment intersect, the transitions, and infinite grids already receive. This is the third time that rule has applied, so the change also gives it a single home: a `prim_is_unbounded` predicate next to `op_is_local`, rather than a fourth ad-hoc branch.
- **Bound primitives keep their classification**: the cheap octahedron, triangular prism, and L-norm sphere are non-exact fields, so the compiler downgrades tracked exactness for them exactly as it already does for the ellipsoid.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: the 3D primitive requirement gains tape reachability for the full set.
- `python-bindings`: the module exposes every primitive the tape can express.

### New Capabilities

_None._

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/bounds.cpp`, `bindings/python/pyclay_module.cpp`, tests.
- Serialization is unaffected: primitives already round-trip through `Prim::type` plus its parameter block.
- Non-goals: the `_ab` endpoint variants (see above), and `sd_cylinder_inf`'s cross-section offset is exposed as-is rather than generalized to arbitrary axes.
