## 1. The preview

- [x] 1.1 `clay_sdf_move_preview_document` returns a borrowed read-only document
      carrying the drag, built on first request so a host that never previews
      pays nothing
- [x] 1.2 It shares the transaction's own preview content, so an update is
      visible through the same handle with nothing to refresh
- [x] 1.3 ...and the document is TOUCHED, so the tape cache does not serve the
      first frame's answer for the rest of the gesture
- [x] 1.4 It dies with the gesture — commit, cancel and destroy all drop it
- [x] 1.5 A drag that reaches nothing still previews: the layer unchanged is the
      honest answer, not a null a host must branch on every frame

## 2. Taking a deformer back

- [x] 2.1 `clay_layer_deformer_count`
- [x] 2.2 `clay_layer_remove_deformer`, refusing an index past the end
- [x] 2.3 `clay_layer_clear_deformers`, succeeding on an empty chain
- [x] 2.4 All three are undoable edits, sharing one lookup and one command

## 3. Proof

- [x] 3.1 The preview carries the drag, the real document does not move, and the
      saved bytes are unchanged
- [x] 3.2 It carries the WHOLE scene, not only the dragged layer
- [x] 3.3 A second update is visible through the same handle
- [x] 3.4 What the preview showed is what the commit writes
- [x] 3.5 Commit and cancel both spend it; a null handle previews as null
- [x] 3.6 A drag that reaches nothing still previews
- [x] 3.7 Remove, count and clear behave, and their refusals are refusals
- [x] 3.8 Removing is one undoable edit that restores the warp exactly
- [x] 3.9 3.3 and 3.4 fail when the touch is removed
