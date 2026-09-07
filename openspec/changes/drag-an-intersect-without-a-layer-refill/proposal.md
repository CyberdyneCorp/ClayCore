# Drag an intersect without refilling its layer

## Why

Dragging a boolean operand across a form re-meshes the whole layer when the
operand INTERSECTS, and only its own neighbourhood when it subtracts. The
reporter of #471 measured the same drag, the same document, the same 97 items,
differing only in the operator:

| fixture | subtract refill | intersect refill |
|---|---|---|
| reference | 3.8 – 4.2 ms | 41.5 – 44.0 ms |
| ten times the extent | 12.8 ms | 6.8 – 10.1 **seconds** |

Nothing about an intersect is harder to evaluate. What differs is the region the
edit dirties. `src/scene/bounds.cpp` classifies `Op::Intersect` as
`Nonlocality::BoundedByLayer`, `item_influence_bound` resolves that to the
layer's extent, `node_command_bound` unions it over the layers sharing the
content, and the refill follows.

**That classification is correct and this change does not touch it.**
`max(acc, item)` is the item's own value everywhere the item is not, so an
arbitrary edit to an intersect — changing its primitive, its operator, its
group, hiding it — really can change the field anywhere the layer has material.
The measurements behind it are in the file: over 400,000 sample points the
band-clamped drift outside the layer's extent is exactly 0, against 0.100 and
0.065 outside the item's own geometry.

The performance defect is that a general answer is being used for a specific
question. Remeshing does not ask "where can this node change the field". It asks
"where can THIS EDIT have moved the surface", and for one particular edit — an
in-place move of an existing operand — the answer is much smaller.

## What lands

A second, narrower bound beside the influence bound, for exactly one edit kind.

- `scene::item_geometry_reach_in_document` — the box a change confined to one
  item's own geometry reaches in the document: the item's geometry bound,
  dilated by its layer's chain pad, by each enclosing group's blend support and
  by the layer folds above, unioned over every instancing layer. It is
  `node_influence_bound_in_document` with the intersect arm taken out, and it is
  sound only as HALF of a before/after pair.
- `scene::command_surface_delta_bound` — the pair, for a `SetTransformCmd` on a
  visible Intersect item with finite support. `std::nullopt` for every other
  command, op, and for every case the proof does not cover.
- `apply_edit` uses it when BOTH sides claim one, and keeps the conservative
  union otherwise.
- `clay_layer_set_transform_bound` (ABI 0.90.0) — the same edit a host already
  makes, plus the region it changed, ready for `clay_brick_cache_mark_dirty`.
  `clay_layer_node_influence_bound` and `clay_brick_cache_mark_dirty_nodes` are
  unchanged: they answer for an arbitrary edit and must stay conservative.

## The argument

Outside the swept union of where the operand was and where it went, the
operand's own field is a positive beyond the band on both sides. `max(acc, item)`
therefore returns a beyond-band positive on both sides, and the band-clamped
value — which is what a brick stores and what a mesher reads — cannot have
moved.

What it does NOT claim is that the RAW value is unchanged out there. It is not:
it is the moved operand's own distance. That is the difference between this and
a local op, whose combine outside its support is the identity bit for bit, and
it is why the CHAIN PAD is a term here and is not one in a local op's bound: a
smooth combine further down the chain can drag a beyond-band difference back
toward the band. `cull_pad` is the measured distance over which it can, and this
reuses that expression rather than spelling a second one.

## What the measuring found

**The reproduction, in `BM_OperandDrag*`** — the issue's fixture, one drag frame
per iteration, 98 items, one voxel size at both extents. A 24-thread Linux
desktop at load average 3.5–4.1; the milliseconds are what this machine did on
this run and are NOT a gate (it reads the same unrelated pair 0.43x and 1.72x
minutes apart), the brick counts and volumes are deterministic:

