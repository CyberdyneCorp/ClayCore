# Design: radial symmetry

## The one real decision: copies in the tape, or a fold in point space

There are two ways to make a field radially symmetric, the engine already
contains one of each, and they are not interchangeable.

**A fold in point space** is what `crep_radial_point` does for
`Repeat::radial`: map the query point into the nearest sector and evaluate
there. Cost is O(2) regardless of count — it evaluates the item twice, for the
sector and its angular neighbour, and takes the nearer. It is beautiful and it
is a MODIFIER: the fold happens inside one item's evaluation, so the item is
the thing arrayed. Nothing about it reaches a stroke, because a stroke is many
items and folding each one separately arrays each stamp about the axis
independently — which is not what "sculpt radially" means.

**Copies in the tape** is what the layer mirror does at
`src/scene/tape_build.cpp:663`: emit the item again with a reflected inverse
matrix and combine the two with `Op::Add` under the Mirror Blend. Cost is one
extra instance per copy. This is the one that composes with everything else —
culling sees real items with real bounds, the seam blend is an ordinary combine,
per-item opt-out is a flag on the node, and a stroke works because its stamps
are ordinary items on a layer that arrays every item it holds.

**This change takes the second**, because it is a symmetry MODE and the first
cannot be one. The cost is stated rather than discovered: emitted instructions
per participating item scale with `count`, so a 6-fold radial layer compiles six
times the prim instructions. `Repeat::radial` remains the right tool for a
24-fold decorative array, and remains available on any item.

That is the trade in one line: **the modifier is cheap and cannot be a mode; the
mode is a mode and cannot be cheap.**

## Following the mirror exactly, including where it is odd

The mirror emits ONE reflection per enabled axis rather than the `2^n` of a full
multi-plane symmetry — x and y give the original plus two reflections, not the
four quadrants. Radial composes with it the same way: each mode contributes its
own copies of the base item and the products are not emitted.

This is a deliberate consistency rather than a limitation nobody noticed. A
sculptor turning on both wants radial arms each mirrored, which the products
would give — but emitting products means the two modes multiply, `count * 2^axes`
instances per item, and the existing mirror already established the additive
convention. Changing both at once is a larger decision than this change, and it
would change what existing mirrored documents evaluate to. Recorded as a
scenario so the behaviour is chosen rather than inherited.

## Where the rotation goes

The mirror builds `item^-1 * R * layer^-1` — reflect in the layer's local frame,
between the item's inverse and the layer's. The rotation is the same shape with
`math::rotation_matrix(axis, angle)` in place of `reflection_matrix(axis)`. A
rotation has determinant +1 where a reflection has -1, so unlike the mirror it
is expressible as a `Transform`; it is built as a matrix anyway, so the two paths
read alike and the reader is not asked to notice why one is special.

## Bounds

`bounds.cpp:648` dilates a mirrored item's bound by the blend support and unions
the reflected boxes. Radial does the same with rotated boxes. A rotated AABB is
not an AABB, so each copy's bound is the axis-aligned bound OF the rotated box,
which over-covers — correct, and cheaper than the alternative of rotating the
item's own geometry bound. Over-covering costs cull precision, never correctness.

## Serialization

The layer record gains three fields after `mirror_k`, gated on the scene minor
so a document written before them loads with the mode off. `kSceneMinor` and
`kClaySpaceMinor` move together — `src/io/clayspace.cpp` static-asserts that they
are equal, which is the guard that keeps the two from drifting.

## What was considered and rejected

- **Radial as a third mirror axis.** It is not a plane, it has a count, and
  overloading `mirror_axes` would make the bitmask mean two different kinds of
  thing.
- **Folding at the layer level instead of per item.** A layer-level fold would
  make the layer's whole accumulated field radial, which sounds equivalent and
  is not: a subtract inside the layer would then carve every sector including the
  ones it was not drawn in, because the fold happens before the chain rather than
  per item.
- **A world-space axis.** Rejected against the roadmap's own P0 criterion that a
  symmetry centre be explicit, persistent and moveable. The layer-local axis is
  all three for free.
