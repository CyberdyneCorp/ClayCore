# Design: one undo, across three representations

## Where it can live, which decides its shape

**`scene::UndoStack` cannot become the session history, structurally.**
`tools/check_layering.py` allows `scene` to include only
`{parallel, kernel, math, field}` — not `voxel`, not `mesh`. A history that
reverses a voxel pass and a vertex delta must see both, so it cannot be in
`scene`, and no amount of care makes it fit there.

The modules that may see all three today: `brush`
(`{scene, voxel, field, mesh}`), and `io` transitively. Neither is right.
`brush` is the stroke engine and says so — it earns its unusual position with
one call that needs a mesh and a mask together. `io` is serialization.

So this change adds a module. That is not a novelty here: `parallel` exists for
exactly this reason — the pool lived inside a backend, the layering rule
forbade the core library from reaching it, and the answer was to place it
properly rather than to route around the rule. A `session` module above
`scene`, `voxel` and `mesh` is the same move, and `check_layering.py` gains one
line.

**This settles open decision 2.1 for a structural reason rather than a
taste one:** the session history WRAPS `UndoStack` and does not replace it.
`UndoStack` needs only `scene` and should stay there, keeping its coalescing
and grouping; the session history sits above and dispatches to it. Replacing it
would drag the command stack up a layer for no benefit.

**Ownership stays where it is.** Today `scene::UndoStack` is instantiated by
each binding — `clay_document` holds a `unique_ptr<UndoStack>`, and `PyDocument`
holds its own. That is the thing to fix while doing this: two bindings each
owning a history is two implementations that will drift, and the session
history is a bigger surface to drift in. It belongs on `io::ClaySpaceDoc`,
which already owns the scene document, the voxel grids, the mask fields and the
mesh layers — so the index needs no new ownership and no new lifetime rule, and
both bindings get one implementation.

## The shape

    struct Step {
        enum class Owner { Scene, Voxel, Mesh };
        Owner owner;
        // exactly one of these is meaningful, chosen by `owner`
        std::size_t scene_entry;      // index into UndoStack's own stack
        struct { scene::LayerId layer; std::size_t pass; } voxel;
        struct { scene::LayerId layer; mesh::VertexDeltas deltas; } mesh;
    };

Undo pops the newest `Step`, dispatches on `owner`, and pushes it to the redo
side. That is the whole mechanism.

## Why an index and not a merge

`scene::Command` is a nineteen-alternative variant designed to be small and
serializable — `sizeof(Command)` is 128 bytes inline. A voxel pass is an
unbounded list of `{cell, before, after}`; a mesh displacement is a sparse
per-vertex record. Putting either into the variant would either blow the
variant's size for every command or introduce a heap indirection into the type
the undo stack stores by value, and it would make the document format's command
chunks carry payloads that are not document state.

Each mechanism chose its representation for a reason that has not changed. The
gap was never the inverse. It was the **order**.

## Every inverse already exists

This is the finding that turns a subsystem into an index.

| representation | the inverse, today | reachable? |
|---|---|---|
| SDF / layer | `UndoStack` stores `Command` inverses | public |
| voxel | `SculptLayerRecord::changes` holds `{cell, before, after}` in the order the pass touched them; `VoxelGrid::revert_from` / `apply_from` replay it | **private** — they serve sculpt-layer strength and visibility |
| mesh | `mesh::VertexDeltas::revert(Mesh&)` / `apply(Mesh&)`, both idempotent and refused against a mesh of the wrong vertex count | public |

So the build is: expose a pass-scoped revert on `VoxelGrid` beside the private
one it already has, keep a `VertexDeltas` per mesh step, and record the order.

## What is genuinely hard here

**Voxel sculpt layers are addressable, not a stack.** A user can dial layer 2's
strength after making layer 5, and that is a feature — it is what a sculpt layer
IS. So the history has two different kinds of voxel step:

- *the pass* — reversed by replaying its `before` values,
- *a change to a pass* (strength, visibility, order, merge-down) — reversed by
  restoring the previous value.

Conflating them would mean undoing a strength tweak removed the pass. They are
separate step kinds, and the spec says so.

**Merging a layer down destroys a pass.** `merge_sculpt_layer_down` folds one
record into another; the folded record is gone. Undoing it means restoring a
record, which means the step has to *hold* it. That is the one voxel step whose
memory is proportional to the pass rather than to a handle, and it is worth
naming because `add-history-budget` will have to account for it.

**A mesh layer's vertex count can change.** `VertexDeltas::revert` is refused
against a mesh of a different vertex count — correctly, because that is a caller
pairing a record with the wrong mesh. But if anything can change a mesh layer's
vertex count between an edit and its undo, the step becomes unreversible and the
history must say so rather than fail at the moment the user presses undo.

**Redo after a new edit.** The existing `UndoStack` discards redo on the next
edit. The session history must do the same, across representations, or a redo
could re-apply a voxel pass onto cells a later SDF edit has already changed.

## What stays outside

Consolidate, rasterize and the representation bridges are destructive and are
recorded by none of the three. They do not become steps. The history reports
that an unreversible operation lies at a point, so a host can present a boundary
rather than let a user undo *through* it and be surprised by what survives.

This is the part `correct-the-undo-scope` exists to make impossible to repeat:
the previous boundary was real, undocumented, and discovered by shipping.

## Open decisions

1. **DECIDE:** does the session history replace `UndoStack` as the public type,
   or wrap it? Wrapping keeps coalescing and grouping where they are and keeps
   the change additive; replacing gives one type to reason about. Leaning to
   wrapping.
2. **DECIDE:** what a host is told about an unreversible operation — a flag on
   the depth query, a distinct step kind, or a separate "history horizon" query.
   The cheapest thing that lets a UI draw a boundary honestly.
3. **DECIDE:** whether a mesh step stores its `VertexDeltas` by value in the
   history or borrows the one the sculptor already produced. By value is simpler
   and is what makes the step self-contained; it also doubles the memory of a
   mesh stroke.
4. **DECIDE:** does enabling the history mid-session start an empty one, or
   refuse? `enable_undo` today is a light switch and the answer must not change
   for the SDF path.
5. **MEASURE, then decide:** the memory a mixed session holds, since this is the
   input `add-history-budget` needs and that row currently assumes one mechanism.
