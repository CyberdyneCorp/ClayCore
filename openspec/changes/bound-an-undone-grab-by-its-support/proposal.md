## Why

Undoing a Move on a field layer costs more the longer the layer's chain of
edits is, at a flat triangle count. ClaySpaceDesktop measured it against
v0.120.0 (issue #639): **20 ms -> 4,536 ms** over 50 undo steps in a live
session, and **61 ms at gesture 11 against 362 ms at gesture 40** in a host
test.

A host Move is a grab put at the HEAD of a node's deformer chain, one per
segment (`brush::moved_chain`). The host dirties what `clay_document_undo_bound`
reports, and that is, per command, the union of what the command TARGETS before
and after — for a `SetDeformersCmd`, the NODE. So undoing one segment refills
the node's whole influence bound, and that bound itself grows with every grab
on the chain, because `deformed_local_bounds` dilates the node's whole box by
each grab's pull. Undo cost ~= bricks-in-the-node x chain length: the first
factor is fixed by the node (and grows with the chain), the second grows per
gesture.

## Reproduced on this engine

Probe: `benchmarks/undo_grab_bound_probe.cpp`, C ABI only. A sphere node of
radius 1.5, N grabs of radius 0.2 around its equator, one grab of radius 0.15 on
its pole at the head of the chain; undo it, mark the reported bound, refill.
Bricks of 8^3 at voxel 0.05. On origin/main at `49f4418a` (ABI 0.120.0):

| grabs on the chain | bricks the undo marks | the grab's own ball marks |
|---:|---:|---:|
| 1 | **1,000** | 12 |
| 10 | **1,440** | 12 |
| 40 | **4,000** | 12 |

The undo's count IS the node's (`clay_layer_node_influence_bound` marks the
same 1,000 / 1,440 / 4,000), and it grows with the chain although the edit is
the same 0.15 ball every time. The acceptance test
`undoing one grab marks a count independent of the node's size and its chain`
reads the same shape at other sizes: 1,000 bricks for a radius-1.5 node, 32,768
for radius 6, 61,952 for radius 6 with 40 grabs — for an edit whose ball marks
12.

## Measured after

Same probe, both arms built from the same source with `clang++ -O2` against
each tree's `cpu-only` Release `libclaycore.a`; Mac (M-series), CPU backend;
200 timed undos per point after five discarded, medians, the two binaries
interleaved three times per point after one throwaway run of each. The three
repeats agree to within 0.1 ms (main) and 0.005 ms (branch); the middle one
is shown.

| grabs | bricks main -> branch | undo p50, main -> branch | per refilled brick, main -> branch |
|---:|---:|---:|---:|
| 1 | 1,000 -> **12** | 1.665 -> **0.089 ms** (18.7x) | 1.66 -> 7.42 us |
| 10 | 1,440 -> **12** | 2.481 -> **0.086 ms** (28.8x) | 1.72 -> 7.17 us |
| 40 | 4,000 -> **12** | 7.415 -> **0.115 ms** (64.5x) | 1.85 -> 9.59 us |
| 80 | -- -> 12 | -- -> 0.158 ms | -- -> 13.18 us |
| 160 | -- -> 12 | -- -> 0.337 ms | -- -> 28.12 us |

**The count is flat and asserted; the clock is not the claim.** The brick count
is what the tests pin (`undoing one grab marks a count independent of the
node's size and its chain`: 12 bricks at node radius 1.5 and 6, at 1 and 40
grabs, where main marks 1,000 / 32,768 / 61,952).

**The chain-length factor is still there, and this is it.** On the branch the
same 12 bricks cost 7.4 us each at 1 grab and 28.1 us at 160, although none of
the equator grabs reaches the pole the bricks sit on. The issue's second factor
-- the price of one refilled brick grows with the chain -- is untouched by this
change, and WHERE that per-brick cost goes on this fixture (compile, cull, or
evaluation) is not measured here. What is removed is the first factor, the
node's extent. The per-brick column is not comparable across the arms: most of
main's 1,000-4,000 bricks are proven uniform or empty and cost almost nothing,
while the branch's 12 are the surface bricks the grab actually moved.

## What Changes

- **A deformer step reports the deformer's support, clamped into the node's bound.**
  When the chains before and after a `SetDeformersCmd` differ only in a HEAD of
  links that are exactly the identity outside their own ball — grab, magnify,
  blob, alpha, under an easing that is exactly zero at the rim on every backend
  — `UndoStack::replay` clamps those balls into that command's before/after
  bound (a grab's at its centre and its displaced end), placed as the item is
  placed (every mirror and radial copy, every instancing layer), dilated per
  enclosing group and per layer fold. One rule: `scene::command_head_delta_bound`
  over `scene::deformer_head_reach_in_document` (design.md D1-D5).
- The placement is `placed_local_bound`, which is `geometry_bound`'s body with
  the local box made a parameter — not a second spelling of it (D3).
- `clay_document_undo_bound` / `_redo_bound` keep their signatures. **No new
  symbol, no version bump**: ABI stays 0.120.0. What they return for a deformer
  step is now tighter, and never tighter than what changed.
- `clay.h`, `docs/05`, `docs/06` say what a deformer step reports and what it
  does NOT promise.

## What is NOT changed, and why

- **The per-brick price.** Each refilled brick is still evaluated through the
  whole chain, so one brick costs more on a longer chain. This removes the
  node-extent factor only, and the measurement above shows both.
- **Radial pose**, which has finite weight and is still not the identity past
  its ball (D2), and any head containing a whole-item deformer, keep the node's
  bound.
- **The forward path** (`apply_edit`) is unchanged (D7).
- **The issue's second route** — make a refill over a baked volume cost close to
  an analytic item's (~430 us against ~7 us a brick, the host's numbers) — is a
  separate problem and is not attempted here.

## Capabilities

### Modified Capabilities

- `c-abi`: "Undo reports the region it changed" gains the deformer-head
  narrowing and five scenarios.

## Impact

- `src/scene/bounds.cpp`, `include/clay/scene/bounds.h` — the head reach and the
  extracted placement.
- `src/scene/commands.cpp`, `include/clay/scene/commands.h` — the command-level
  rule and the replay clip.
- Tests: `test_c_undo_bound_grab_support.cpp` (C ABI), `test_deformer_head_reach.cpp`.
- `benchmarks/undo_grab_bound_probe.cpp` — not gated.
- A host that dirties by the undo bound refills the grab's ball instead of the
  node. A host that ALSO widened it by hand keeps working; it just pays for it.
