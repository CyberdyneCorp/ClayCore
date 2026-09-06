## Context

The roadmap row is one line — "snap-to-ground, centre-mass, zero-to-origin as
single ABI calls" — and three of its four words turned out to be decisions
rather than descriptions. This file takes them, with what reading the tree said
about each.

What already exists and is not rebuilt here:

| piece | where | what it gives |
|---|---|---|
| the tight world box | `clay_layer_bounds` -> `layer_world_bounds` -> `pick::layer_bounds` | SDF, voxel and mesh content, composed with the layer matrix |
| the placement, per axis | `clay_document_{set_,}layer_transform_nonuniform` | position, rotation, three factors, in and out |
| the command | `scene::SetLayerTransformCmd` | transform AND `scale_axes` in ONE command, so one undo step |
| the world translation | `math::Transform::apply` = `rotation.rotate(p * scale) + position` | `position` is outermost, so a delta added to it is a world translation and touches neither scale |

That last row is the whole arithmetic of this change. `layer_matrix` composes
`l.xform.matrix() · scale_matrix(l.scale_axes)`, with the per-axis scale
INNERMOST, and `xform.position` applied last. Adding a world-space delta to
`position` therefore translates the layer's world content by exactly that delta,
whatever rotation and whatever squash the layer carries. No case analysis, no
frame conversion, and the same three lines serve all three calls.

## Goals / Non-Goals

**Goals:** three placements that are correct on a squashed layer; one undo step
and one invalidation each; a stated refusal wherever the bound cannot describe
the layer; no new command, no format field, no change to any existing call.

**Non-Goals:** item-level variants; a document-level snap; changing what
`clay_layer_bounds` answers; any promise about where the SURFACE lands (see
"What the box is not", below).

## Decisions

### What "centre mass" means: the box centre, and the name says so

The engine has no density. Two honest readings existed:

1. **The centroid of the layer's bounds** — the centre of the box
   `clay_layer_bounds` already returns. O(1) on top of a query the host is
   likely already making, deterministic, identical across backends, and defined
   for all three representations by the same call.
2. **An occupancy-weighted centroid sampled from the field** — bake the layer
   onto a lattice, weight each cell by its inside fraction, average. For a
   solid of uniform density this IS the centre of mass, so it would earn the
   name.

**Taken: (1).** The cost of (2) is not the arithmetic, it is everything the
signature would have to grow to carry it. A sampled centroid has no answer
until someone names a cell size — the same parameter `clay_consolidation_params`
carries, and the same one `clay_layer_consolidation_cost` exists to price. The
decided signature is `(doc, layer)` and has nowhere to put it, so (2) is not
merely more expensive here, it is **not expressible**: any implementation would
have to invent a cell size, and a convenience call whose answer silently depends
on an invented resolution is the wrong kind of convenience. Beyond that it would
be a CPU bake per press (every lattice bake in this header is CPU-only and says
so), it would answer differently for a mesh layer, which has no field until it
is rasterized, and its answer would move as the artist changed the layer's
voxel size, which changes nothing about where the shape is.

**So the call is named `clay_layer_centre_bounds`, not `clay_layer_centre_mass`.**
This is the one place the change departs from the names in the roadmap row, and
deliberately: "centre of mass" is a physical quantity, a host integrator reading
it will assume a hollow shell centres differently from a solid, and it does not
— they centre identically, because only the box is read. A name that has to be
walked back in its own header comment is a name that will be believed by
whoever does not read the comment. `centre_bounds` promises exactly what it
does, and `clay_layer_bounds` is right there to say what the box is.

**A third reading, and it is NOT this call.** "Centre" can also mean *move the
PIVOT to the content's centre without moving the content* — the layer stays
where it is on screen and the gizmo stops hanging off it. That cannot be done at
layer level at all: keeping the content still while the layer's origin moves
means shifting every item's local placement by the inverse, which is item-level
work on the shared edit list, and on an INSTANCE it would move every other
instance. It is a real gesture and it belongs to the item-level change below.

