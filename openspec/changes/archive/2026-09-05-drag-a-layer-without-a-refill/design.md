# Design

## Why the guarantee holds, stated where it can be checked

`emit_item` composes `world = layer.xform * item.xform` and emits
`world.inverse_matrix()` (`src/scene/tape_build.cpp:670`). The layer's transform
reaches the tape in exactly three places:

1. inside each item's inverse matrix, where it is a change of frame;
2. `rounding * layer.xform.scale` on items and groups (`:819`, `:824`, `:855`);
3. `layer_scale_for_gate_`, which the cull uses.

For a rigid placement `layer.xform.scale == 1`, so (2) and (3) are unchanged and
(1) is a pure re-framing: the compiled field is the previous field composed with
the inverse of the placement change. For a similarity, (2) and (3) scale by the
same factor the distances do, which is what keeps a rounded or blended shape
similar to itself rather than merely relocated.

Layers combine with `emit_combine(Op::Add, Blend{}, 0.0f)` — a hard min
(`:899`), already stated in `scene-model` as "layers combine by hard union at the
document level". So no cross-layer term has to be re-solved when one layer
moves; the composite is `min` over layers and only one operand changed.

This is the whole argument, and it is why the gate in phase 1 is a *field
equality* test rather than a tolerance: mesh the layer, place it, mesh again,
and compare through the matrix.

## Phase 1: the report

`read_transform` already builds the `math::Transform` the command carries. The
classification is a predicate on the transform plus the node's `scale_axes`, and
the delta matrix is `new.matrix() * old.inverse_matrix()` taken before the apply.

Nothing about invalidation moves. The value of shipping this alone is that the
guarantee gets a test on hardware the phase-2 gesture then relies on, and that a
host can take the win itself before the gesture exists.

## Phase 2: the gesture

`GestureRegion` (`bindings/c/clay_c.cpp:3060`) is the shape to follow — RAII,
invalidation on destruction, region stated up front. A placement gesture is the
same idea with the destruction deferred to an explicit `end` and the region
known analytically: the layer's bound at the opening placement, unioned with its
bound at the closing one.

The gesture holds no copy of the document. It holds the layer id, the placement
it opened with, and the placement of the last update. `end` applies one
`SetTransformCmd`-equivalent for the layer, through the ordinary command funnel,
so undo gets one step for free and the invalidation is the one `apply_edit`
already computes.

Refusing other edits while a gesture is open is a guard in `edit_guard`, which is
already the single place every edit passes and already the place a protected
layer is refused. That keeps the refusal from being something each entry point
has to remember.

## Phase 2: the per-layer split

`compile_document_part(doc, active, below, cull, index)` already compiles either
"every visible SDF layer before `stop`" or "`stop` alone"
(`src/scene/tape_build.cpp:890`). What is needed is the same function with
`below` generalised from "everything before" to "everything except", which is one
condition in `run_part` — the loop already visits every layer and decides per
layer whether to compile it.

The complement property the spec asserts (pointwise `min` of the two parts is
the whole) is exactly what the hard union between layers already means, so it is
a test of the split, not a new numerical claim.

**Seeds must be keyed by scope.** The resume store keys an entry by brick and
shape; two scopes compile different tapes, so a scoped result served as an
unscoped seed is a wrong field, silently. The key gains the scope, and a scope
the store does not hold is an ordinary miss.

## Phase 2: meshing one layer

`clay_document_mesh` meshes the whole document's SDF content. Meshing one layer
is the same call over a one-layer scope, so it is the mesh-path sibling of the
brick-path split and not a second mesher. `clay_document_mesh_layer` is an
unrelated call — it borrows an imported mesh layer's triangles — and the naming
collision is worth resolving in the header rather than in a reviewer's head.

## What a preview cannot show

Two exactly-placed surfaces drawn separately interpenetrate where the hard union
would have resolved them. Every DCC shows this and it resolves on drop. It is in
the spec so that nobody later reads the exactness claim as covering the union.

## Rejected

**Re-keying brick seeds by the placement.** Only a translation by a whole
multiple of the brick lattice can be re-keyed, which is not what a gizmo
produces.

**Patching the resident tape's matrices instead of recompiling.** It saves the
compile and not the evaluation, and the measurement says the per-brick culled
tape compile is where the time goes — so patching the whole-document tape does
not reach the term that matters. Worth revisiting for the per-brick path, which
`add-item-spatial-index` is already about.

**Letting the gesture keep a snapshot of the document so other edits can
interleave.** A snapshot of a 1000-item document per drag is the allocation
pattern the iPad's memory pressure punishes, for a case (editing elsewhere
mid-drag) no host has asked for.