| row | dirty bricks | AABB / layer volume | bound | refill | remesh | total |
|---|---|---|---|---|---|---|
| subtract, reference | 63 | 2.9% | 0.0018 ms | 2.81 ms | 2.22 ms | 5.04 ms |
| intersect, layer bound, reference | 576 | 100% | 0.0146 ms | 22.63 ms | 5.20 ms | 27.9 ms |
| intersect, swept delta, reference | 121 | 7.9% | 0.0161 ms | 5.66 ms | 1.81 ms | 7.51 ms |
| subtract, ten times | 63 | 0.11% | 0.0019 ms | 2.91 ms | 1.63 ms | 4.57 ms |
| intersect, layer bound, ten times | 9,680 | 100% | 0.0218 ms | 328.90 ms | 67.81 ms | **397 ms** |
| intersect, swept delta, ten times | 286 | 1.2% | 0.0178 ms | 12.19 ms | 3.45 ms | **15.7 ms** |

The pathology reproduces — 27.9 ms a frame at the reference size against 5.04
for the subtracting control, and 397 ms a frame at ten times the cross-section,
which is the issue's "seconds per frame" at this fixture's resolution — and the
swept bound removes it: **x3.7 at the reference size and x25 at ten times**,
with the region growing x2.4 in bricks where the layer bound grows x16.8.

Computing the delta costs about 0.002 ms a frame more than the influence bound
it sits beside, because the influence bound is still taken on both sides so a
refusal can fall back to it. That is 0.01% of what it saves.

**The acceptance condition, in COUNTS** (`test_intersect_delta_oracle.cpp`, the
same fixture at radius 1 and radius sqrt(10), same item count, same cutter, same
drag):

| region | reference | ten times the cross-section | growth |
|---|---|---|---|
| conservative (layer) | 900 bricks | 15,600 bricks | **x17.3** |
| swept delta | 540 bricks | 1,152 bricks | **x2.13** |

The residual x2.13 is not the extent. It is the CHAIN PAD: the fixture's blend
radii scale with the form, as a real sculpt's do, and the pad follows them. The
pad is also what makes the delta region 4.5x the subtracting control's at ten
times the extent (286 bricks against 63) — the operand and its sweep are
identical, and the difference is entirely that dilation.

**Three terms were tested by removing them.** Only one of them is load-bearing
on the fixtures built here, and saying so is the point of having measured:

| term removed | probe (field samples) | oracle (incremental vs rebuild) |
|---|---|---|
| the layer folds above (`folds_from_layer_support`) | **203 sign changes, 229 band entries, 265 band exits**, worst 0.529 | **93 bricks whose stored values differ from the rebuild** |
| the chain pad (`cull_pad`) | no violation found | no violation found |
| the ancestor group blend supports | no violation found | no violation found |
| the sweep itself (the after box alone) | violations, by construction — the probe's own self-test | — |

The fold term took two attempts to make the ORACLE see, and the second attempt
is in the fixture rather than in the test: a fold only moves the document's
surface where the two layers' fields are within its support of each other, so
the first fixture — a small sphere folded onto the form beside the cutter — left
the term untestable while the probe was already failing on it. The fixture is
now a box over the whole form, and dropping the term costs 93 stale bricks.

The pad and the group supports stay in. They are there on the argument above,
they cost 0.2 world units on the reference fixture and 0.38 at ten times it
(which is the 4.5x above), and the honest report is that no fixture built here
can tell whether they are needed — not that they were shown to be. Removing the
pad is a real, measurable win and it needs the breadth of evidence
`blend_cull_pad`'s own campaign carries — 700+ configs, up to 160,000 in-band
samples each — because the direction of that error is stale geometry with
nothing to point at.

## What is deliberately not in this change

- Any narrowing of `item_nonlocality`, `item_influence_bound`,
  `node_influence_bound_in_document`, `clay_layer_node_influence_bound` or
  `clay_brick_cache_mark_dirty_nodes`.
- Any other edit kind: a prim change, an op change, a blend change, a stroke
  append, a group edit, a layer-level intersect's own transform. Each is a
  separate question with its own proof.
- A per-brick predicate inside the swept box (the guide's optional second
  stage). The region was the dominant cost; refine it only if a measurement
  asks.
- A threshold in `tools/check_bench.py`. This machine reads the same unrelated
  pair 0.43x and 1.72x minutes apart; the counts are gated instead, in the unit
  suite, where they are deterministic.