### `zero_to_origin` clears the translation, not the placement

It sets `xform.position` to `(0, 0, 0)` and leaves the rotation, the uniform
factor and the three per-axis factors exactly as they were. It is "put this
subtool back where it started", not "reset this subtool's transform" — the
reset already exists as one call to
`clay_document_set_layer_transform_nonuniform` with an identity, and a
convenience call that quietly threw away a rotation the artist authored would be
the most expensive undo in the set.

It reads NO bounds, which is what makes it a different call from
`centre_bounds` rather than a special case of it. The two coincide exactly when
the content's box is already centred on the layer's origin, and differ by
however far off-pivot the content was authored — which for a layer built by
brush stamps around a model is usually not zero.

### Which bound, and what the box is NOT

All bounds come from **`clay_layer_bounds`**: the tight world-space extent, the
box a camera frames. Not the influence bound, which is dilated by blend support
and by the chain pad — snapping a dilated box to the floor leaves the model
hovering by exactly that dilation, which is the failure the row exists to
prevent.

The tight box is the honest choice and it is still not the silhouette. Three
things the header must state, all read out of `pick::node_shape_bounds`:

- **A SUBTRACT item contributes its own box.** The walk expands over every
  visible root regardless of op. A layer whose lowest visible node is a
  subtracting box lands that box on the floor, and the material stops higher up.
- **A smooth blend can bulge past both operands.** Rounding IS dilated in
  (`bound.dilated(n.rounding * placed_distance_scale)`); a smooth union's bulge
  is not, so a heavily blended seam near the low face can sit slightly below the
  plane.
- **Hidden ITEMS are excluded.** `pick::layer_bounds` walks `roots` and skips
  `!n->visible`, so the snap follows the silhouette the artist can see, which is
  the right answer — and it means hiding the lowest item moves the layer on the
  next press.

The alternative — march the surface for its true lowest point — costs a bake or
a raycast sweep per press, has no exact answer for a smooth field either, and
would make a menu item the slowest call in a host's transform panel. Rejected;
the limitation is documented instead.

### The radial refusal, which is a defect this change surfaced

`clay_layer_bounds` **does not cover a radial layer's copies.** The influence
path does — `src/scene/bounds.cpp` emits `radial_count - 1` rotated copies for a
participating item — but the pick path this call reads
(`pick::node_shape_bounds`) carries the MIRROR copies and stops there. The
comment on `pick::probe_layer` says the same thing about the sibling probe path
in as many words: "Not the source layer itself, and not its radial symmetry
[...] Carrying the radial copies is a change to what a hit on one names, and
belongs to its own change."

So on a layer with `radial_count > 1` the box describes the un-arrayed item, and
a snap computed from it drops the layer until the ORIGINAL sits on the floor
while its copies are already through it. Three options:

- Answer anyway and document it. Rejected: this is not a loose answer, it is a
  wrong one, and the wrongness is invisible — the layer moves, plausibly, to the
  wrong place.
- Widen `pick::layer_bounds` to emit the radial copies. That is the real fix and
  it is NOT this change: it alters what `clay_layer_bounds` answers for every
  existing host, including camera framing and the mesh-rasterization region that
  a spec scenario already pins, so it needs its own tests and its own note.
- **Refuse.** Taken. `clay_layer_snap_to_ground` and `clay_layer_centre_bounds`
  return `CLAY_ERROR_INVALID_ARGUMENT` on a layer carrying a radial mode, naming
  the mode as the reason. `clay_layer_zero_to_origin` is unaffected because it
  reads no bounds.

The precedent is `clay_layer_lattice_gizmo`, which "returns no warps for a layer
carrying a per-axis scale" rather than placing a cage through a record that
cannot hold its map. Same shape: refuse the case the available bound cannot
describe, say which case, and leave the widening to the change that owns it.

### The refusal set, and why it differs per call

