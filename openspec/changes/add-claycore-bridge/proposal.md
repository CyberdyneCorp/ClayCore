# Proposal: the pipeline seam — what crosses, and who owns it

**DRAFT for review.** The product questions in `design.md` are decisions to take
before anything is built, not after. Section 1 of `tasks.md` is a measurement
pass; the rest depends on how the seam is drawn.

## Why

`docs/sculpt_comparison.md` states it in one line: **"Without it you can sculpt
but not ship."** Sculpting is finished, retopology and UV live in
CyberRemesherAndUV, and between the two there is nothing — no baking, and no
agreement about what crosses the boundary or in what form.

This is the last row of Phase 3 and it has never been started.

## What the roadmap says, and what is actually true

The ROADMAP row reads:

> A retopo-oriented mesh export profile, plus a field-evaluation callback so a
> baker can sample exact normals and AO from the field rather than raycasting a
> mesh.

**Half of that already shipped**, and the proposal has to start there rather
than build it twice. Verified against the tree rather than assumed:

| what a bake needs | status |
|---|---|
| batched field distances, with colour | `clay_eval_points` — **shipped** |
| unit-length surface normals from the field | `clay_eval_gradients`, tetrahedron trick — **shipped** |
| the same against ONE layer | `clay_layer_eval_points` / `_gradients` — **shipped** |
| a quad mesh at a controlled density | `clay_document_mesh_quads` with a target — **shipped** |
| re-import a retopologized mesh and sculpt it without touching topology | mesh layers + `MeshSculptor`, `indices` byte-identical — **shipped** |
| carry paint and uvs from the sculpt onto the retopo | `clay_mesh_transfer_attributes`, with a fall-back report — **shipped** |

So a host can already sample exact normals and colour from the field. What it
cannot do is everything *around* that.

## What is genuinely absent

Five things, and only the first two are large:

1. **No ray can be bounded.** `clay_raycast` exposes no maximum distance. A
   cage projection is "search 5 mm along this normal, in both directions, and
   take the nearest hit" — and there is no way to say 5 mm. A miss and a hit on
   the far side of the model come back indistinguishable, which is exactly the
   failure that produces a bake with garbage in the seams.

2. **No AO, and no thickness.** Both need rays cast *from* the surface.
   `brush/procedural_mask.h` deferred them explicitly and said why: *"both need
   rays cast from the surface, which is a different cost class and a different
   set of parameters (ray count, length, falloff), so they are their own change
   rather than two more enumerators here pretending to be as cheap as the
   rest."* This is that change.

3. **Curvature is computed and thrown away.** `brush::mask_from_surface` derives
   curvature, cavity and convexity from the Laplacian, and the only thing it can
   return is a `MaskField`. A baker wants a value per point, not a lattice.

4. **No bake entry point.** A host today wires `raycast_many` +
   `eval_gradients` + `eval_points` itself and has to get the cage
   correspondence right on its own. Every host will write that loop, and they
   will not all write it correctly.

5. **No texture-space anything.** The target of a bake is a UV layout, and this
   library has no notion of one.

## The decision this change exists to take

**Where is the seam?** Three answers, and they differ in scope by an order of
magnitude. `design.md` lays them out; the recommendation is **B**, and the
reason is that A requires this library to learn UV semantics — parameterisation,
seams, islands, padding, dilation — which is the thing the sibling repository
exists to own.

Nothing is implemented until that is settled, because each answer implies a
different ABI and they are not subsets of one another.

## What this is not

- **Not retopology.** `docs/08-mesh-readback.md` is already blunt that quad
  export is *"a lattice-derived quad grid, not retopology... the input a
  retopology pass replaces, not the output of one."* That stays true.
- **Not PBR material authoring.** `docs/sculpt_comparison.md` records roughness,
  metallic and normal channels as a declared non-goal for *painting*. Baking a
  normal map from a field is a different thing from painting one, and this
  proposal covers only the first.
- **Not mesh booleans.** Decided against 2026-08-21 and unaffected.
