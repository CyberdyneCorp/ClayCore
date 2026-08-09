# Proposal: sculpt layers

## Why

Nothing in the engine records a sculpting pass as something you can dial back.

`Document` has layers, and they are a different concept: a layer holds items or
a voxel grid, and it can be hidden, locked and transformed. That is an
*organisational* layer. What is missing is a *strength* layer — the thing a
sculptor uses to try a pass, look at it at 40%, and turn it off without undoing
the twenty strokes that came after it.

Undo is not a substitute and the difference matters. Undo is a stack: to remove
a pass you made ten minutes ago you must discard everything since. A layer is
addressable: you keep working and revisit the pass whenever you like. Every
sculpting package has both for that reason.

The absence bites hardest exactly where sculpting is least certain. Deciding
how much muscle definition a figure wants, or how deep a panel line should cut,
is a judgement made by looking — and looking is much easier when the answer is
a slider than when it is a re-sculpt.

## Approach

A sculpt layer records the *difference* a pass made, not the result, so its
strength can be re-evaluated at any time:

- on a voxel grid, the cells the pass changed and what they were
- on an SDF layer, the edits the pass appended

with a strength in [0, 1] and a visibility flag, composited in order.

Voxel occupancy is binary, which makes partial strength interesting rather than
obvious: 40% of a pass cannot mean "40% of a cell". The natural answer is the
one the falloff brushes already use — dither the pass's changed cells against
the same cell-coordinate hash, so a strength of 0.4 keeps a reproducible 40% of
them, and the mechanism is one a reader has already met. That should be stated
explicitly rather than discovered.

## Open questions

- **What starts and ends a pass.** A stroke, an explicit begin/end, or a host
  decision. Undo already brackets gestures with `begin_undo_group`, so there is
  a precedent to follow or deliberately not follow.
- **Cost.** A layer storing changed cells and their previous values is
  proportional to the pass, not the model — but many passes over the same
  region accumulate, and the budget needs stating.
- **Interaction with multi-resolution**, if `add-multi-resolution` lands: a
  pass made at one level and viewed at another needs a rule.
- **Whether SDF sculpt layers are the same feature or a different one.** On the
  SDF side a "pass" is a set of appended items, and dialling its strength means
  scaling amplitudes — closer to a group with a weight than to a recorded diff.
  It may be cleaner as a separate change once `expose-scene-groups` exists.

## Impact

`voxel-engine` and `scene-model` gain the layer concept. `file-io` gains
persistence, and the size question above is a real constraint on the format.
`c-abi` and `python-bindings` gain the surface. Additive: a document with no
sculpt layers behaves exactly as today.