| condition | snap | centre | zero |
|---|---|---|---|
| no layer carries the id | `NOT_FOUND` | `NOT_FOUND` | `NOT_FOUND` |
| layer ghosted or locked | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` |
| layer holds no material | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` | **accepted** |
| layer carries a radial mode | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` | **accepted** |
| `ground_y` not finite | `INVALID_ARGUMENT` | — | — |
| a placement gesture is open | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` |
| box degenerate in one or more axes | **accepted** | **accepted** | **accepted** |

**Empty is refused, not a silent no-op.** A host greying out the menu item wants
to know, and the two states a no-op conflates — "already in place" and "there is
nothing here" — are the two the artist most needs told apart.

**The code is `CLAY_ERROR_INVALID_ARGUMENT`, not `NOT_FOUND`.** `NOT_FOUND` in
this header means "no layer carries this id", and reusing it would leave a host
unable to tell a stale id from an empty layer by the code alone. INVALID_ARGUMENT
is already this ABI's answer for "the layer you named cannot take this
operation": a protected layer takes it, `clay_document_instance_layer` takes it
for a voxel source, and a group node takes it when asked for a primitive.
`CLAY_ERROR_UNSUPPORTED` was considered and does not fit — every use of it here
is about a build, a format or a name the library does not know, never about
document state.

**A degenerate box is ACCEPTED.** All three calls only translate, and nothing
divides by an extent, so a planar layer, a single-cell voxel grid and a
single-vertex mesh all have a well-defined low face and centre. Refusing a flat
layer would refuse exactly the layer a "drop it on the floor" button is most
often pressed on.

### Composition, undo, and the gesture

These **replace the translation** and compose with nothing. Like
`clay_layer_placement_update`, each states a TOTAL placement rather than an
increment; unlike it, the total is computed rather than passed. The rotation and
both scales are carried through bit-identically — read with
`clay_document_layer_transform_nonuniform` (which answers the PRODUCT of the two
scales and never refuses), written with
`clay_document_set_layer_transform_nonuniform`. The single-factor pair is never
touched on the inside, for the reason the proposal gives: the reader refuses a
squashed layer and the setter clears the squash.

**One command, one undo step, one invalidation.** `SetLayerTransformCmd` already
carries the transform and `scale_axes` together — "One command rather than two,
because they are one placement: two would let an undo restore a frame that never
existed" — so nothing new is needed. This is a policy over the existing
vocabulary, exactly as `clay_layer_consolidate` is.

**The command is recorded even when the placement does not move.** Rejected the
alternative (skip when unchanged) because the geometric no-op is not detectable
exactly: a second snap recomputes the box from an already-moved layer and
`f ⊕ (p ⊕ (g ⊖ (f ⊕ p)))` is not `g` in float. Suppressing would need a
tolerance in world units that this ABI would then have to name and defend, and a
host that cannot predict how many undos its own button cost is worse off than
one that pays a refill for a press that did nothing.

That same float chain is why **idempotence is stated to an ulp, not to the
bit.** Applying a snap twice lands within one rounding at the coordinate's
magnitude; a test that asserts bit-equality of the second application will be
flaky at large coordinates, so the spec scenario states a tolerance.

**Refused while a placement gesture is open,** with no special casing: the
gesture already refuses every other edit, including edits to other layers,
because it holds no snapshot to reconcile against.

**The placement change is RIGID** in the sense of `clay_placement_kind` — pure
translation — so the 0.82.0 guarantee applies verbatim: the layer's surface
afterwards is its surface beforehand moved by one matrix, every other layer's
field is bit-identical, and a host may transform its drawn mesh instead of
refilling.

### An instance is placed, never severed

Checked in `clay.h` before deciding, and the header is explicit: what an
instance shares is the edit list; "WHAT IS NOT SHARED is everything else the
layer carries: its transform, its name, its visibility, its protection, its
mirror and its radial mode."

So these calls touch only the named layer's placement. **They do not sever
sharing**, unlike `clay_layer_consolidate`, which severs because a bake replaces
an edit list and there is no reading under which baking one instance should bake
the other nine. There is no such reading here either, in the opposite direction:
placing one instance is precisely the gesture instancing exists for, and a
convenience call that unlinked a subtool because the artist pressed "snap to
floor" would be the worst possible way to discover the link was fragile. The
bound each instance reads is the shared content under ITS OWN placement, so two
instances of one edit list snap to the floor independently and both land.

