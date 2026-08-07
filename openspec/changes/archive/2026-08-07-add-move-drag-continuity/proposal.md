# Proposal: move drag continuity

## Why

`add-move-brush` resolves one drag. A real Move is a stream of them: the finger
moves, and the brush is re-applied every frame with a longer displacement.

Applied as landed, each frame PREPENDS another grab. A two-second drag at 60fps
leaves a hundred and twenty warps on every item it touched. That is not a
performance nicety — each one multiplies into the declared Lipschitz, so the
safe step scale collapses and the raymarcher crawls, and the document carries a
hundred and twenty records where one would do.

## What it is

Two small additions on top of the merged Move brush, both ported from the
parallel `add-document-grab` implementation that was reverted in its favour:

- **Coalescing.** A drag holds its centre and radius fixed and only grows its
  displacement, so those two identify it without a drag id having to be threaded
  through the API. `moved_chain` replaces a leading grab carrying the same
  centre and radius instead of stacking another in front of it. This is the
  discipline `AppendStroke`/`TrimStroke` coalescing already applies to strokes.

- **Preview.** `move_brush` is already pure, but neither binding exposed that:
  both resolved and applied in one call. A host wants to show what a drag is
  about to affect — or to preview it — without touching the document.

## What it is not

Not a change to the deformation, the mapping, or where the warp sits in the
chain. A drag whose centre or radius changes is a different gesture and is kept
beside the first, not folded into it.
