# Proposal: close the mesh brush vocabulary

## Why

The eleven mesh verbs cover the classical vocabulary, and comparing that count
against ZBrush's ~36 overstates the gap: `Flatten` carries three modes, so
Blender's Flatten/Fill/Scrape and ZBrush's Trim Dynamic/hPolish/Planar are
already covered, and a large share of the rest are alpha or falloff variants of
one verb rather than distinct operations.

Four things are genuinely absent, and one of them is an asymmetry this
repository created last week.

**Alphas do not reach mesh brushes at all.** `MeshBrushSettings` has no alpha
field. Voxels have had `sculpt_carve_alpha` since 0.24; SDF items gained
`Deformer::alpha` in `add-sdf-alphas`. The mesh layer — the representation an
artist reaches for *after* a retopo pass, when detail is exactly what they are
adding — is the one that cannot stamp one. That is the wrong way round, and it
is new.

**Nothing relaxes a stretched region.** `Smooth` is Laplacian: it averages
positions, which moves the surface inward. What is missing is the verb that
slides vertices *along* the surface to even out edge lengths without changing
the shape — Blender's Slide Relax, ZBrush's Relax.

This matters more here than in either of them. Topology is fixed by contract, so
a large `Grab` stretches the triangles it has; `docs/sculpt_comparison.md` names
that as the point where the engine stops on purpose and says the stretch "is the
signal the mesh wants retopo". Relax is what lets an artist recover *without*
a round trip through another tool, and it is the one verb whose value goes UP
because polygons are never added.

**Every deposit verb compounds.** Draw, Clay and Inflate accumulate stamp on
stamp, so a slow stroke digs deeper than a fast one over the same path. The
`Layer` brush is the one that does not: it deposits to a fixed height above
where the surface was when the *stroke* began and stops there. Both ZBrush and
Blender have it, and its absence is why a stroke's speed is currently part of
its result.

**`Grab` is the only way to move material sideways**, and it drags the region
rigidly. `Nudge` pushes tangentially along the stroke, sliding material across
the surface rather than carrying a lump of it.

## Approach

Four verbs' worth of behaviour, three of them composed from machinery that
already exists.

**Alpha as a weight multiplier, sampled by the same kernel the SDF alpha uses.**
`calpha_sample` is a kernel-dialect function and the CPU profile compiles to
ordinary C++, so the mesh side can call it directly rather than reimplementing
bilinear lookup. That is worth doing for a reason beyond saving code: **the same
stamp then reads identically on a mesh and on a field**, which is the standard
`sculpt_grab` and the `grab` deformer are already held to. An alpha multiplies
`BrushRegion::weights`, so it composes with every verb and every falloff at
once instead of being a verb of its own.

**Relax is `Smooth` with the normal component removed.** The Laplacian target is
already computed; projecting the displacement onto each vertex's tangent plane
is the whole verb, and `tangential()` already exists in `sculpt.cpp`.

**Layer needs the stroke's starting surface, and already has it.**
`VertexDeltas::note` captures each vertex's `before` the FIRST time the stroke
touches it, and `brush::apply_to_mesh` drives one record across a whole stroke.
So the clamp reference a Layer brush needs is a record the stroke is already
keeping — no new per-stroke state, no new lifetime to get wrong.

**Nudge is a tangential Grab.** Project the drag onto the tangent plane per
vertex rather than applying it rigidly.

## The part that is not mechanical

**Relax is not exactly shape-preserving, and the docs should say so rather than
implying it is.** Sliding a vertex along its tangent PLANE moves it off a curved
surface by second-order error, so a relax pass on a sphere shrinks it very
slightly. The honest options are to state the error, or to re-project onto the
pre-stamp surface — which needs a closest-point query per vertex and turns a
cheap verb into a BVH walk. The proposal recommends stating it, measuring it in
the example, and leaving re-projection as a later option if the drift is ever
visible.

**Layer's height is in world units, not strength.** Every other verb scales its
strength by the brush radius so it behaves the same at any size. Layer's whole
point is a fixed ceiling, so a radius-relative height would make the ceiling
move when the brush resizes — which is the behaviour it exists to remove.

## Open questions

- **Whether Layer without a record is an error or a no-op.** With no
  `VertexDeltas` there is no stroke origin, so every stamp clamps against the
  current surface and Layer degrades to Draw. Refusing is defensible; so is
  documenting the degradation. Refusing is probably right, since silently
  becoming a different verb is the worse failure.
- **Whether alphas belong on `MeshBrushSettings` or on the stroke.** Per-stamp
  is simpler and matches the SDF side; per-stroke would let one stamp's alpha
  orientation follow the stroke direction, which is what ZBrush's rolling alpha
  does. Per-stamp first.

## Impact

`meshing` gains the verbs and the alpha field. `c-abi` and `python-bindings`
gain the surface, additively — `clay_mesh_brush` is an enum a new value extends,
and `MeshBrushSettings` grows fields that default to today's behaviour. No
format change: vertex displacement was never stored as an edit item, so nothing
in `.clayspace` describes a brush.
