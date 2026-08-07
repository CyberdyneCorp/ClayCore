# Tasks: add-move-brush

- [x] 1.1 `SetDeformersCmd`: whole-list replace, exact inverse, serialization tag
- [x] 1.2 `brush::move_brush`: a world drag into per-item grabs, each in its own
      frame, each marked for the FRONT of its chain
- [x] 1.3 The world-to-local mapping accumulates the transform chain from the
      layer down, not just the node's own transform
- [x] 1.4 Cull by influence bounds: an item the drag cannot reach gets no warp
- [x] 1.5 C ABI: the move, and replacing a node's deformers; one undo step
- [x] 1.6 Python bindings for both
- [x] 1.7 Tests: a blended form moves as one and symmetrically; a single-item
      grab does not; a node under a transformed group still moves where the drag
      was aimed; out-of-reach items are skipped; order in the chain matters and a
      prepended warp acts where it was aimed; the pull is monotonic; the whole
      move is one undo step; the chain round-trips through the format
- [x] 1.8 An example, its `CAPABILITY_EXAMPLES` note, docs, roadmap, full
      verification

Found while building:

- [x] 1.9 There is no transform chain to accumulate. `compile_group` passes the
      layer through and `emit_item` uses `layer.xform * item.xform`, so a
      GROUP'S TRANSFORM NEVER REACHES ITS CHILDREN — checked directly, a sphere
      under a group translated to x = 2 evaluates at the origin. The expected
      hard part of the mapping does not exist, and a resolver that accumulated
      would have disagreed with the evaluator. Worth flagging separately: a
      group carrying a transform silently does nothing.
- [x] 1.10 `clay_layer_move_surface`, not `clay_layer_move` — that name was
      taken by reparenting a node in the tree. The collision is a good one: the
      two move different things and confusing them would be easy.
- [x] 1.11 A pre-existing C ABI bug, found while reusing the deformer decoder
      and verified against origin/main: `clay_item_add_deformer` bounds at
      `CLAY_DEFORM_POSE_LINE` (11), while MAGNIFY is 12 and NOISE is 13. Both
      are declared, documented, carry parameter counts and are handled by
      `make_deformer` — and were refused at the door, so Python could reach them
      and C could not. The binding parity gate cannot see it: it checks that the
      ENUMERATOR exists, not that a call accepts it. Fixed with a regression
      test.
