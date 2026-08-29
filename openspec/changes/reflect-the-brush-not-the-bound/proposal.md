# Proposal: under symmetry, reflect the brush, not the bound

## Why

Issue #363. The move brush selected the items a drag warps by testing each
item's INFLUENCE bound against the ball. Under a layer mirror that bound is
expanded over the item's reflections (and radial copies) and dilated by the
seam support, so every participating item's bound spans the plane and a ball
on one side reaches items on both. On a 325-item fixture — a unit base, 300
dabs over the +x hemisphere, a 24-ball ridge at x 1.45 appended last — a grab
on the ridge crest selected 22 ridge balls unmirrored and 46 items mirrored:
the base, dabs on both sides, and the ridge.

Three costs followed. The extra warps were no-ops: the compiler evaluates a
mirror copy as the item's whole record — deformer chain included — at the
reflected point, so a grab centred on the ball moves the item near the ball
and its copy near the reflection; an item whose COPY sat under the ball had
its body where that grab weighs zero, and did not move at all (its pole read
0.00000 where everything else read -0.05945). A 12-segment gesture along the
ridge accumulated 532 warps mirrored against 244 unmirrored, each a grab
evaluated per sample on every later refill. And because the base (root
ordinal 0) was in every mirrored drag, the gesture's frontier (#360) was 0 —
the empty prefix `plan_frontier` refuses — so under a mirror the dirty-prefix
path never engaged: resumed 0 / refilled 24 on the fixture's window, every
frame, with the probe reporting the entry erased. The sculpt was also
asymmetric between a drag and its mirror image (5,536 grid samples apart by
the whole pull), since the +x drag moved A and the -x drag moved B.

A fourth, found while proving the fix: the gesture's stated reach (#361) was
the ball alone, so a seed warm on the reflected side was advanced to the new
revision still describing the undragged copies — 997 of 8,192 samples stale,
up to 0.023, the whole pull.

## What Changes

- The brush is stated as the IMAGES the layer's symmetry makes of it —
  `brush::drag_images`: the ball, one reflection per mirror axis, one
  rotation per radial copy; additive, exactly the copies `emit_item` emits.
  Every item is tested on its OWN bound (`scene::item_own_influence_bound`,
  a factor-out of the geometry bound without the copies) against each image,
  and takes one grab per reaching image, at that image's centre with that
  image's displacement. Items that do not participate in the symmetry see the
  ball alone — the gate is `emit_item`'s, repeated.
- `MoveWarp` is one per NODE, carrying the node's grabs in a fixed order by
  value and the identity of every image the node can see; `moved_chain`
  replaces every leading grab of the gesture, not only the front one.
- `clay_layer_move_surface` states its reach as one box per image, and the
  seed store's region invalidation takes several boxes at once.
- Under no symmetry there is one image and the resolved warps are byte for
  byte what they were.

## Impact

- The 325-item fixture selects 22 items mirrored, the same set as unmirrored,
  and never the base; the 12-segment gesture leaves 244 warps either way;
  the mirrored drag states frontier 302, its bricks resume as the unmirrored
  ones do, and the refill matches a fresh mirrored oracle bit for bit.
- A drag at +x and its mirror image at -x produce identical chains and
  identical fields (0 samples differing) on an identity layer transform.
- A drag centred on the plane, pulling along it, takes two coincident grabs
  and lifts 1.73x what one grab did (0.1645 against 0.0950 on a ball of
  radius .4), continuously as the centre leaves the plane (0.1645, 0.1650,
  0.1600, 0.1460 at x = 0, .01, .05, .1). This is the mesh-sculpt default:
  each symmetry pass applies and overlaps compose.
- After the 12-segment gesture on a 150-brick window, a mirrored refill
  went 63.5 ms to 20.8 ms per segment and the full walk 160 ms to 92 ms
  (532 to 244 warps); unmirrored unchanged at 14.6 ms.
- Benchmarks: `BM_MoveDragMirrored1000` (warped_ratio 1.0, in items),
  `BM_MoveDragRefillMirrored` / `MirroredCold` (0.63 ms against 8.6 ms,
  0.07x; the unmirrored pair reads 0.16x). Unmirrored rows unchanged.

## Non-goals

- No kernel change. The grab is what it was; only where it is aimed changes.
- No deduplication of coincident or overlapping images. Collapsing two
  coincident grabs to one keeps the one-sided pull at exactly x = 0 and drops
  it the moment the centre leaves the plane — a discontinuity at one float
  value. A kernel-level overlap reducer (Blender's "symmetry feather") is a
  possible follow-up, opt-in, not part of this change.
- The evaluation-bound contract is untouched: `item_influence_bound` still
  covers every copy, and culling, invalidation and `drag_frontier`'s spans
  keep using it.
- Feathered-replace and opt-out participation semantics are unchanged; the
  brush repeats the compiler's gate rather than restating it.
- Per-drag mirror symmetry of the field (f(p) == f(Rp) after any drag) is
  structural — the layer field is a symmetric expression in the item copies
  — and neither created nor at risk here.
