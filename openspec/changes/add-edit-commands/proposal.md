# Proposal: expose the mutation vocabulary — editing, not just adding

## Why

Scoping the undo layer turned up something larger underneath it: **neither
binding can modify an existing node.** `pyclay`'s `Layer` has `add` and nothing
else — no remove, no move, no way to change a transform, a primitive, a colour,
a blend. The C ABI has `clay_add_item`, `clay_remove_node` and
`clay_set_layer_mirror`, and that is the whole mutation surface.

So a document can be built and then only rebuilt. Place a sphere and you cannot
move it. Pick the wrong blend radius and you delete the item and add it again,
losing its node id. Hide a layer, reorder layers, retransform a layer — none of
it is reachable. That is not a sculpting API; it is a scene *writer*.

`scene/commands.h` already defines the whole vocabulary and the scene-model
spec already requires it: thirteen command types, each with a computable
inverse, shared with the document file format. Only the bindings are missing.

Undo depends on this. An undo stack over a vocabulary the caller cannot invoke
is worth nothing, so this lands first and `add-undo-stack` builds on it.

## What Changes

- **Node edits** in both bindings: set transform, primitive, colour,
  op/blend/rounding, and move a node to a new parent and index; remove a node.
  `pyclay` gains removal, which it does not have at all today.
- **Layer edits**: add, remove, reorder, set visible, set transform. Only
  `add_sdf_layer` / `add_voxel_layer` exist now.
- **Stroke edits**: append points to an existing stroke and trim the last N —
  the pair the engine coalesces into one undo step, and what an interactive
  sculpt stroke actually issues.
- **Node handles that survive edits.** Every edit is addressed by node id, so a
  caller can hold an id across a session. Editing does not renumber.
- Each entry point maps onto exactly one `scene::Command`, applied through
  `scene::apply`, so the binding cannot drift from the file format's semantics.

## Capabilities

### Modified Capabilities

- `python-bindings`: node and layer editing join the surface.
- `c-abi`: the same, mirroring `pyclay` as the parity gate requires.

## Impact

- `bindings/python/pyclay_module.cpp`, `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, tests both sides, `tools/check_binding_parity.py` mapping entries, docs.
- ABI 0.4.0 — additive.
- The parity gate did not catch this gap, because it compares the two bindings
  against each other and both lacked it equally. Worth noting in the gate's own
  documentation: it prevents drift, it does not prove completeness against the
  engine.
- Non-goals: group/ungroup as a single operation (the command vocabulary
  expresses it as add/remove of a subtree, and a dedicated call can follow),
  and the brick cache.