`clay_layer_consolidation_cost` states the same rule from the other side — "Asking
what a bake would cost must never be the thing that unlinks a subtool" — and
this is that rule applied to a write that has no business severing.

### No delta is returned

The signatures return `clay_result` and nothing else. A host that wants the
translation — to move its drawn mesh under the RIGID guarantee rather than
refill — reads `clay_document_layer_transform_nonuniform` before and after and
subtracts the two positions, which is exact because the change is a pure
translation. Rejected adding an out-parameter to each: three more optional
pointers, three more null checks and three more tests, for a subtraction of two
values the caller can already read. `clay_layer_placement_report` is not the
answer here — it classifies a placement the caller already knows, and the whole
point of these calls is that the caller does not know it yet.

### Layer scope, and what item-level would cost later

Item-level variants are deliberately not in this change, and that is a decision
taken rather than an omission. Layer scope is the scope every other whole-subtool
property already has — visibility, mirror,
radial, protection, the placement itself — so a host learns one scope rather
than two, and the menu item these serve is a subtool menu item.

Adding them later re-lays out nothing: `clay_layer_selection_bounds` already
answers the same box for a subset of nodes, `clay_layer_set_transform_nonuniform`
already writes a node's placement per axis, and `SetTransformCmd` is already one
undo step. An item-level arm is those three composed, on the same rules decided
here, plus one question this change does not have to answer — what a snap means
for a selection spanning a group, whose children's placements are relative to
it.

## Risks

- **A host reads "centre" as "centre of mass".** Mitigated by the name and by
  the header stating the box is the box, but a host that centres a hollow shell
  and a solid and expects different answers will be surprised once.
- **The radial refusal looks like a regression to a host that already ships a
  hand-rolled snap.** Their version answers, wrongly. The refusal names the
  radial mode so the diagnostic explains itself, and the widening is filed.
- **The subtract-box and blend-bulge cases will produce a bug report.** They are
  properties of `clay_layer_bounds`, which is why the header states them beside
  the call rather than leaving the first reporter to find them.

## What building it found

The three claims this file asked the implementation to check, and what happened
to each — plus two the file did not think to ask.

### 1. `position` IS outermost for every representation. Confirmed.

