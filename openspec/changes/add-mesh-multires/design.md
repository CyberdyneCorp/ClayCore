# Design: mesh multiresolution

## Context

`VoxelGrid` already carries a level stack: `add_level`, `add_level_region`, and
a rule that outside a refined region the level has no storage and reads its
parent, so the lattice stays complete and a watertight transition is a
construction rather than a tolerance. That is the closest precedent in the tree
and the requirements below are written to rhyme with it where the mathematics
allow.

`mesh::Adjacency` and `mesh::Bvh` are both built for a surface whose topology
does not move. A subdivision hierarchy is exactly that once it exists, so
unlike dynamic topology this change reuses them rather than replacing them —
per level, lazily, and droppable.

`mesh::transfer_attributes` moves colours, UVs and normals by closest point and
barycentric interpolation and explicitly does not move positions. Detail
reprojection needs a geometry query with the same machinery and different
semantics, and overloading the attribute call would break a guarantee its
callers depend on.

## Goals / Non-Goals

**Goals:**

- Fine detail survives coarse edits, and survives them plausibly rather than
  approximately — a wrinkle stays normal to the surface it was sculpted on.
- Sculpt level and display level are independent.
- A low-level dab propagates to the descendants of the vertices it touched and
  to nothing else.
- Adding and removing levels is deterministic and reports its cost first.
- Undo records what was edited, not the derived caches.

**Non-Goals:**

- Topology changes inside a hierarchy that carries detail.
- Sculpt layers. The detail representation is designed for them; the stack
  itself is the next change.
- Baking detail to normal, displacement or vector-displacement maps.
- GPU evaluation of the hierarchy.

## Decisions

- **D1 — `P(n) = Subdivide(P(n-1)) + Detail(n)`, never a stack of unrelated
  meshes.** Independent absolute positions per level is the implementation that
  looks simplest and cannot do the one thing the feature exists for: with no
  relationship between a level and its parent, a change to the parent has no
  defined effect on the child, so either the child is discarded or the parent
  edit is. The relationship IS the feature.

- **D2 — Detail lives in a transported local frame, not in world space.** A
  world-space delta is correct for small changes and visibly wrong for the case
  that motivates the feature: rotate or bend the parent surface and the
  detail's world vector no longer points along the surface it belongs to, so
  wrinkles shear off the cheek that carried them. Tangent, bitangent and normal
  coefficients against a frame derived from the subdivided parent move with the
  surface by construction.

- **D3 — The frame is transported, not rebuilt.** A tangent re-derived from
  whichever neighbour happens to come first flips under small deformations, and
  a flipped frame rotates detail. The order is: a UV tangent where a valid
  parametrization exists, otherwise a deterministic geometric tangent,
  transported by shortest-arc rotation when the parent normal moves, with sign
  consistency enforced against the previous frame.

- **D4 — Reuse `Adjacency` and `Bvh` per level, lazily, and let them be
  dropped.** Topology is stable per level, which is precisely the condition
  those two are built for. A level's runtime cache is keyed on the level's
  revision, built when the level becomes active, and released under memory
  pressure without touching authoritative detail.

- **D5 — A separate `project_surface` for geometry, `transfer_attributes`
  untouched.** Reprojection moves positions; attribute transfer promises it
  does not. Two functions with the same BVH underneath and different contracts
  is the honest shape.

- **D6 — Adding a level preflights and refuses.** A subdivision multiplies
  faces by four, and on the target device the peak allocation is what kills an
  app rather than the steady state. The estimate is computed from the
  subdivision rule, checked against the budget, and refused with a typed error
  before anything is allocated — the same discipline the import budget already
  applies.

- **D7 — Undo records edited state, not derived state.** Higher-level
  positions are a cache of the parent plus the detail. Recording them would
  multiply an undo step by the number of levels for no information.

- **D8 — The hierarchy refuses arbitrary topology edits once it carries
  detail.** The relationship in D1 is defined by the subdivision rule; change
  the base connectivity and every stencil above it is meaningless. The
  supported route is a conversion — project the detail onto a new base — and
  that conversion is explicit and expensive by design.

## Risks / Trade-offs

- **Memory.** Levels multiply by four. The mitigations are residency policy
  (sculpt level and display level resident, others compact detail only) and
  preflight, both of which belong to the runtime change that follows.
- **Face-varying UVs under Catmull-Clark.** Averaging across a seam destroys
  it, quietly, and the artefact appears in a texture rather than in a geometry
  test. Needs its own scenarios.
- **Which rule first.** Choosing Catmull-Clark serves the imported quad
  character; choosing Loop serves whatever an adaptive surface produced. The
  hierarchy abstraction is written to take either, but the first shipped rule
  decides which workflow lands first.
- **Frame instability at poles and degenerate valences.** A deterministic
  fallback is required and its behaviour has to be pinned by a test rather than
  discovered in a render.

## Migration Plan

Additive. No existing type changes; `mesh::Mesh` is what a level exports.
The document format gains a chunk older readers skip. The C ABI grows opaque
handles and changes no existing signature.

## Open Questions

- Whether a level can exist over a REGION only, as the voxel level stack
  allows, and what the transition looks like on an irregular surface.
- Whether per-level index buffers are cached or re-derived.
- Whether the first rule is Catmull-Clark or Loop.
