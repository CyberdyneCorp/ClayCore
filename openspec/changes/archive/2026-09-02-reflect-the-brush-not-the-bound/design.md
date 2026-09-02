# Design

## What a copy is, and why the bound was the wrong thing to widen

`emit_item` compiles a participating item as itself plus one instance per
mirror axis and `radial_count - 1` rotated instances, each the item's WHOLE
record — primitive, per-axis scale, deformer chain — evaluated at
`item^-1 · R^-1 · layer^-1 · p`. A grab in the chain with local centre
`local(W)` therefore moves the item's body near `W` and its copy near `R·W`.
For an item whose body sits near `R·W` — its copy is what the user sees under
the ball — the grab centre `local(W)` is two diameters away from the body and
the region weight is zero everywhere: neither the body nor the copy moves.
Measured on the oracle fixture: the copy's pole under the ball read a delta of
0.00000 where the dragged item read -0.05945.

Widening the bound made those items candidates and gave them a no-op. What
moves the copy under the ball is a grab whose local centre is `local(R·W)`
with displacement `R·D` — the REFLECTED brush. So the drag is stated as its
images and every item is tested on its own bound against each.

## Why the images are exactly the compiler's copies

The images are `{W} ∪ {R_axis W per set mirror axis} ∪ {R_{-2πk/C} W per
radial copy}` — `1 + popcount(mirror_axes) + (radial_count - 1)`, additive,
never products, because that is what `emit_item` emits (x|y is two
reflections, not four quadrants; `layer_symmetry_multiplicity` says the same
for the cull pad). An image with no copy behind it would select an item and
warp its body where the compiler put no geometry. The same reasoning gates
which images an ITEM sees: an opted-out item or a feathered volume replace is
emitted once, so it sees the ball alone; a non-local item (infinite own
bound) is under every image's ball and takes one grab per image it sees.

Images are computed in layer-local coordinates and mapped back through the
layer transform, because the symmetry is the layer's. On an identity
transform a reflection is an exact sign flip, which is what makes the +x
drag's internal image bit-identical to the -x caller's centre and the two
documents' chains identical.

## Why one warp per node, with the grabs in value order

An item both images reach — one straddling the plane — takes both grabs, as
two brushes would. They go in ONE `MoveWarp` and one `SetDeformersCmd`, so a
preview and `*out_applied` count items, and so `moved_chain` sees the whole
set: applied one grab at a time and keyed on the front deformer, a straddler's
chain grew 2 → 4 → 6 over three frames.

The two grabs are ordered by their VALUES — lexicographic on (centre,
displacement), descending — never by which image produced them. Once the item
is not itself plane-symmetric the two orders are two different fields: an
off-centre straddler ordered "self first" measured 325 grid samples apart
between the +x and -x documents; ordered by value, 0. The displacement is in
the key because a drag centred ON the plane gives both images one centre and
opposite pulls, and a centre-only key measured 1,365 samples apart.

## Why a warp names every image, not only the reaching ones

A grab dilates its item's bound by its displacement, so an image that missed
the item at frame one can reach it at frame two, and when the user drags back
it can stop reaching again. `moved_chain` recognises the gesture's earlier
frames by (centre, radius); matched against this frame's reaching grabs alone
the second case leaves the no-longer-reaching image's grab in the chain with
its stale pull. `MoveWarp::gesture` carries one grab per image the item can
see, reaching or not, and every leading chain entry matching any of them is
this gesture's and is replaced. Stopping at the first non-match is what keeps
a different gesture's grab, and any other deformer, in place.

## Why no deduplication of coincident images

Two images coincide when the drag is centred exactly on the plane. Keeping
both composes the two pulls: along the plane they add (a ball of radius .4
lifts 0.1645 under two grabs against 0.0950 under one, 1.73x), across it they
cancel to a pinch (the straddler's extent narrows). Collapsing bitwise-equal
centres to one grab would keep the single-grab result at exactly x = 0 and
switch to the two-grab result at x = 0.001 — a .03 jump in the straddler's
extent at one float value. The two-grab rule is continuous there (extents
.2805, .238, .2285, .2275 at x = .05, .01, .001, 0) and is what a
mesh-sculpt mirror does by default; an overlap reducer is an opt-in in those
tools and is a follow-up here, in the kernel, not a selection rule.

## Why radial is in the same change

The radial defect is the same defect through the same bound expansion, and
the fix is the same loop: copy k is the item rotated by `+2πk/C`, so the image
that reaches it is the ball rotated by `-2πk/C` — the inverse `emit_item`
composes. A rotation is inexact in float, so the rotated-image drag matches to
a tolerance (≤ 6e-8 measured) where the mirror matches bit for bit.

## Why the reach is several boxes

The copies move where the images are, so the gesture's reach must cover every
image's ball or a seed warm on the reflected side is served stale (997 of
8,192 samples, up to 0.023, before). Stated as the UNION of the balls the
reach is the slab between them — under a mirror, the whole document — and
every warm brick resumed every frame for ground the drag never touched
(0.35x the cold refill against 0.16x unmirrored). So `GestureRegion` holds one
box per image and the seed store's invalidation takes them under one lock,
one revision: still one invalidation per gesture.

## How this composes with the prepared drag (add-sdf-sculpt-transaction)

`prepare_move` / `resolve_prepared_move` split a drag into the half that
depends on the anchor and radius and the half that depends on the displacement.
Reach depends on centre and radius only, so under symmetry it is decided at
prepare exactly as it was decided in `collect`: every item on its own bound
against every image's ball, the balls boxed once per drag, the participation
gate consulted only when there is more than one image. A `PreparedMove` then
carries its IMAGES — for each image the item sees, the local centre, whether
that image reaches the item, and the reflection or rotation (in the layer's
frame, with the layer transform alongside) that makes the image's displacement
out of the drag's. `resolve_prepared_move` maps the total displacement through
each image with the arithmetic `drag_images` uses, so the +x drag and its -x
mirror image stay bit-identical on an identity layer transform, and sorts the
reaching grabs by value; the rest land in `gesture`. `move_brush` is the two
halves composed, so a live drag under a mirror previews and commits what the
one-step resolver produces, sample for sample.

The per-frame contract holds: one resolve per affected item, no scene access,
`visited` flat across the scaling rows. What the unmirrored frame now pays is
the one allocation `MoveWarp::deformers` costs, since a warp carries a chain
of grabs rather than one inline; the prepare pass pays one more per reached
item for `PreparedMove::images`, once per gesture in a transaction and once
per call through `move_brush`.

`SdfMoveTransaction::preview_grabs` and the C ABI's
`clay_sdf_move_preview_grab_count` / indexed `clay_sdf_move_preview_grab`
report every reaching grab — a straddler has two — because a host that drew
the first alone would preview half the drag. The transaction's dirty box is
the union of every image's swept ball; it is one box by contract, so under a
mirror it spans the plane, and a host keeping the images apart has
`brush::drag_images`.