A snap moves the box by exactly the delta on an SDF, a voxel and a mesh layer
alike, each carrying a rotation AND a per-axis scale
(`tests/unit/test_c_convenience_placements.cpp`, "all three representations take
one rule"). No arm needed a case of its own, and no frame conversion appeared
anywhere: the whole write is `position += delta`.

### 2. No existing test asserts `clay_layer_bounds` on a radial layer. Confirmed.

The refusal is exactly as wide as stated, and the gap is exactly where the
reading said it was: `src/scene/bounds.cpp`'s `geometry_bound` emits the
`radial_count - 1` rotated copies, `src/pick/pick.cpp`'s `node_shape_bounds`
carries the mirror copies and stops. Nothing else had to move.

### 3. The idempotence tolerance was stated against the WRONG magnitude. REFUTED.

The scenario said "one rounding at the coordinate's magnitude", and the obvious
reading of that — one ulp at the coordinate the layer ends up at — is wrong, and
wrong by about sixty-fold. Measured: a layer at y = 2048 snapped to y = 0.25
twice moves 7.27e-06 on the second press, against an ulp of 1.19e-07 at the
resulting coordinate.

The reason is that a snap from far away lands the layer NEAR the named plane, so
the result is small while the arithmetic that produced it ran at 2048, where an
ulp is 2.44e-04. The error is set by the magnitude of the box the FIRST press
read, not by where the layer ended up, and the two differ by however far the
layer had to travel. The spec scenario now says so, and the test asserts a third
press moves no further than the second — the property that makes this a bound
rather than a step. The arithmetic did not need restating; the sentence did.

### 4. An UNBOUNDED layer was not in the refusal table, and had to be. NEW.

The table has a row for a DEGENERATE box and none for an infinite one, and the
omission is not harmless. `scene::prim_local_bounds` answers `Aabb::infinite()`
for a plane and for an infinite cylinder, and that box's faces are `±FLT_MAX`
rather than an infinity — so nothing in the arithmetic raises, nothing reports a
domain error, and the placement computed from it overflows to an infinite
position. The layer is then at infinity and its tape evaluates to NaN
everywhere.

Both bounds-reading calls now refuse it with `CLAY_ERROR_INVALID_ARGUMENT`, on
the same footing as the empty layer: the box cannot describe a low face or a
centre. `zero_to_origin` still accepts it, since it reads nothing. The C ABI
already had the vocabulary for this (`box_is_finite`, used by the invalidation
path, and `Aabb::is_infinite`) which is what made it a two-line addition rather
than a redesign — but no row of the table pointed at it, and deleting the check
makes the test place a plane at 3.4e38.

### 5. Composing the PUBLIC per-axis pair would not have been bit-exact. REFUTED.

"Decisions" above says the placement is "read with
`clay_document_layer_transform_nonuniform` [...] written with
`clay_document_set_layer_transform_nonuniform`", carried through
"bit-identically". Reading the two implementations says otherwise, in both
halves:

- The reader hands the rotation back as an AXIS AND AN ANGLE, through a
  normalization and an `atan2`; the setter rebuilds a quaternion from them
  through `Quat::from_axis_angle`. A round trip through that pair is not the
  identity on the bits, and the spec scenario asks for the rotation back "bit
  for bit".
- The per-axis reader answers the PRODUCT of `xform.scale` and `scale_axes` —
  deliberately, so one manipulator never branches — and the setter stores what
  it is given into `scale_axes` with `xform.scale` at 1. The composed map is the
  same, but the layer's own record is not, so a layer that had been placed
  through the uniform setter would come out of a snap with its factor moved.

So the write policy lives one level down instead, in
`scene::translated_layer_command`, which takes the `scene::Layer` and copies the
quaternion, the uniform factor and the per-axis triple across untouched. That is
also what lets pyclay share it — see the next item.

### 6. There IS a change to `src/`, and there had to be. REFUTED.

`proposal.md` and task 1.1 both say "No change to `src/`". That holds for the
BOUND and the COMMAND, which neither moved nor grew — but not for the write
policy, because pyclay does not go through the C ABI. It includes the engine
headers directly and applies `scene::Command`s itself, so a policy stated only
inside `clay_c.cpp` would have been stated twice, in two languages, which is
precisely what task 1.2 says must not happen.

The four functions therefore live in `scene` — `ground_snap_delta`,
`origin_centre_delta`, `origin_translation_delta` and
`translated_layer_command`, in `include/clay/scene/placement.h` and
`src/scene/placement.cpp`. `scene` is the right module by `check_layering.py`:
they need `math` and the command vocabulary and nothing else, and `io` (the only
other module that sees the whole document bundle) may not include `pick`.

What stays at the binding level is which BOX to read, because that genuinely
differs: the C ABI composes the SDF, voxel and mesh arms through
`layer_world_bounds`, while pyclay's `Layer` only ever wraps an SDF layer and
reads `pick::layer_bounds`, the same box its own `bounds` answers. Those are the
same box wherever both are defined.

### 7. A hidden ITEM cannot be exercised across the C ABI. NEW, and minor.

Task 2.6 asks for a case showing that hiding the lowest root moves where the
next snap lands. There is no C entry point for a NODE's visibility — the ABI has
`clay_document_set_layer_visible` and nothing below it — so that case is
asserted in `tests/unit/test_computed_placement.cpp`, against
`pick::layer_bounds` and `scene::translated_layer_command` directly. The
SUBTRACT-item case is pinned in the same file for the same reason: it is a
property of the bound, and the header states it because the first host to meet
it will file a bug.
