# Brushes and features

Every sculpting verb claycore ships, what it does, and how it is parameterised.

The engine has no brush *objects*. What a host calls a brush is one of six
mechanisms, and knowing which one a verb is tells you most of what you need:
whether it is exact, whether it can be undone as an edit, and whether it bakes.

| Mechanism | What it is | Exactness | Undo |
|---|---|---|---|
| **Combine op** | How an item joins the field accumulated before it. Lives on the node; recompiled every evaluation. | Booleans under a hard blend are exact; smooth blends, the extended set and relief are not, and the tape says so | Ordinary edit |
| **Deformer** | A map applied to the point before the item's primitive is evaluated. A chain per node. | Breaks exactness where it is not rigid; the Lipschitz factor is folded into the tape | Ordinary edit |
| **Baked field operation** | Samples a field into a narrow-band `FieldVolume` with the operation applied. | Result is a bound field, not a distance — it declares the Lipschitz it measured | Replaces the item's volume |
| **Resolver** | A pure function turning a gesture into an *ordinary item*. No document is read or touched. | Whatever the item it produces is | Ordinary edit |
| **Voxel verb** | An in-place edit of a palette-indexed grid. | n/a — occupancy, not a field | **Not on the COMMAND stack** — that vocabulary covers document edits only — but recorded as cell runs, so `Document.undo()` reverses a whole verb in one step |
| **Mesh verb** | An in-place displacement of a mesh layer's own vertices. Topology never changes. | n/a — triangles, not a field | **Not on the COMMAND stack**; a sparse `VertexDeltas` record restores the gesture bit-exactly, and `Document.undo()` reaches it like any other step |

Two rules hold throughout and explain most of the API shapes below:

- **No camera enters the engine.** A caller passes the frame, plane, anchor or
  normal it already has, in world units. Picking and cameras stay in the host.
- **Everything a document brush produces is an ordinary edit**, so undo,
  coalescing, serialization and instancing apply without any brush-specific
  machinery. Voxel and mesh verbs still mutate storage beside the document in
  place and `scene::Command` still has no variant for either — but since
  `unify-the-undo-history` that no longer means a host has to snapshot them
  itself. **One `Document.undo()` spans all four representations**: the edit
  list through the command stack, voxel edits as recorded cell runs, mesh edits
  as the sparse `VertexDeltas` the sculptor already produced, and mask edits as
  a diff taken on the step's first touch. A sculpt verb is ONE step however many
  cells it changed, and an edit that changed nothing is not a step at all, so a
  menu built from `undo_depth` never offers an undo that does nothing.

  The mechanism is a `session::History` above scene, voxel and mesh, because
  `tools/check_layering.py` forbids `clay::scene` from seeing the other two —
  which is the same rule that keeps a mask's presence from changing what the
  document evaluates to.

---

## 1. Combine ops

`Node::op`, with `blend.k` carrying the mode's radius, depth or amplitude. The
first four are the classic set; the rest are the extended vocabulary, for which
the blend *profile* is ignored and `k` alone parameterises the mode.

| Op | What it does | `blend.k` | `rounding` |
|---|---|---|---|
| `Add` | Union — the usual "deposit material" | blend radius | rounds the item |
| `Subtract` | Difference — carve the item away | blend radius | rounds the item |
| `Intersect` | Keep only what both cover | blend radius | rounds the item |
| `Paint` | Colour only: the item is a **region**, the surface does not move | blend radius | rounds the item |
| `Groove` | Flat-bottomed channel of depth `k` along the item's surface | depth | channel half-width |
| `Tongue` | Flat-topped ridge of height `k` along it — groove's dual | height | ridge half-width |
| `Pipe` | Solid pipe of radius `k` along the two surfaces' intersection curve | radius | rounds the item |
| `Engrave` | 45° V-groove of depth `k` cut where the item's surface crosses | depth | rounds the item |
| `Emboss` | 45° V-ridge of height `k` raised along it — engrave's dual | height | rounds the item |
| `Inset` | Recessed panel: subtract, with the carve depth clamped to `k` | depth | rounds the item |
| `Shell` | Union with the item's shell, wall half-thickness `k` | half-thickness | rounds the item |
| `Replace` | `(a − b) ∪ b` — the item's field replaces the accumulation inside it | unused | rounds the item |
| `Relief` | The item is a **region**; the surface accumulated before it moves **outward** along its own normal by `k` | amplitude | falloff width |
| `Incise` | The same, **inward** — relief's exact inverse, sharing one kernel branch | amplitude | falloff width |
| `TransitionLinear` | Morph between the accumulation and the item along an axis | — | — |
| `TransitionRadial` | Morph between them radially | — | — |
| `None` | Groups only: children apply inline to the outer chain | — | — |

**`Relief` and `Incise` are the only ops that move the surface without
contributing geometry.** They read the item's field as a weight saying *where*,
then offset the accumulated field there — so the existing surface moves along
its own normal, and the item contributes no shape at all. That is the difference
between "raise the surface here" and "union a sphere onto the face". A relief
item in an empty layer produces nothing.

`Paint` uses its item as a region in the same way, and relief borrowed the trick
from it — but paint moves colour and leaves the field untouched, where relief
moves the field and leaves colour alone.

Three consequences worth knowing before using them:

- The **rounding does double duty**: it is the falloff width *and* it rounds the
  region's own field, exactly as it does for groove and tongue. So the reach is
  region + rounding + falloff, and the influence bound is dilated by both.
- **Amplitude ÷ falloff width is one number seen twice.** It is what turns the
  rim into a ledge rather than a swell, and it is exactly the slope the op adds
  — so a picture that looks harsh and a step scale that dropped are the same
  fact. See [`examples/25_relief.py`](../examples/25_relief.py).
- **`k` buys displacement only up to the region's extent.** A surface point
  moves only while it stays inside the rounded region, so the displacement
  equals `k` exactly until `k` reaches how far the region extends past the
  surface along the normal — for a sphere stamp centered on the surface, its
  radius plus the rounding — then saturates, never reaching radius +
  2·rounding. With rounding 0 the ceiling is the bare radius: leaving rounding
  unset costs amplitude as well as edge softness, and a depth slider mapped to
  `k` goes dead past the extent. The standard clay mapping is
  **`k` = rounding = stamp radius**: the stamp raises the surface by exactly
  `k` with a soft rim, reaching no further than 2·rounding outside the item.
  `tests/unit/test_relief.cpp` pins these numbers.

`TransitionLinear`/`TransitionRadial` are **non-local**: their weight is
non-zero arbitrarily far from both operands, so those items report infinite
influence and are never culled. Every other op has finite support.

### Blend profiles

`Node::blend.profile`, with `blend.k` as the radius. Ignored by the extended
ops above, which use `k` for their own quantity.

| Profile | Character |
|---|---|
| `Hard` | Plain min/max — a crease |
| `Quadratic` | The usual smooth-min; cheapest, slight bulge |
| `Cubic` | Wider, softer shoulder |
| `Circular` | Constant-radius fillet |
| `Chamfer` | 45° flat bevel rather than a fillet |

### Symmetry: the layer mirror

`Layer::mirror_axes` reflects the layer's items through the planes where the
layer-local coordinate is 0, one plane per enabled axis, and `mirror_k` is the
**Mirror Blend** seam — 0 is a hard crease on the plane, a positive value
smooth-welds an item to its own reflection where it crosses it. It is applied
at **evaluation**, not authoring: one node exists, both sides render, and the
left arm cannot drift from the right. Setting it before or after the items
were added is the same document, and clearing the axes restores the
unmirrored field.

**Every item participates by default** — a sculptor who turns symmetry on
means the layer. `Node::mirror = false` (C: `mirror = -1` on the descriptor or
`clay_item_set_mirror`; Python: `mirror=False`) keeps an item out — an
asymmetric detail, a scar on one cheek. Strokes are items, so a stroke on a
mirrored layer stamps both sides, and a subtract or relief mirrors the carve
the same as an add mirrors the deposit. Until 0.27.3 this flag was an
**opt-in that defaulted to excluded**, which read as the layer mirror doing
nothing at all (#60). Layers with no mirror axes cost nothing either way.

For symmetry at **authoring** time instead — build one arm, get the node list
of two — see `add_child` mirrored under Armatures (§6).

### Symmetry: the layer radial array

`Layer::radial_count` arrays every participating item `count` times about the
layer-local axis `Layer::radial_axis`, with `Layer::radial_k` smoothing the seam
between neighbouring copies exactly as `mirror_k` smooths the mirror seam. 0 or
1 is off and costs nothing. `clay_set_layer_radial`, `Layer.radial(count,
axis="y", blend=0.0)`.

**Participation is the mirror's flag, not a second one.** An item added with
`mirror=False` is excluded from the layer's symmetry — both modes — because an
asymmetric detail is asymmetric once. Strokes are items, so a stroke on a radial
layer arrays without the caller touching the nodes it resolved into, which is
the property that makes this a sculpting mode.

**Radial and mirror compose additively.** Each contributes its own copies of the
base item and the products are not emitted: a 3-fold radial plus an X mirror
gives 3 rotations and 1 reflection, not the 6 of a combined group. That matches
the mirror's own convention across its axes — x|y emits two reflections, not
four quadrants.

**This is the MODE. `Repeat::radial` is the MODIFIER**, and the difference is
not stylistic:

| | `Layer::radial_count` | `Repeat::radial` |
|---|---|---|
| set on | the layer | one item |
| reaches a stroke | **yes** — stamps are items on the layer | no — the caller would set it per resolved node |
| per-item opt-out | yes, the mirror flag | n/a |
| seam blend | yes | no |
| cost | `count` emitted instances per item | **O(2)** per evaluation, whatever the count |

So a 6-fold sculpting symmetry is the mode; a 24-fold decorative array is the
modifier. The mode has to emit real copies because the cull, the bounds, the
seam blend and the opt-out all need real items to act on — which is exactly what
buys it the properties the modifier cannot have.

---

## 2. Deformers

`Node::deformers`, applied as a chain to the point before the primitive is
evaluated. Order matters and is part of the edit. The brush-like ones — those
with a centre and a finite radius — are marked ●.

| Deformer | What it does | Parameters |
|---|---|---|
| ● `grab` | Translate a region, weighted from the centre out, zero past the radius | centre, radius, displacement, ease, `front_only` |
| ● `pose` | Rotate a region about a centre, weighted the same way | centre, radius, axis, angle, ease |
| `pose_line` | Rotate about the axis through `a`, ramping from nothing at `a` to full at `b` and beyond. Does not stop at a radius | a, b, axis, angle, ease |
| ● `magnify` | Radial scale about a centre. **Positive swells, negative gathers** — magnify and pinch are one deformation with a signed strength | centre, radius, strength, ease |
| `noise` | Fractal gradient noise offsetting the distance — the irregular sibling of `displace` | amplitude, frequency, octaves, gain, seed |
| `displace` | Sine displacement — regular by construction, an even corrugation | amplitude, frequency |
| `twist` | Rotate about Y, radians per unit | radians/unit |
| `bend` | Bend along X, radians per unit | radians/unit |
| `twist_range` | Twist RAMPED across `[y0, y1]` and **held** beyond it. A gizmo's twist acts inside its box; `twist` winds the whole item | radians/unit, y0, y1, ease |
| `bend_range` | The same for bend, across `[x0, x1]` — ZBrush's Bend Arc is angle-limited this way | radians/unit, x0, x1, ease |
| `bend_curve` | Bend along a DRAWN guide: the local X span is laid onto the guide's arc length and the material rides its parallel-transported frames. Every bend a constant rate can express is a circular arc; this is the one that is not | guide points, t0, t1, point type |
| `bend_linear` | Bend a segment about a direction, eased | a, b, v, ease |
| `bend_radial` | Bend between two radii over a depth, eased | r0, r1, dz, ease |
| `taper` | Scale between two heights, eased | y0, y1, s0, s1, ease |
| `wrap_around` | Bend the span `[x0, x1]` about the Z axis | x0, x1 |
| `elongate` | Insert per-axis extent, exact about the origin | half-extents |
| `elongate_axis` | The same without the correction — works for any primitive | half-extents |

`grab`, `pose` and `magnify` have **finite support**, so they are culled and
bounded like any other item. `front_only` on `grab` gates the pull on the
half-space it heads into, so the far side of a form does not travel with the
near side.

### Grab, pose and magnify are per item, and local

This is the one thing to know before wiring a Move brush to `grab`, and it is
easy to miss because nothing errors.

A deformer is emitted into the tape **per item** and applied to that item's
**local** point. So `grab` drags one item's own field, not the accumulated
surface, and its `centre` is in the item's frame — a centre of `(0, 0, 0)` grabs
the middle of a sphere sitting at world `x = 1.5`, and a centre of `(1.5, 0, 0)`
does nothing to it.

ZBrush's Move drags the **surface**. The two coincide only while the region
under the cursor belongs to a single item. On a form smooth-unioned from
several — the normal case for a blocked-out sculpt — grabbing one item pulls its
share and leaves the rest behind. Measured on two balls of radius 0.5 at
x = ±0.45 smooth-unioned at k = 0.25, with a grab of radius 0.8 and displacement
0.4 on the left item alone: **the left side rises 0.070 and the right 0.000**.

The fix is to apply the **same warp to every contributing item**, mapping the
world drag into each item's frame:

```
local_centre = inverse(item.xform) * world_centre
local_disp   = inverse(item.xform.rotation) * world_disp / item.xform.scale
local_radius = world_radius / item.xform.scale
```

That reconstructs a true field-level grab **exactly**, for two reasons worth
stating: combine ops are pointwise in the deformed point, so warping every
operand identically is the same as warping their combination; and `Transform`'s
scale is uniform by design, so a spherical falloff stays spherical under it.
Measured on two blended balls with a world drag centred between them, the lift
is symmetric and peaks at the world centre.

`brush::move_brush` owns that mapping — the resolver the cut tool and snakehook
are. It takes a world centre, radius and displacement, and returns one `grab`
per contributing item, each already in that item's frame. Items the drag cannot
reach get nothing, since a warp outside its own support is a no-op that still
costs a tape record.

**A third thing it owns: the warp goes at the FRONT of the chain.** Deformers
apply in authoring order, so `deformers[0]` is the outermost warp on the
geometry. One appended at the back has its region weight read at a point the
earlier deformers already moved, so the drag acts somewhere other than where it
was aimed — invisible until an item has two deformers.

There is also nothing to accumulate. `compile_group` passes the layer through
and `emit_item` uses `layer.xform * item.xform`, so **a group's own transform
never reaches its children**: a sphere under a group translated to `x = 2`
evaluates at the origin. Worth knowing in its own right — which is why the
bindings now refuse a transform on a group rather than record an undoable,
saved edit that changes nothing (`clay_layer_set_transform`, `Layer.set_transform`).

**A drag coalesces.** A Move is not one call — a host re-applies it every frame
with a longer displacement. A drag holds its centre and radius fixed and grows
only its displacement, so those two identify the gesture and `moved_chain`
*replaces* that gesture's warp rather than stacking another in front of it.
Without this a two-second drag at 60fps leaves 120 warps on every item it
touched, each one multiplying into the declared Lipschitz. A different centre is
a different gesture and is kept beside the first.

**Under symmetry, the brush is reflected — not the item's bound.** The
compiler emits a mirrored item as itself plus one copy per axis, and a copy's
field at `p` is the item's whole record, deformer chain included, at the
reflected point. So a grab in an item's chain moves the item near the ball *and*
its copy near the ball's reflection — and an item whose **copy** sits under the
ball has its body at the reflection, where a grab centred on the ball weighs
zero: measured on a ball whose copy sat under a drag, the copy's pole moved by
0.00000 while everything else moved by -0.05945. Selecting items on the
mirror-expanded influence bound made that the common case (a grab on a ridge at
x 1.45 took 46 items where the unmirrored drag took 22, the base among them,
each a warp that did nothing) and left a drag and its mirror image asymmetric.

The drag is therefore stated as the **images** the layer's symmetry makes of it
— the ball, one reflection per mirror axis, one rotation per radial copy;
additive, exactly the copies `emit_item` emits — and every item is tested on
its **own** bound against each image. An image that reaches an item gives it a
grab at that image's centre with that image's displacement, so the reflected
ball grabs the items whose reflections sit under the ball. Items that opted out
of the mirror (`mirror=False`, `-1` in C) and feathered volume replaces see the
ball alone, because the compiler emits no copy of them. An item both images
reach — one straddling the plane — takes both grabs in one warp, **composing
the two pulls as two brushes would**: a drag centred on the plane pulling along
it lifts a ball of radius .4 by 0.1645 against 0.0950 under one grab (1.73x),
continuously as the centre leaves the plane (0.1650 at x .01, 0.1600 at .05,
0.1460 at .1); pulling across the plane the two cancel to a pinch. This is the
mesh-sculpt default; an overlap reducer would be a kernel-level opt-in. With
an identity layer transform a drag and its mirror image produce the same field
**bit for bit**, and a mirrored drag on late-history items states its own
frontier and resumes where it used to take the base's ordinal 0 and drop.
The gesture's invalidation covers every image's ball, as separate boxes.

`move_brush` is pure, so a drag can be **previewed** — `Layer.move_surface_preview`
and `clay_layer_move_surface_preview` return the nodes it would warp without
touching the document.

Applying the result needs `SetDeformersCmd`, which is new too: the command
vocabulary could not change a node's deformers at all, so a deformer could only
be set when its node was created.

**Magnify has the same resolver, and Pinch is it with a negative strength.**
`magnify` is per item and local for exactly grab's reason, and until 0.68.0 it
had no counterpart to `move_brush` — so Pinch could not be a surface brush on a
field at all, and a host offering it had to either scale one picked item of a
blended form (the failure this whole section exists to prevent) or rebuild the
resolver itself. `brush::magnify_brush`, `Layer.magnify_surface` and
`clay_layer_magnify_surface` close that, with `move_surface`'s contract: every
item the region reaches, mapped into each frame, one undo step and one
invalidation for the whole gesture, symmetry images handled, and a `_preview`
counterpart.

It resolves by *less* than a drag does. A drag's displacement is a vector, so
each symmetry image has to reflect or rotate it; a magnify's strength is a
dimensionless factor and a reflection of a radial scale is a radial scale of
equal strength, so the strength crosses every image untouched. And the region a
host must invalidate is the ball itself with no dilation — outside the radius
the weight is zero and the point is returned unchanged, for either sign.

`pose` is in this section's title and does **not** have a resolver. It does not
fit the shape: radial pose carries an axis, a direction that would have to be
mapped per image, and pose along a line has no finite support at all, so the
reachability rule both resolvers are built on does not apply to it.

**Disjoint brushes do not compound the step scale.** A chain's Lipschitz used
to be one running product, so eight drags 3.06 apart on a radius-4 sphere were
charged `k^8` — a step scale of 0.0616 against 0.7059 for one drag — for a
compounding that cannot physically happen. `grab`, `magnify`, `pose`, `blob` and
`alpha` are the finite-support family: outside their own ball the field is
untouched, so two whose balls cannot reach one another have no point at which
both are anything but the identity. The bound is now the worst *group* of links
that can reach each other rather than the product of all of them, and the eight
disjoint drags cost exactly what one costs.

The gap that separates two groups is `r_i + r_k` plus the total distance the
chain's point warps can carry a point, not the radii alone: the first grab moves
material toward the second, so balls that do not overlap can still meet.
Deformers that act everywhere — twist, bend, taper, a lattice, and `pose_line`,
whose weight clamps rather than falling to zero — are charged against every
group, so nothing is saved by mixing one in. Drags piled on one spot still
compound, because there they genuinely do. `consolidate` remains the answer for
a chain that has really degraded; this only stops charging for a degradation
that is not there.

`magnify`'s **centre is its fixed point** — a radial scale about a point on the
surface bulges the neighbourhood *around* it and leaves the point itself exactly
where it was. This surprises people (and caught the tests first); see
[`examples/23_magnify.py`](../examples/23_magnify.py).

`noise`'s hash is **integer**, not the usual `fract(sin(...))`. A float hash puts
a `sin` inside a chaotic amplifier and a 1e-7 backend disagreement becomes an
O(1) one, which would fail cross-backend parity on the first case. The seed is
an ordinary parameter, so two items with the same seed look the same and an
item's appearance never depends on compile order.

---

## 3. Baked field operations

These **bake**: they sample into a `FieldVolume` with the operation applied and
replace the item's volume. That is stated everywhere a caller looks, because it
is the one place in the library where an edit is not re-derivable from its
parameters.

| Operation | What it does | Key parameters |
|---|---|---|
| `field::relax` | Smooth the field — the ZBrush Smooth brush. Averages over a cell neighbourhood | strength, `radius_cells`, iterations, centre, `region_radius`, falloff, mask |
| `field::move_topological` | ZBrush's Move Topological: a drag weighted by distance ALONG THE MATERIAL, so a part close in space but far along the surface is not dragged | anchor, geodesic radius, displacement, ease |
| `field::flatten` | Pull the surface onto a plane. `mode` picks which side it acts on: two-sided (ZBrush Flatten), cut-only (hPolish, Planar, Trim) or fill-only | plane point + normal, strength, centre, `region_radius`, falloff, `mode`, mask |
| `brush::mask_extrude` | Pull a masked patch of a surface off as a solid — ZBrush's Extract | thickness, side, threshold, `border_round`, `border_smooth`, cell size, band |

Relax and flatten both take a region with a falloff. A `region_radius` of zero
means:

- for **relax**, "everywhere" — which is a filter rather than a brush;
- for **flatten**, *refused*. Flatten is local by nature: where its weight is one
  the result **is** the plane, so with no region it replaces the shape with a
  half-space — a ball comes back as a box.

**A relax with a region costs what the region contains.** Until v0.50.0 it did
not: `relax` swept every stored sample in the volume so its callback could
discover, per sample, that the weight there was zero. At a 0.01 cell a
five-cell brush cost 16.7 ms and about 0.6 ms of that was work inside the
brush. It now rewrites only the bricks the region meets — the region being the
radius plus the **taper**, since a falloff narrower than the kernel is widened
before it is used.

| cell 0.01, `radius_cells` 1, one pass | before | after | |
|---|---:|---:|---|
| brush radius 0.05 | 16.7 ms | **2.2 ms** | 7.5× |
| brush radius 0.20 | 18.6 ms | 5.9 ms | 3.2× |
| brush radius 0.40 | 23.5 ms | 13.7 ms | 1.7× |
| `region_radius` 0 (a filter, not a brush) | 71.5 ms | 71.5 ms | unchanged |

The gain follows how local the brush is, which is the point: cost tracks the
dab rather than the document. A `region_radius` of zero means everywhere and
still sweeps everything, correctly.

The far-bound rebuild `shrink_band` does is no longer one of them. It used to
run on every dab; it now runs only when the band actually narrows, which is the
FIRST dab of a stroke and no other — a bake starts a couple of cells above the
floor, and what a sample-free brick reports depends on the stored-brick set,
the grid and the band, none of which a later dab moves. Over a 24-dab stroke at
a 0.01 cell the steady dab fell from 2.32 ms to **1.77 ms**, and the first dab
did not move, correctly.

The per-pass volume copy is gone too. A stencil needs the pass's *input*, not
its half-written output, and the obvious way to get that was to copy the whole
volume — six megabytes at a 0.01 cell, to protect the few hundred kilobytes a
brush touches. It now copies only the bricks the pass will overwrite, and reads
outside them from the volume itself, which still holds what it held because
those bricks are never written.

### A dab that costs the dab still is not a live brush

All of the above makes one relax cheap. It does not make Smooth **online**,
and that gap is worth naming precisely because the numbers above look like it
should have.

`relax` takes a volume and returns a volume. A layer is an edit list, so a host
holding a layer and a brush has to bake the layer to get the volume — and if
it has nowhere to keep that volume between pointer events, it bakes again on
the next one. The per-dab cost is then *bake + relax*, and the bake is
`sdf_consolidate`: 313 ms on the reference iPad. The only affordable shape for
that is one bake at pointer-up, which is why Smooth showed the artist nothing
until they lifted the pen, however local the dab underneath had become.

The missing piece was never the kernel. It was a **place to keep the working
volume for the length of a gesture**, and that is what `session/sdf_sculpt.h`
adds — see §3.1 below.

### 3.1 Field brushes as transactions

Two brushes are not spelled as edit-list stamps and cannot be: relax bakes
(there is no node meaning "the average of what was here"), and Move is a warp
on every item it reaches. Both are now driven as a **transaction** —
`clay::session::SdfSmoothTransaction` and `SdfMoveTransaction`:

```
begin    capture the source; build the transient state ONCE
update   mutate only that state; receive the dirty region
commit   one persistent command group, one undo step
cancel   nothing persistent ever happened
```

Between `begin` and `commit` the document is untouched — no nodes, no
deformers, no undo entries, and a save taken mid-gesture is byte-for-byte the
one taken before it. That negative is the whole architecture, and it is the
first thing the tests assert.

| | before | with a transaction |
|---|---|---|
| Smooth, per dab | bake the layer, relax, discard | relax the retained working volume in place |
| Smooth, layer evaluations per stroke | one per dab | **one, at pointer-down** |
| Smooth, live preview | none — the result appears at pointer-up | the working volume, with dirty bounds per dab |
| Move, per pointer event | `move_brush` walks the whole edit list, then one `SetDeformersCmd` per item | one `resolve_prepared_move` per *affected* item; nothing persistent |
| Move, undo entries per drag | one per frame, coalesced by `moved_chain` | one, at pointer-up |

Each `update` returns a `SdfSculptDirty`: a conservative world-space box, the
number of bricks the brush selected, and whether anything actually moved. The
box and the count are **geometric** — they describe what the brush selected,
not which samples happened to change — so they are reproducible for a given
brush over a given lattice however much unrelated model surrounds it. `changed`
is the value question, kept separate because a dab whose weight came out zero
everywhere still selects its bricks and has nothing to redraw.

**Move updates take the TOTAL displacement from the anchor**, never an
increment on the last frame. Updates of 0.1, 0.2 then 0.5 end at exactly what a
single fresh drag of 0.5 produces; a composition of the three would move the
surface further than 0.5 ever asked for. The preview is rebuilt each frame from
the chains captured at `begin` plus the grabs for the current total — one per
image of the drag that reaches the item, so under a layer mirror the copy under
the ball moves in the preview exactly as it does in the commit — which is also
what the commit installs, so a preview and its commit cannot disagree. The
prepared half of the drag (`brush::PreparedMove`) carries those images: where
each lands in the item's frame and whether it reaches, decided at pointer-down
with the rest of the reach; the per-frame half maps the displacement through
each image and touches no scene state.

**A commit refuses a source that moved underneath it.** The layer is
fingerprinted at `begin` and re-checked at `commit`; if another tool, a
replayed journal or an undo has edited it, the commit fails and the external
edit stands. A preview computed against a document that no longer exists is
never written over one that does.

### 3.1.1 Where the working field comes from

A transaction has to start somewhere, and the first version of this sampled the
whole finite layer at pointer-down. That was the honest first answer — a local
patch needs a rule for what it means where it meets the field it was cut from,
and that rule is real correctness surface — but it moved a cost rather than
removing one: `begin()` measured within 1% of a whole `bake_layer`.

It is now lazy. `begin()` compiles the layer, allocates an index for the working
lattice and takes a digest; it **evaluates nothing**. Each dab materializes the
bricks its relax will read, and no others:

```
begin        a lattice, and no samples at all
update A     materialize A's dependency region, relax it
update B     reuse what A brought in, materialize only what B newly reaches
commit       assemble the layer once, overlay what the dabs changed
```

Two things make that safe rather than merely fast.

**A brick that stores samples is exactly a brick that has been materialized.**
`kBrickEmpty` already means something specific — "no surface here, and here is
which side" — and consumers are entitled to believe it. Rather than overload it
with a second meaning, materialization *force-stores* every brick it fills, even
one whose samples are all past the band. Stored-ness is then an honest record of
what has been filled in, at the cost of a brick that says nothing interesting.

**The dependency halo is derived from Relax's own stencil, not guessed.** It is
the ball Relax rewrites, plus a brick *diagonal* — `rewrite_region` writes whole
bricks that merely touch the ball, so a written sample can sit a diagonal beyond
it — plus the stencil's reach. Getting this short does not read a wrong number:
`sample_at` returns *nothing* for an unmaterialized brick and Relax renormalizes
over the taps that exist, so the sample comes out smoothed against a smaller
neighbourhood. A seam at a brick face, invisible except as a measurement.

**And one semantic change, which is real.** The whole-layer path relaxed a
*redistanced* bake. The lazy path redistances a *relaxed* field, because a local
working field cannot start from a globally post-processed one — redistancing is
not something you can do to one brick. Both are sound signed distance fields and
neither approximates the other. Measured on the surface they differ by **0.0037,
about 0.073 of a cell**, and the tests record that distance rather than claiming
the two match.

### 3.1.2 Not paying for history that has not changed

A dab over worked geometry still has to evaluate whatever contributes there, and
almost none of it changed since the last dab. Measured (#306) at a 0.05 voxel,
one dab into 12 bricks: 0.23 ms at 200 items, **18.07 ms at 50,000**.
Consolidating the layer shows the floor — but a bake discards the parameters of
everything it absorbs, so the cure costs the artist their history.

`session::SdfPrefixCache` is the same split without the loss. An old, stable
prefix of the root list is sampled into a volume; the live suffix is evaluated
over it; the document is untouched and every item stays editable.

```
roots [0, K)              roots [K, N)
cached FieldVolume    +   live suffix     ==  the layer's field
```

It can be exact because the tape compiler already emits a layer's chain as a
**fold at item boundaries**: after every root the stack holds one value.
`compile_layer_prefix` and `compile_layer_suffix` name that boundary, and
`eval::eval_points_seeded` continues from it. Prefix-tape-then-seeded-suffix is
**bit-identical** to compiling the whole document — zero difference over 20,000
random points, not a tolerance.

What needs care is when the cached *volume* may stand in for that seed, and the
answer is sharper than it looks:

| where the prefix volume… | error in the composed field |
|---|---:|
| **stores** the samples | 3e-7 — float rounding |
| stores **nothing** | 0.27 — about **14 cells** |

Outside its stored bricks a volume answers with a conservative *far bound*, not
the distance the history had, and a blend folded onto that is silently wrong by
cells. So the volume seeds a window only where it stores every sample of it, and
anywhere else the prefix tape is evaluated instead — correct either way, only the
cost differs. `fallback_windows` counts how often the slow answer was needed,
because a cache with a high fallback rate is one that is not working rather than
one that is wrong.

Two further things had to be right, and both were measured wrong first: the
prefix is baked **without redistance** (which rewrites every sample: 0.063, two
cells) and over the **whole layer's region** rather than its own (the lattice
origin is `region.min`, so a different region is a different lattice: 0.0074).

The result is a **sampling** source, and worth reading as one: exact on the
lattice it was built for, ordinary interpolation error between lattice points
(7.6e-3 at a 0.03 cell — the fidelity a consolidation of the same prefix would
have), and exact outside the stored region. A consumer that needs the true field
at arbitrary points wants the walk.

The cache is derived state. It is never serialized, never a node, never visible
to undo, and **deleting every entry must be equivalent to flushing a CPU cache**:
slower, and identical output.

### 3.2 Bounding a long session

Both degradation mechanisms `scene::report_layer` measures — a lengthening
deformer chain, a steepening resampled volume — decay the marcher's safe step
with every completed stroke. Consolidation cures both and destroys the
parameters it absorbs, which is why `report_layer` measures and never bakes.

`session::SdfSculptComplexityPolicy` is the other half: the place a *session*
says when that trade is acceptable.

```cpp
session::SdfSculptPolicy policy;
policy.cell_size = 0.01f;                       // required; the host's own
policy.complexity.min_safe_step_scale = 0.25f;  // 0 disables a criterion
policy.complexity.max_deformer_chain  = 8;
policy.complexity.allow_consolidation = true;   // the whole opt-in
```

Zero disables a criterion, so a value-initialised policy authorises nothing and
is never over budget. The check runs **only between completed strokes** — never
while the pointer is down — and over budget without `allow_consolidation` is a
*report*: `budget().over_budget` is set and the document is untouched. With it,
the collapse happens inside the stroke's own undo step, so one undo puts back
both the stroke and the layer it consolidated. A layer that is already a single
volume item is not baked again.

| 24-dab stroke, cell 0.01 | before | after |
|---|---:|---:|
| first dab | 2.96 ms | **2.31 ms** |
| steady dab | 1.81 ms | **1.60 ms** |

At a 0.02 cell the two are level: the copy was already small there. The gain is
not only the copy — a snapshot of one brush's worth of bricks is a few hundred
kilobytes and fits in cache, where a tap against the whole volume walks a sparse
index into six megabytes.

**And it stopped paying for the samples it cannot reach.** A brush is a ball;
the bricks it selects are a box around one. Measured, 51–73% of those bricks
could not hold a reachable sample, and 62–95% of the samples visited came back
with a weight of zero — each having paid a lookup, a `cell_position` and a
square root to say so. Three things fixed that: the selection narrows to the
ball, the weight compares squared distances so only the taper takes a square
root, and the base value comes from the sample the rewrite already handed over
rather than from a lookup that would return the same number.

| 24-dab stroke | before | after | |
|---|---:|---:|---|
| steady dab, cell 0.01 | 1.61 ms | **1.00 ms** | 1.61× |
| steady dab, cell 0.02 | 0.61 ms | **0.23 ms** | 2.71× |

**A dab now costs what it moves, with nothing left in it that scales with the
model.**

Smoothing destroys **exactness** but cannot break the **Lipschitz** bound: an
average cannot vary faster than the thing it averages, and a 1-Lipschitz field
is automatically a conservative bound on the distance to its own zero set, so
the raymarcher stays correct. Flatten's region blends under a weight that varies
across it, which *can* be steeper than the source — so it measures the Lipschitz
its samples actually have rather than assuming one, and the document's safe step
scale drops to match.

**Which side it acts on is the whole hard-surface family.** Two-sided is ZBrush's
Flatten: material above the plane goes *and* hollows below it fill. Cut-only is
hPolish, Planar and the Trim brushes — cutting *without* filling is what leaves a
crisp facet against untouched surface, and filling the hollows beside a facet is
what a polish must not do. Fill-only is the dual, and closes a scanned hole flat
without touching the surface around it. The three differ by one clamp on the
blend term, which is why it is a mode rather than three entry points.

Measured across a flank carrying a hollow, with the plane at x = 0.5:

| y | source | two-sided | cut | fill |
|---|---|---|---|---|
| −0.10 | 0.576 | 0.498 | **0.498** cut | 0.576 kept |
| 0.00 | 0.375 (hollow) | 0.498 | **0.375** kept | 0.498 filled |
| 0.30 | 0.513 | 0.498 | **0.498** cut | 0.513 kept |

**hPolish chains only if you consolidate between passes.** A flatten bakes, and
sampling the *document* gives an exact source and a 1-Lipschitz result. Chaining
a second pass samples the first pass's *volume*, where outside the band a volume
reports a lower bound rather than a distance — so the blend works from the wrong
value. The declared Lipschitz goes 1.00 → 14.0 on the second pass whatever the
falloff, and by the third the form is visibly corrupt rather than merely
expensive. [`examples/28_hpolish.py`](../examples/28_hpolish.py) measures that.

`Layer.consolidate` closes the loop: it collapses the layer into one volume,
**redistances** it — replaces the samples with the distance to their own zero
set — and hands back a source that is a distance field again, so the next pass
starts where the first one did. Six passes hold the declared Lipschitz at
√3 instead of reaching 32, and the memory does not creep.
[`examples/38_consolidation.py`](../examples/38_consolidation.py) measures both
halves. Baking WITHOUT redistancing does not do it: steepness is a property of
the field, and resampling reproduces it.

The other route is the cut tool, where an Intersect against a prism is exact and
stays exact — cheaper than a bake when the facet is a plane through the whole
form rather than a region with a falloff.

`flatten` has two overloads. Prefer the one taking a **document sampler**: a
volume's band tracks the surface only while the surface stays inside it, and
flatten moves it many band widths, so flattening a volume in place is accurate
only near the band it came from. The volume overload exists for imported meshes,
where there is no document behind the surface.

**A flatten on a volume costs what the brush touches.** Until v0.52.0 it did
not: the overload resampled the whole of `v.bounds()` for a brush that reaches
a ball, so the dab cost what the model cost. It now resamples only the bricks
that ball meets — the radius plus the **taper**, exactly as relax measures its
region — and re-decides which of them store samples, which is the part a rewrite
could not do: flatten moves the surface many band widths, and the facet lands in
bricks that held nothing.

| a five-cell dab, one ball at the origin plus N-1 far away | before | after | |
|---|---:|---:|---|
| cell 0.02, 1 ball | 40.8 ms | **0.8 ms** | 53× |
| cell 0.02, 8 balls | 266.9 ms | 5.9 ms | 45× |
| cell 0.015, 8 balls | 628.9 ms | 12.0 ms | 52× |
| cell 0.01, 1 ball | 262.7 ms | **3.5 ms** | 75× |
| cell 0.01, 8 balls | 1800.3 ms | 28.7 ms | 63× |

The field evaluation is what became local: the same dab asks for the same 36
bricks whatever surrounds them, and a test holds that as a count rather than a
time. What still scales with the model is the compact rebuild of `data_` and the
far-bounds chamfer — at cell 0.01 with eight balls, 11.2 ms of the 28.7 is the
volume copy the overload returns and most of the rest is those two.

It also stopped **inflating the volume it flattened**. The overload read its
source back through `eval()`, which outside the band reports the far bound — a
value that steps by brick rather than by cell — so re-recording those steps as
samples took a re-baked ball from 444 stored bricks to 1,246 and declared a
Lipschitz of **14** where its source declared 1, compounding on every stroke. It
now reads the volume's own stored sample and falls back to the bound only where
no brick holds one, floored at what an empty brick already guarantees. The same
ball flattened is 444 bricks at a Lipschitz of 1.7, and 444 at 2.4 after eight
dabs. See [#300](https://github.com/CyberdyneCorp/ClayCore/issues/300).

`relax` has the same pair, mirroring flatten symbol for symbol
(`clay_item_volume_relax_from` beside `clay_item_volume_flatten_from`). For
relax the document-sourced form is exactly bake-then-relax fused into one call
— relax averages cell-aligned taps, and a fresh bake's taps *are* the document
at those lattice points — so inside the band the two are identical, and a test
holds that. What it buys is one entry point, and a source that can never be a
volume derived from another volume.

**The document-sourced forms bake through the CPU backend's pool.** Until
v0.50.0 they did not: `clay_item_volume_from_document` and the `_relax_from` /
`_flatten_from` pair handed the sampler a callable and asked the tape for one
point at a time, while `Layer.consolidate` had gone through the batched window
fill since it was written. A tape instruction costs about ten nanoseconds and
its arithmetic costs one, so the interpreter was most of a bake and the
interpreter was per point. Measured on a twelve-core machine, same document,
byte-identical output:

| | 193-node layer at cell 0.05 |
|---|---:|
| batched block fill | **23.6 ms** |
| the per-point walk it replaced | 386 ms |

Nothing in the ABI changed, and a host that already called these gets the
16.4x without doing anything. Note the figure is the bake **alone** — the
`sdf_consolidate` device case pays a serial redistance on top and moves less.

**And the tape is culled per brick, where that is worth doing.** A brick needs
only the items whose influence reaches it — a 600-dab sphere hard-unioned
compiles to 1,199 instructions and any one brick needs 5.4 of them — which is
how the brick cache has always evaluated and how the bake now does too. On a
0.02 cell:

| blend `k` | brick tape / whole tape | bake |
|---|---:|---:|
| 0 (hard union) | 0.5% | **3.4×** |
| 0.04 | 7% | **2.2×** |
| 0.06 | 16% | 1.5× |
| 0.10 | 49% | *refused* |

The last row is the point of the table. A smooth union's cull pad grows with
`k`, so a wide enough blend keeps every item in every brick's tape and the
per-brick compile becomes pure overhead — measured at 0.55× before the guard
existed. That slope is the whole of the frame-path cost #335 reported: the pad
was `4k` for a quadratic profile against a region that is a fixed brick plus
band, so doubling `k` from 0.03 to 0.06 cost 1.87× on a refill — and the cull
benchmarks all blended at 0.03, which is why the fixtures could not show it.
`BM_DeepDocCullPlanned2000K06` is the same document at the other radius, gated
against it as a ratio. **The pad now grows with the chain instead of standing
at the support** (#335): `min(support, k · envelope(N))` per profile, where the
envelope rises with `log2` of the layer's effective contributor count — every
node times its mirror and radial copies, since each copy is a leaf the tape
folds through its own seam blend — from about `2.8k` at 75 contributors to the
full support past ~800 for quadratic (~390 circular, ~6,800 cubic), and the
layer's seam `k` enters as its own quadratic term capped at the pad the item
maxima alone would set. Sweeps to 8,000 contributors over three profiles,
adversarial orders and symmetric layers found the sufficient pad creeping
about `0.4k` per doubling of length, and the fit clears every measured knee
by at least `0.5k`; where the clamp binds the tapes are instruction-identical
to the old pad's, so no document is culled wider than before. What it buys
back sits at real stroke lengths — 12–15% of surviving instructions on a
quadratic chain of a few hundred nodes, 17–24% cubic. A **hard** blend
contributes nothing to the pad however its `k` reads, since the profile makes
the smin a step — though its own bound keeps the dilation, which in a mixed
chain is margin for the drag its smooth neighbours apply. The bake therefore
**measures** a sample of the lattice and falls back
to the whole tape when a brick's tape is not a third of the document's or less.

The result is byte-identical to the whole-tape bake, and that is a consequence
rather than a tolerance: a culled value can only be *larger* than the truth, so
one that lands inside the band already **is** the truth, and a brick with no
sample in the band stores nothing but its sign. Only what a kept brick stores
beyond the band has to be paid for with the whole tape, which is 27% of the
samples, batched. Storing culled values there instead would make the volume
overstate its own distance by 1.65 cells against the plain bake's 0.1 — a field
a marcher steps through.

`clay_item_volume_flatten_from` deliberately does **not** cull: flatten draws
the samples onto a plane *after* the fill produces them, so a brick the fill saw
as empty can come back holding the surface, and the values it would then store
were never paid for. Relax is unaffected — it samples first and relaxes the
volume afterwards.

`move_topological`'s document-sourced form goes through the pool too, since
v0.51.0, and it needed a different kind of batched source to get there. The
other two verbs evaluate their source *at* the sample lattice; this one samples
at the point the displacement pulls back to, which depends on the geodesic
weight there — so it takes a batch of **arbitrary** points rather than a fill
that knows the grid. Both places it asks the source anything go through that:
the sampling pass, and the material the geodesic walk runs over.

| 193-node layer, cell 0.02 | |
|---|---:|
| batch of arbitrary points | **305 ms** |
| the per-point walk it replaced | 4,605 ms |

15× at 193 nodes and 16× at 600, byte-identical. The geodesic walk itself was
measured before any of this and is only 4–5% of the operation — it makes 87k of
the 2.09 million source calls — so its traversal is deliberately left
sequential. This form is reached from `pyclay`; the C ABI's move takes an
existing volume.

**Putting a bake back: feather the replace.** Every one of these verbs returns
a volume that a host then places with `CLAY_OP_REPLACE`, and the hard replace
corrugates the *shading* even when no verb was applied at all (issue #67). The
surface is exact — measured by bisection the round trip's zero set deviates by
nothing — but the hard replace holds *both* fields live at the surface, and a
bake ties with the field beneath it at every sample plane, so any
finite-difference normal across the min/max branch switch pays |b−a| over its
own epsilon, rippling at the cell wavelength: ~32° of normal tilt at
`cell 0.04`, and it does not shrink with the cell. Set
`clay_volume_params.feather` (a band's width is the sweet spot) when baking,
and the placement crossfades instead: deep inside the sampled box the result
**is** the volume — one field, one gradient, tilt 1.2° at `cell 0.04` and
falling with the cell — outside the box the surrounding field continues
untouched, and the box edge stops being a hard rectangle. The blend's
correction is clamped at the volume's **band**, which is what keeps the
declared Lipschitz closed-form (max + band·1.5/feather) and per-brick culling
exact — and means a verb that moved the surface further than the band should
be baked with a band that covers it. Feather zero, and every descriptor from
before the field existed, is the hard replace byte for byte. A feathered
replace does not participate in the layer mirror: the crossfade follows one
sampled box.

Both also take an optional **mask**, and it freezes exactly: a fully masked
sample keeps its source value verbatim rather than nearly so, because a frozen
region that drifts by a rounding error per iteration is a frozen region that has
moved. It arrives as a *callable* rather than as a `MaskField` — a sampled field
is a leaf that sits below `scene` while a mask sits above it, so naming the type
in `field/` would make `field → voxel → scene → field` a cycle.

**Mask extrude takes no region at all**, and that is the point rather than an
omission: *the mask is the region*, so the painted area bounds the work and the
volume it samples is smaller than either of the others'. It has two paths that
agree to within a voxel — an SDF one that samples, and a voxel one that stays in
cell space and keeps the palette, since a grid already knows which of its cells
are on its surface. The side is `Outward` (a plate sitting on the surface),
`Inward` (a pocket) or `Centred`.

The one thing it needed that did not already exist is `brush::mask_to_field`. A
mask is a `[0,1]` scalar on a lattice and **not a distance field**: composing one
into a field expression directly puts a near-vertical step in the result and the
Lipschitz bound stops meaning anything. So the mask is *measured* first, by an
exact Euclidean distance transform — not a chamfer, whose error is anisotropic,
which would leave the rim showing flats where the lattice has them. After that
the extrude is ordinary op composition: the shell of the source intersected with
the masked region, with `border_round` giving the soft rim.

It **refuses** rather than returning something empty when the mask is empty,
never reaches the surface, or the wall is thinner than a cell. A mask that
misses is the common mistake, and an empty result would read as a bug in the
caller's painting rather than in their aim. `border_smooth` acts on a *copy*, so
extracting twice at two thicknesses cannot silently change the border.

---

## 4. Resolvers

Pure functions turning a gesture into an ordinary edit item. Nothing is read or
written, so a host can preview the result before committing it.

| Resolver | What it does | Returns |
|---|---|---|
| `cut::cut_item` | A shape drawn on a frame (rect, circle, polygon, spline lasso, **open trim curve**) becomes an extruded item sized to cut through | a `Node`, or nothing for a non-orthonormal frame or a zero-area shape |
| `brush::snakehook` | A drag from a surface anchor becomes a tapered stroke item — a horn, tendril or spike | a `Node`, or nothing for an empty path or degenerate normal |
| `brush::move_brush` | A world-space drag becomes the per-item warps that move the ASSEMBLED surface | one `grab` per contributing item, in that item's own frame |
| `brush::tube` | Nomad Sculpt's Tubes: a drawn path becomes a rope, pipe or tentacle — a swept SPHERE (exact) with no profile, a swept item (bound) with one | a `Node`, or nothing for fewer than two points or no positive radius |

**The cut is a prism, not a frustum.** A converging cut has a non-flat face and
a result that depends on where the camera stood, so the sweep is parallel and
the caller passes the frame. Keep-inner versus keep-outer is *the op* — place
the result with `Subtract` or `Intersect` — not a separate flag, which would be
a second way to say one thing.

**A trim curve is an open stroke, and that is not a flag on the lasso.**
`CutShape::from_curve` tessellates *closed*, so it is a spline lasso and the cut
is its inside. A trim stroke's endpoints must stay apart, because what closes the
outline is the frame's own bound on the side being discarded. The same control
points give different polygons and different fields. `side` names the half the
outline covers; the op still decides its fate, so covering *above* and
subtracting keeps the same material as covering *below* and intersecting. See
[`examples/30_trim_curve.py`](../examples/30_trim_curve.py).

**The Tube tool joins things that already existed.** The smooth/sharp toggle is
the curve's own `StrokePointType`; a varying radius is the stroke opcode's
per-point radius; a non-circular cross-section is `Prim::swept`; a closed tube is
`stroke_closed`. What the resolver adds is the radius distributed by **arc
length** — so a path whose control points bunch does not bunch the taper — and
the choice of representation, which is the cross-section itself rather than a
flag: no profile is a swept sphere and stays **exact** at step scale 1.0, while a
box or hexagon is a swept item at about 0.55. There is no "Validate" step,
because a tube is an ordinary item from the start rather than a live curve
waiting to become geometry. See [`examples/32_tube.py`](../examples/32_tube.py).

**Snakehook adds material rather than moving it.** ZBrush pulls existing
surface, so the body dimples slightly where the tendril came from; this grows a
tendril and leaves the body alone. The difference shows only at the base.
`taper_curve` shapes the thinning as `(1 − t)^c`: **above 1 thins away quickly**
and leaves a long thin whip, below 1 holds thickness and then drops, which is
the shape of a horn.

---

## 5. The stroke engine

Samples in, edit items out. `resolve_stroke` is the pure core; three consumers
take the resulting stamps — `apply_to_grid` writes voxels, `stamps_to_nodes`
turns them into SDF nodes, and `apply_to_mask` paints a mask. Because stamps
become ordinary edits, undo, coalescing and serialization apply unchanged.

Masking is therefore the **same gesture** as sculpting, resolved by the same
code: everything in the table below reaches a mask stroke. `apply_to_mask` takes
a *target* rather than a direction, so painting and erasing are one call, and it
owns the conversion from a stamp's **world** radius to a footprint sized in
**mask cells** — a caller doing that by hand gets a stroke whose width tracks the
mask's resolution instead of the brush's radius.

| Feature | Field | What it does |
|---|---|---|
| Spacing | `spacing` | Distance between stamps as a fraction of brush **diameter**. 0.25 is dense; 1.0 places them just touching |
| Pressure | `pressure.size`, `.strength`, `.curve` | Exponents on normalized pressure. 0 disables a channel — that is what "size only" and "flow only" brushes are. On an SDF layer the **strength** channel reaches relief, incise and add only — see below |
| Jitter | `jitter_position`, `jitter_size`, `jitter_rotation`, `seed` | Derived from the stamp index and seed, **never from a random source**, so a stroke resolves identically everywhere |
| Rotate along stroke | `rotate_along_stroke` | Turns each stamp to follow the path; only matters for stamps that are not rotationally symmetric |
| Taper | `taper_start`, `taper_end` | Fraction of stroke length over which the radius ramps in and out |
| Steady stroke | `steady` | "Lazy mouse" — the emission point trails the cursor, smoothing a shaky path |
| Accumulation | `accumulation` | `Buildup`: passing twice acts twice. `Clamped`: the stroke reaches its strength once, however many stamps overlap. Same caveat as strength on an SDF layer |
| Base | `radius`, `strength` | What pressure, taper and jitter modulate |

Presets serialize with a **schema version from the first release** rather than
one retrofitted later: presets outlive engine versions, and a library of them
silently reinterpreted by a later build is the failure that number prevents.
Deserialization accepts its own version and earlier ones, and **refuses a newer
one** rather than reading a prefix and pretending.

**A stamp's strength reaches an SDF item only where scaling preserves what the
op means.** For relief and incise `blend.k` is the amplitude, and half an
amplitude is exactly half the displacement — so pressure and accumulation mean
something, and ClayBuildup gets the buildup it is named for. For add the amount
*is* the stamp: strength scales the whole deposit — scale, rounding and blend
together — so a half-strength stamp is a self-similar half-size deposit, zero
authors no node at all, and full strength is the item exactly as authored. One
asymmetry is deliberate: `Clamped` accumulation divides a relief amplitude by
the expected overlap so the stroke reaches its strength once, but it does **not**
divide an add stamp's deposit — overlapping unions do not add up, so a clamped
add stroke is identical to a buildup one. Every other op reads `blend.k` as a
radius, a depth or a half-thickness: scaling those would change the *shape*
rather than the amount, silently and differently per op — a groove at half
strength is not a shallower groove. Those ops ignore strength.

**A stroke carries `rounding`**, which matters more than it sounds: groove and
tongue read it as the channel half-width and relief and incise as the falloff
width. A stroke that drops it leaves relief declaring an amplitude over ~1e-6,
and the step scale collapses from 0.118 to 2.8e-06 — the geometry is there and
nothing can march it. See [`examples/29_claybuildup_smooth.py`](../examples/29_claybuildup_smooth.py).

A tap has to leave a mark: a single sample, or a path shorter than one spacing,
yields exactly one stamp at the start.

---

## 6. Armatures

ZBrush's ZSpheres. A **tree** of spheres: each node names a parent, a link is
the sphere-swept cone between the two, and the whole tree is ONE edit item.

This is the mechanism for **blocking a figure out**, and what it buys is
structure rather than a new shape.
[`examples/34_organic_character.py`](../examples/34_organic_character.py) is
forty-odd primitives whose positions are numbers typed into a file: there is no
way there to say "an arm hangs from this shoulder", only to work out where the
arm's endpoints would be if it did, and to retype them all when the shoulder
moves. The same figure as an armature is 18 nodes and a tree, and moving the
shoulder is one edit that the arm follows —
[`examples/40_armature.py`](../examples/40_armature.py) measures exactly that.

**It is the stroke with its chain generalised**, which is worth knowing rather
than taking on faith. A stroke walks `(i, i+1)`, so its topology is a line; an
armature walks `(i, parent[i])`, so it can branch. The link, the smooth union
between links and the blend parameter are literally shared code, so a
line-shaped armature and the stroke built from the same points agree to 4.8e-07
across four blends — and the example asserts it, so the two cannot drift apart.

**`blend_k` is the skin.** At 0 the links are a hard union and you see the
armature; above it the smooth union fills the crotches between links, which is
what turns a stick figure into a body. There is no separate skinning pass.

| Edit | What it does |
|---|---|
| `add_child` | Append a node under a parent |
| `move` | Move a node BY a delta, **carrying every descendant**. The property the feature exists for |
| `set_radius` | Resize one node; the links either side follow |
| `set_sign` | Flip one node between building (+1) and carving (-1) — see negative nodes below |
| `delete_subtree` | Remove a node and everything under it, renumbering the survivors and their signs |
| `add_child` mirrored | Add a node **and** its reflection through x = 0, under the mirror of the parent where one is known. Returns 1 for a node on the plane, whose reflection is itself |

Every one is a pure function over `(nodes, parents)`; the command that installs
one is a whole-tree replace, so its inverse is exactly the tree that was there
before and one undo puts a whole arm back.

**Negative nodes** (#99) are ZBrush's negative ZSphere: a sign per node, +1 by
default, and the field is **the armature of the positive nodes MINUS the
armature of the negative nodes**, each half built exactly as the unsigned
armature is, the carve applied after the whole positive fold. A link exists
only between two nodes of the *same* sign — skin between builders, carve
between carvers — so skin along a negative node's links is never drawn (the
membrane cut a trailing subtract sphere cannot express: no sleeve bridges the
hollow's opening) and a carve never sweeps a positive parent's radius (an
eye-socket child does not swallow the head it is cut into). A negative
parent-child pair carves its link as one swept segment, so a deep hollow is a
scoop; a negative node may carry children. Eye sockets, mouth cavities and the
hollow of an ear are blocked out this way —
[`examples/40_armature.py`](../examples/40_armature.py) carves the figure's eye
sockets with two of them. The sign travels with the tree: set at build time
(`clay_item_set_armature_signs`, pyclay's `signs=`), flipped on a placed rig
(`set_sign`), read back (`clay_layer_armature_signs`, positive-padded exactly
as short parents read as roots) and saved with the document at format minor 8.

Two constraints are load-bearing rather than incidental:

- **The fold order is ascending node index.** `csmin_quadratic` is not
  associative, so three links meeting at a hip give a different field depending
  on the order they combine. Fixing the order is what makes an armature evaluate
  the same on every backend, and the parity corpus carries a branching armature
  so that is checked rather than assumed.
- **A root contributes its sphere only when nothing else names it.** Giving
  every root a sphere on top of its links is invisible at `blend_k = 0`, because
  `min` is idempotent, and wrong above it — a smooth union of two overlapping
  terms pulls the surface outward, and the chain armature stopped matching its
  stroke by 8e-2. The isolated-node case still needs the sphere, which is why
  the rule is "unreferenced" rather than "not a root".

**Per-node rotation is deliberately absent.** A sphere is isotropic, so rotating
one changes no distance and no surface. It earns its place in ZBrush because the
adaptive skin lays out quads whose edge flow follows the node frames; marching
cubes, surface nets and dual contouring do not consult one.

A tree whose parents do not form a forest — a cycle, or an index out of range —
is **refused** rather than stored, because a cycle would make the field depend
on the order links are walked rather than on the tree.

**A placed armature reads back** (#77). The two halves come back the way they
went in: `clay_layer_stroke_points` serves the nodes — the same x, y, z, radius
list a stroke's reader already returned — and `clay_layer_armature_parents` the
topology, one parent index per node with a root naming itself. The indices are
the ones `clay_layer_armature_edit` takes, which is what lets a host that
reloaded a document re-pose a rig it did not author; branching topology is not
recoverable from the skinned surface, so without the reader the tree was simply
gone. `clay_layer_node_prim` answers which primitive a placed node carries, so
the armature is findable without probing readers until one stops refusing, and
`clay_layer_node_count` / `clay_layer_node_at` say which nodes exist to ask
about — without them the host still had to guess ids, and a rig placed after a
run of removed nodes was invisible to the guess.

**And so does a plain item** (#317). The walk above ends in a typed reader for
every kind except the one every other branch falls through to: a placed
primitive answered which primitive it was and nothing else, so where it stood,
how big it was and how it combined lived in a table beside the document, keyed
by node id, that the host also had to keep correct across undo and redo.
`clay_layer_node_transform`, `clay_layer_node_params` and
`clay_layer_node_op_blend` are the reading half of the setters that write those
values, each taking what its setter takes. `clay_layer_node_influence_bound` is
not a substitute and never was: it is dilated by rounding and blend support and
covers a layer mirror's reflection too, so an item at x = 0.9 in a mirrored
layer reports a bound centred on the origin.

---

## 7. Voxel sculpting verbs

In-place edits of a palette-indexed grid. Each reads a **snapshot** of the
region first, so a cell's outcome never depends on a neighbour the same call
already changed.

| Verb | What it does |
|---|---|
| `sculpt_smooth` | Majority filter over the 26-neighbourhood: spurs dissolve, notches fill |
| `sculpt_inflate` | `amount > 0` dilates, `< 0` erodes, `\|amount\|` times |
| `sculpt_flatten` | Pull the surface onto a plane through the brush centre — two-sided |
| `sculpt_pinch` | Move surface cells one step toward the brush centre |
| `sculpt_magnify` | ...and one step away — the inverse, sharing pinch's walk so the two cannot drift apart |
| `sculpt_scrape` | Flatten **and** smooth from one snapshot. Calling both in sequence is not the same thing |
| `sculpt_smudge` | Drag **surface** material along a direction, leaving the interior. Grab moves a lump; smudge smears a skin |
| `sculpt_grab` | Translate occupancy through the same inverse map the SDF `grab` deformer uses, so both representations mean the same thing. Resampling is nearest-cell and rounds **per axis**, so a displacement under half a cell on every axis moves nothing. **It also does not compose** — for a drag, use `VoxelGrid.grab` / `clay_voxel_grab_begin` (see below) |
| `sculpt_fill_cavities` | Fill pockets: an empty cell with ≥4 of its 6 face neighbours occupied is inside a cavity. The rule is local, so it fills what is **narrow**, not what is enclosed — a through-hole wider than one cell does not qualify, a one-cell perforation does. Its everyday input is a **dithered soft stamp**, which leaves single-cell holes through its own deposit; closing them cut a test stroke's greedy mesh by 27%. `repair_fill_voids` is the one for sealed voids, and neither substitutes for the other |
| `sculpt_carve_alpha` | A caller-supplied scalar stamp modulating per-cell strength. **The engine decodes no images** — a host with an alpha has already loaded a PNG |
| `sculpt_crease` | **Does not exist, deliberately** — DamStandard on a lattice is a recipe rather than a verb. See below |
| `repair_report` | What a pre-bake check wants to know, without performing the fix |
| `repair_close_holes` | Seal perforations by the same pocket rule. Only ever adds cells |
| `repair_fill_voids` | Fill every empty cell the outside cannot reach — enclosure is *decided*, not guessed locally |

**A voxel grab does not compose, and a drag is a gesture.** A grab of N cells is
not N grabs of one cell. `sculpt_grab` reads the grid, resamples occupancy
through the falloff and writes back, so the next call reads its own output — and
the displacement is rounded to whole cells *after* the falloff weights it, so at
one cell only the very middle of the region rounds to a cell, which inside solid
material changes no occupancy at all. Measured on a solid ball 16 cells across,
the same total drag of 8 cells in +y, counting cells that ended up somewhere
new:

| footprint | 1 × 8 | 2 × 4 | 4 × 2 | 8 × 1 |
|---|---|---|---|---|
| 24 cells | 59 | 61 | **0** | **0** |
| 32 cells | 205 | 169 | 190 | **0** |
| 40 cells | 357 | 376 | 293 | 126 |

Occupancy is not conserved across the split either (2109 cells at rest became
2235, 2298 and 2371), so composed grabs smear and duplicate rather than
translate. Accumulating past the cell size host-side and emitting whole cells —
the obvious reading of the rounding note — produces exactly the stream of
one-cell grabs that moves nothing.

`VoxelGrid.grab` and `clay_voxel_grab_begin` are the answer: the gesture
captures the material as it is, and every `update` takes the **total**
displacement from the anchor, resampled from that capture. A run of updates ends
where a single one to the same total would, repeating one changes nothing, and a
pointer that comes back to where it started puts the material back — the same
shape `clay_sdf_move_begin` has on the field side. What it does *not* change is
the half-cell dead zone: occupancy is binary and there is no sub-cell state to
move. What changes is that the total is measured from the anchor, so a slow drag
accumulates toward that half cell instead of rounding to zero every frame.

Every verb takes `BrushParams`: `size`, shape (`Cube`/`Sphere`), falloff
(`Constant`/`Linear`/`Smooth`/`Gaussian`), `strength`, `seed`, and an optional
**mask**. Where a mask is given the effective weight is scaled by `1 − mask`, so
a fully masked cell is untouched by *every* verb rather than by a hand-picked
few.

**A grid can carry a stack of resolution levels.** Blocking a form out wants
coarse cells and detailing wants fine ones, and paying for fine cells everywhere
to get them in one place is what a single-resolution grid forces. `add_level`
pushes a finer level, `set_active_level` chooses which one the verbs above edit,
and `drop_level` discards one. The coarsest level is the one that was always
there, so a grid with a single level behaves exactly as it did and serialises to
the bytes it always did.

**Any verb here can be a valid call that changes nothing** — a sub-cell grab or
### DamStandard on a voxel layer: a recipe, not a verb

The V-groove is `Op::Incise` on an SDF layer and `MeshBrush::Crease` on a mesh
layer. On a voxel layer it is a stroked erode with a **constant** falloff at a
small size:

```cpp
BrushParams p;
p.size = 3;                          // small: the groove is as wide as the brush
p.shape = BrushShape::Sphere;
p.falloff = BrushFalloff::Constant;  // NOT smooth — see below
grid.sculpt_inflate(cell, p, -2);    // negative carves; positive raises the ridge
```

stamped along the stroke by the brush engine as any other verb is. `amount > 0`
raises the crisp ridge instead, which is the Alt behaviour.

**Constant, not smooth, and that is the surprising part.** Occupancy is binary,
so a fractional weight is resolved by dithering against a hash of the cell
coordinate — and a three-cell brush has too few cells for a smooth taper to
average out. Measured on a stroked groove, surface height along the line
(6 is untouched, depth 2 requested):

| falloff | profile along the stroke |
|---|---|
| `Constant` | `4 4 4 4 4 4 4 4 4 4 4 4 4` |
| `Linear` | `4 4 5 4 5 5 5 4 5 5 4 4 5` |
| `Smooth` | `5 5 5 5 5 5 5 4 5 5 4 4 5` |
| `Gaussian` | `5 5 5 5 5 5 5 5 5 5 4 4 5` |

A soft falloff on a tight brush does not give a soft crevice; it gives a
speckled one.

**Why there is no verb.** A `sculpt_crease` was scoped, implemented and
measured, and stroked along a line it produced a profile **identical** to the
plain erode above; on a single dab it was *shallower*, because its squeeze
filled the groove back in. The reason is structural: on a mesh the pinch moves
*vertices* tangentially and steepens the walls of a surface sheet, while on a
lattice moving material toward the groove centre puts material **into** the
groove. The operation that sharpens a mesh crease fills a voxel one, so what is
left of DamStandard here is the depth profile — which is the falloff.
`openspec/ROADMAP.md` records the decision and the numbers.

smudge, a flatten on an already-flat region, a dithered stamp that misses every
cell it was offered, a footprint over empty space. None of that is an error and
none of it is reported as one. To tell a live edit from a dead one, read
`VoxelGrid::change_count()` (`clay_voxel_change_count`, `grid.change_count` in
pyclay) before and after: it counts cell writes that actually changed a cell,
is monotone, and only the difference means anything. `occupied_count` is not a
substitute — grab and magnify move material without adding any, so the count
can be identical across an edit that moved a whole lump. The one caveat is
`sculpt_pinch` and `sculpt_magnify`, which may revisit a cell within one call
and so give an upper bound rather than an exact tally.

### Sculpt layers: a pass you can dial back

Bracket a run of verbs with `begin_sculpt_layer` / `end_sculpt_layer` and the
grid records what those edits **changed** — for every cell it touched, the value
before and the value after. The pass's strength stays adjustable afterwards.

This is not undo, and the difference is the whole point. Undo is a **stack**:
removing a pass from ten minutes ago discards everything since. A sculpt layer
is **addressable** — make the wrinkles, keep sculpting for an hour, then take
the wrinkles to 40% without disturbing anything made after them.

A layer records what its pass **did**, not the brushes that did it. Dialling a
layer replays recorded cells; it does not re-run the strokes. So a pass whose
result depended on the layer beneath it — `sculpt_smooth` reads its neighbours,
`sculpt_inflate` grows from what is already there — keeps the result it
recorded when that layer is dialled away. Re-running instead would make a
layer's content depend on whatever sits below it, which is the opposite of
addressable, and would re-evaluate the whole stack on every slider move. This
is what ZBrush's layers do too.

**What a fraction means on binary occupancy** is where this departs from ZBrush
by representation rather than by choice. ZBrush interpolates vertex offsets, a
continuous quantity; a voxel is there or it is not. So a fractional strength is
a reproducible fraction of the **cells**, dithered against the same
cell-coordinate hash the falloff brushes use (`src/voxel/dither.h`, shared by
both). Three properties follow:

- the same strength picks the same cells on **every platform and every run** —
  the hash is over integer cell coordinates, not an RNG draw;
- raising the strength **adds** cells to the ones already showing rather than
  reshuffling, because each layer holds one fixed seed;
- **0 and 1 are exact** — the grid without the pass, and the pass applied
  directly — because the dither admits none and all at the ends.

Layers composite **bottom-up**, so the last one recorded wins where two
overlap. `move_sculpt_layer` changes that, which is why reordering is a feature
rather than a caveat; `merge_sculpt_layer_down` folds two passes into one that
still dials, at full strength — keeping the upper layer's dither would bake a
fractional subset in and leave the result unable to reach the other cells
again.

**Memory is reported, not enforced.** `sculpt_layer_bytes` and
`sculpt_layer_total_bytes` say what the stack costs: a pass costs its own
cells, not the model, so a stroke over a thousand cells is a thousand entries
whether the grid holds a thousand voxels or a million. Nothing caps it, because
a cap that silently stopped recording would leave the pass on the grid and
un-dialable — a correctness bug wearing a memory limit's clothes. A host with a
budget merges layers down (one entry per cell instead of two) or ends the
layer. Both are decisions a user can see.

They ride in the `.clayspace` voxel payload at **minor 10**, storing the diff
rather than the result, so a reloaded document is still dialable. That payload
is opaque to the container, so an older reader meets an unknown tag and falls
back to the **flattened** grid — the sculpt is exactly what the layers composed
to, it just stops being adjustable. Nothing is lost that the older build could
have shown.

**SDF layers do not have them.** A diff of changed cells has no counterpart in
an edit list, where the equivalent is a weighted group; that waits on
`expose-scene-groups`.

`examples/52_sculpt_layers.py` renders one pass at five strengths.

---

## 8. Fixed-topology mesh brushes

The classical sculpting mode — vertices move, on a mesh layer's own triangles —
with one line held above everything else: **topology never changes.** No verb
here creates, splits, deletes or reorders a polygon or a vertex. `indices` and
`quads` come out byte for byte as they went in, so a quad mesh sculpted here is
still the same quad mesh.

That is the whole reason they exist. A mesh layer carries triangles verbatim
(§ [`docs/08`](08-mesh-readback.md)), and until these landed the only way to
*edit* one was `Volume::from_mesh`, which resamples the model onto a lattice:
the sculpt comes back, the edge loops and the uvs do not. The round trip
`quad_mesh` and `add-representation-round-trip` built is sculpt SDF → quad
export → retopo/UV elsewhere → bake, and the step it had no verb for is the one
an artist wants next — refine ON the retopologized mesh.

This does not change what a document evaluates to. A sculpted mesh layer is
still never evaluated, never blends with a field, and exports exactly as its
(now edited) vertices say.

| Verb | What it does to the vertices |
|---|---|
| `Grab` | Drag the region by the stroke's per-stamp motion. Polygons stretch or compress; none are created |
| `Draw` | Displace along the **region's averaged normal** — one shared direction per stamp, which is what makes it a rounded organic swell rather than a balloon |
| `Inflate` | Displace along **each vertex's own normal**, signed. The per-vertex direction is exactly what distinguishes it from draw |
| `Smooth` | Laplacian average over the one-ring, weighted by falloff |
| `Pinch` | Signed **tangential** gather (+) or spread (−) about the brush centre — one deformation with one sign, as `magnify` is for fields. The normal component is removed, so a pinch gathers along the surface instead of sinking the region |
| `Flatten` | Project toward a plane, with the same `TwoSided` / `CutOnly` / `FillOnly` mode `field::flatten` established — because cut-only *is* Trim Dynamic and hPolish |
| `Clay` | Draw's deposit **clamped to a plane** floating at the stamp height: material is added up to the plane and no further, which is what makes flat-topped strips instead of a swell. On a flat surface it is draw; the difference is what it does to an uneven one |
| `Crease` | A tight negative draw **and** a pinch in one stamp. Sequenced separately they leave a rounded ditch, because the pinch would gather vertices the draw had already pushed down |
| `Scrape` | Flatten cut-only **and** smooth from one snapshot, mirroring the `sculpt_scrape` rule that calling the two in sequence is not the same thing |
| `Polish` | Smooth **gated by how much the surface bends**: full strength where the normals around a vertex agree, fading to zero at twice the threshold angle. Noise goes, hard edges stay, and `polish_angle` is the whole brush |
| `Snakehook` | Grab re-anchored on the class it is dragging, so the region walks with the pull. On fixed topology it stretches triangles to the extreme — **stated behaviour, not a defect**: that stretch is the artist's signal the mesh wants retopo, exactly as Blender behaves with Dyntopo off. `brush::snakehook` remains the verb for GROWING new volume |
| `Relax` | Slide vertices **along** the surface to even their spacing, rather than toward the Laplacian average. Smooth reshapes; this redistributes. It matters more here than in a tool that can subdivide: topology is fixed, so a large grab stretches the triangles it has and this is what recovers them without a retopo round trip |
| `Layer` | Deposit up to a **ceiling** `layer_height` above the surface as the STROKE found it, and no further. Every other deposit verb accumulates, so a slow stroke digs deeper than a fast one over the same path; this one does not. Needs the stroke's `VertexDeltas` to know where it started |
| `Nudge` | Push material along the surface in the drag direction. Grab carries the region rigidly; this slides it. Per-vertex tangent planes rather than the region's average normal, so a curved region's rim is not pushed off the surface |
| `Paint` | Blend each vertex's **colour** toward the target by the brush's own weight. Blender's Paint. Moves no vertex |
| `Smear` | Push existing colour along the drag, taking each vertex's new colour from the one-ring neighbour most nearly **opposite** it, weighted by alignment. Blender's Smear. A zero drag direction is no smear rather than a smooth. Moves no vertex |

**Deformers are not brushes.** `taper` and `twist` reach a mesh layer through
`MeshSculptor::deform`, and they are a different kind of thing from the sixteen
verbs above: no centre, no radius, no falloff, because a deformer states
something about the FORM and a brush states something about a dab. They act on
every vertex, scaled by the mask — which is what holds part of a form still —
and a fully masked vertex is bit-identical to where it started.

They carry a FRAME, which the SDF versions do not need: the canonical taper and
twist are maps about one axis, an SDF item supplies that axis from its own
transform, and a mesh layer has none to supply. `origin` is where the span
starts, `axis` is the direction it runs, and material past the span travels
rigidly with the end rather than winding on for ever.

They are applied FORWARDS, once per vertex, where an SDF deformer must run
backwards to answer "where did the material at p come from". Forwards is the
easier direction and the exact one — a point round trips through the mesh's
forward map and the kernel's inverse map to within float epsilon — so a tapered
mesh and a tapered field are the same shape rather than two plausible ones.

**`bend` is deliberately absent, and that is a measurement.** `cbend_point`
takes its angle from `p.x` and then moves `p.x`, so negating the parameter is
not its inverse (worst error 1.73, against 1.2e-07 for taper). Worse, it is not
injective: at `k = 0.9`, rest points at `x = -1.74` and `x = +1.75` land 0.0101
apart. The bend folds over, so past a gentle angle there is no forward map to
write. Deciding what a mesh bend should be is a decision about the SDF bend's
convention, and it is recorded in the roadmap rather than guessed at here.

**Nothing re-tessellates, and `Relax` does not rescue a deformation.** That is
worth stating precisely, because the obvious guess is wrong and `examples/57`
measures it: a taper leaves the top of a column with the SAME vertex count
around a SMALLER circumference, so the damage is *anisotropy*, not uneven
spacing. Relax slides vertices along the surface — it can even out uneven
spacing, and it cannot change how many vertices a ring has. Measured, six relax
passes move edge-length variation from 0.2929 to 0.3050 after a taper: slightly
*worse*, since sliding along a tangent plane is not shape-preserving.

Relax is the recovery for a large `Grab`, where the damage genuinely is uneven
spacing. Recovering a deformation needs re-tessellation, which this engine
deliberately does not do.

**The colour pair moves nothing.** `Paint` and `Smear` are the only two verbs
that leave `positions` and `normals` byte-identical — the exact mirror of the
guarantee the other fourteen make about `colors` — so a colour pass over a
finished sculpt shows up as no diff on the geometry at all. Both REFUSE a mesh
with no colour attribute rather than creating one: twelve bytes per vertex is a
real cost to hide behind a brush stroke, and a silent creation would make "I
painted and nothing happened" indistinguishable from "this mesh had no
colours". `MeshSculptor::ensure_colors` is the deliberate act.

**One stamp is one operation.** Every verb reads a *pre-stamp snapshot* of the
region — positions, normals, the region's averaged normal, its centroid and its
plane — so `draw` does not chase its own deposit, `smooth`'s Laplacian is a
simultaneous average rather than a vertex-order-dependent sweep, and the
composed verbs are single operations rather than sequences.

**Reach is measured along the surface.** The Move Topological rule: a brush on
the upper lip must not drag the chin through a closed mouth. A class is in the
region when a path over the one-ring reaches it without leaving the brush's ball
and within a path budget; the *weight* then comes from the straight-line
distance, because a path measured along edges overestimates geodesic distance by
a direction-dependent amount and a falloff driven by it bands visibly. On a
connected sheet the surface-measured and straight-line modes agree bit for bit.

**Adjacency is built over weld classes**, not raw indices. A UV seam, a hard
edge or a per-face normal duplicates a position into independent indices, and a
ring built over those stops dead at every seam. Vertices sharing a position are
one class, and every verb writes one displacement to all of a class's members —
so a seam cannot open into a crack.

**Masks reach every verb for free.** Each vertex's weight is scaled by
`1 − mask` at its world position, the rule every voxel verb already follows, so
a painted mask protects polygons from all sixteen verbs — `Grab` and
`Snakehook` included, and the colour pair too, since their weight is the same
weight — with no per-verb code.

**The stroke engine gained its fourth consumer.** `resolve_stroke` already fed
`apply_to_grid`, `stamps_to_nodes` and `apply_to_mask`; `apply_to_mesh` inherits
spacing, pressure curves, deterministic jitter, taper, steady stroke and
buildup-versus-clamped accumulation with no new machinery. Buildup accumulation
is what turns one `Clay` stamp into ClayBuildup.

**Undo is a vertex-delta record.** A mesh stroke is destructive vertex
displacement, not an edit-list node, so it cannot undo through the command
vocabulary the way an SDF stroke does. `mesh::VertexDeltas` records the vertices
a gesture actually reached — sparse, and coalesced so one stroke is one step —
and restores positions *and* normals bit-exactly. `indices` and `quads` are not
recorded, because nothing here can change them.

**Mesh layers became pickable.** `raycast_scene` cannot see a mesh layer and
never will — that is the layer's design — so `pick::raycast_mesh` asks the BVH
directly, through the layer transform, and hands back the surface point, the
normal and the weld class a surface walk should start from. Back faces are not
culled: a sculptor working inside a shell means it.

**Not dynamic topology, on this representation.** No dyntopo, no multires,
no remeshing, no subdivision reaches a mesh layer's own triangles, and that
contract is what makes the layer worth holding after a retopology pass — see
the amended non-goal in
[`docs/sculpt_comparison.md`](sculpt_comparison.md). What changed on
2026-08-29 is the wider claim rather than this one: adaptive topology and a
subdivision hierarchy are now scoped as SEPARATE representations beside this
one, in `openspec/ROADMAP.md` Phase 5, and neither is implemented today. When
they land, a caller converts into them by name; these sixteen verbs still
guarantee `indices` and `quads` byte for byte. Not in the parity system
either: this is CPU-side like the voxel verbs, and the determinism bar is
asserted instead — same stroke, same mesh, same result, bit for bit.

Runnable: [`examples/45_mesh_brushes.py`](../examples/45_mesh_brushes.py),
[`46_mesh_brush_compositions.py`](../examples/46_mesh_brush_compositions.py) and
[`47_mesh_brush_reach_and_undo.py`](../examples/47_mesh_brush_reach_and_undo.py).

---

## 8b. Adaptive topology — the third mesh mode

Section 8 is the fixed-topology mode: sixteen verbs that move vertices and never
touch a polygon. This is the other one.

**What was actually missing, stated precisely.** Not "dyntopo" — *there was no
representation in this library whose connectivity could change.* `Mesh` is flat
arrays a mutation renumbers. `Adjacency` is CSR that goes stale on a count
change and says so. `Bvh::refit` refuses a topology change by design.
`VertexDeltas` deliberately records no indices. Every one of those is the right
decision for what it serves, and together they mean adaptive topology cannot be
retrofitted — it has to be a representation of its own.

### The two modes, and choosing between them

| | Mesh layer (`MeshSculptor`) | `DynamicSurface` (`DynamicSculptor`) |
|---|---|---|
| Topology | **Never changes.** `indices` and `quads` byte-identical | Changes locally under the brush |
| What a large pull does | Stretches the triangles it has | Creates the triangles it needs |
| Quads | Preserved through every verb | **None.** Triangles, and the export derives no pairing |
| Best for | An imported retopologised model whose UVs and edge flow are paid for | Free-form blocking out, where the form is not settled |
| Undo | `VertexDeltas` — positions, normals, colours | `TopologyDelta` — connectivity too |

**A caller converts into an adaptive surface deliberately.** It is never a mode
the fixed sculptor slips into, and that is what keeps a mesh layer worth holding
after a retopology pass.

### What it costs

A half-edge surface with stable handles costs several times a flat mesh per
triangle: four pools of records, two half-edges per edge, and no compaction on
erase. `DynamicSurface::bytes()` reports it, and `dead_slots` says how much of
it is storage a session's edits left behind — a surface never compacts, so a
long session's pools grow with its history rather than its content, and
round-tripping through `to_mesh`/`from_mesh` is how a host reclaims that.

**In time, a dab costs its footprint and not the model.** That is the property
the whole mode depends on — a sculptor works on a model too big to redraw, so a
stamp that scales with the surface is unusable however correct it is. Measured
on a flat patch at a fixed tessellation density, with the same brush at every
size, so the only thing changing is how much surface surrounds the dab:

| Faces | Dab, topology on | Dab, topology off |
|-------|------------------|-------------------|
| 100k | 1.41 ms | 0.33 ms |
| 1M | 1.94 ms | 1.26 ms |
| 5M | 3.43 ms | 2.47 ms |

A 50x model is a 2.4x dab. Note which fixture that is: subdividing a
fixed-radius sphere for each size would make the surface FINER, so a
fixed-radius brush would legitimately cover quadratically more faces, and the
resulting curve would say nothing about locality. This is a flat patch at fixed
spacing, where only the surrounding surface grows — `BM_DynamicStamp` in
`benchmarks/bench_main.cpp`, on a 24-core x86 box.

Conversion is not local and is not meant to be: `from_mesh` on 320k faces is a
few hundred milliseconds and on 5M it is seconds, which is why it is a
deliberate one-off and not a mode a host toggles per stroke.

### The three parts

**The operators** — split, collapse, flip. Each is ATOMIC: it decides
completely against the untouched surface and only then writes, so a refused
operation changes nothing at all and there is no rollback path because there is
nothing to roll back. Each honours edge constraints itself — boundary, UV seam,
sharp, material, user-locked — rather than trusting a caller to have filtered
its input.

Collapse has the longest refusal list because it is the dangerous one: a
topological link-condition test, plus duplicate triangles, inversion,
degeneracy, boundary corruption and normal flip past a threshold. The link
condition alone is not sufficient and the tetrahedron proves it — every one of
its edges passes, and collapsing any leaves two faces on the same three
vertices.

**The remesher** — split above `target * split_factor`, collapse below
`target * collapse_factor`, flip toward better triangles, then relax
tangentially.

The gap between the two factors is hysteresis, and **it does not work on its
own**: splitting halves a length, and the two thresholds are less than a factor
of two apart, so an edge just over the split threshold becomes two just under
the collapse one. What makes it converge is refusing a collapse whose result
would be long enough to split again — the standard incremental-remeshing rule,
and the reason the classic 4/3 and 4/5 factors work at all.

The target is **brush-relative** by default: `radius / detail_resolution`. A
world-unit target means an artist shrinking the brush to add detail gets the
same triangles back; a fraction of the radius is what a sculptor means by
"detail", and it needs no second slider.

Remesh timing is **per verb, with a reason each**, because the right answer
differs: Grab remeshes *after* (it stretches, and the stretch is what needs
refining), Clay *before* (a deposit onto triangles too coarse to hold its shape
is a smooth bump where the brush promised an edge), Snakehook *both*.

**The chunked index** — leaves of a few hundred faces, each simultaneously the
BVH leaf, the brush candidate set, the parallel work unit, the normal-recompute
unit, the dirty-tracking unit and the host's upload unit. A per-face tree gives
the first and leaves the library to invent a different granularity for each of
the others.

### Which verbs it offers

Fifteen of the sixteen. **Layer is declined**, structurally rather than by
omission: its ceiling is measured per vertex from where the surface was when the
*stroke* began, and half the vertices under the brush at the end of an adaptive
stroke did not exist at the start. A verb that silently became Draw for the new
vertices and Layer for the old ones would be worse than one that says it is not
offered.

### Determinism

The same verbs on the same input give the same surface, connectivity included.
That is not a consequence of single-threading — it is an ordering rule: every
candidate set is **sorted by stable slot id** before any operator runs. A
spatial query returns faces in the tree's traversal order, the tree's shape
depends on the history of edits, and a remesher that split them in that order
would give a different surface for a differently-edited but identical input.

### What a host is told

Three revisions — topology, geometry, attributes — advancing independently, so
an index buffer is re-uploaded only when connectivity changed. A stamp reports
the chunks it touched, and chunk data is **copied into caller-owned buffers**
behind a capacity query: a mutation can move or free anything, so a borrowed
pointer held across one would be a use-after-free with no generation to check.

Runnable: [`examples/66_dynamic_topology.py`](../examples/66_dynamic_topology.py)
— a 1,200-triangle sphere becomes a nose, an ear and a horn, with the locality
of the refinement measured rather than illustrated.
---

## 8d. Multiresolution — the fourth mesh mode

Sections 8, 8b and 8c are ordered by what may change: fixed topology never
changes, an adaptive surface's changes locally, and a voxel remesh replaces all
of it. This is the fourth answer, and the one a production workflow spends most
of its time in: **topology changes only by deterministic subdivision, and detail
survives an edit to the form beneath it.**

**What was actually missing, stated as an artist would.** On a mesh layer, fine
detail and coarse form are the same edit. The detail IS the vertex positions and
there is no record of which part of a position was form and which part was
wrinkle — so a pass that changes proportions destroys the pass before it. Every
sculptor who has put pores on a face and then been told the brow is wrong knows
what that costs.

### The model, and it is one line

```
P(0) = the base cage                 the production geometry, sculpted directly
S(n) = Subdivide(P(n-1))             pure Catmull-Clark, no detail
P(n) = S(n) + Frame(n) * Detail(n)   the level as the artist sees it
```

Levels are **not** a stack of unrelated meshes. With no relationship between a
level and its parent, a change to the parent has no defined effect on the child,
so either the fine detail or the coarse edit has to be discarded — and keeping
both is the entire purpose. The relationship IS the feature.

### The four modes, and choosing between them

| | Mesh layer | `DynamicSurface` | `MultiresSurface` | Voxel remesh |
|---|---|---|---|---|
| Topology | Never changes | Changes locally | Changes only by subdivision | Replaced wholesale |
| Detail vs form | The same edit | The same edit | **Separate, by level** | n/a |
| Quads | Preserved | None | Preserved at the cage, produced above it | None |
| Best for | An imported retopologised model | Free-form blocking out | Refining a settled production cage | Rebuilding density after a long session |
| Undo | `VertexDeltas` | `TopologyDelta` | `MultiresDelta` — coefficients and cage positions |`Step::Kind::MeshReplace` |

The lifecycle is explicit and one-way per stage: free-form construction on an
adaptive surface, freeze the topology, retopologise, then a hierarchy over the
result. Blurring that ordering is what makes multiresolution implementations
fragile elsewhere — and it is why `MultiresSurface::set_base_mesh` **refuses** a
cage change on a hierarchy that already carries detail. The supported route is
`project_from`, which is explicit and priced.

### Detail lives in a transported frame, not in world space

A world-space offset is correct for small changes and wrong for the case the
feature exists for: rotate or bend the parent surface and a stored world vector
no longer points along the surface it belongs to, so wrinkles shear off the
cheek that carried them. Detail is therefore three coefficients — tangent,
bitangent, normal — against an orthonormal frame derived from the **pure
subdivision** surface, never from the surface with detail already on it.

The frame is **transported rather than rebuilt**. A tangent re-derived from
whichever neighbour comes first geometrically flips under a deformation that
barely moves the surface, and a flipped frame rotates the detail stored in it —
a swimming, smearing artefact that appears in a render and in no numeric test
that only checks magnitudes. So the frame is built once at the cage (a UV
tangent where a valid parametrization exists, a deterministic index-chosen
geometric tangent otherwise) and every level above receives its parent's tangent
rotated by the shortest arc onto its own normal.

`examples/68_mesh_multires.py` measures the difference against the
implementation this design rejected: after a coarse form change, the detail's
lean against the surface drifts about **10× further** when the same detail is
carried as a world vector.

### The sculpt level and the display level are independent

The level a brush writes to and the level a host draws are set separately. Move
the broad form at level 1 while watching the pores at level 4 — that is the
workflow, and the reason both exist.

Sculpting at a level writes only that level's persistent data: the cage's own
geometry at level 0, coefficients above it. A higher level's coefficients are
never rewritten merely because its reconstructed positions moved.

### The brushes are the fixed sculptor's

Not "the same behaviour" — the same code. The active level's evaluated positions
live in a `mesh::Mesh` inside that level's cache, with an `Adjacency` over it, so
a multires stamp is `MeshSculptor::stamp` on that level. Every verb, the
surface-aware geodesic reach, the mask gate, automasking, alpha stamping, the
local normal recompute and the BVH refit come along unchanged and unchangeable.
All sixteen verbs are offered, including `Layer` — which an adaptive surface
declines, because there the vertices under the brush at the end of a stroke did
not all exist at the start.

What the multires sculptor owns is the step after: turning the moved positions
back into what the hierarchy stores, and propagating.

**The stroke engine reaches it too.** `brush::apply_to_multires` is the
hierarchy's counterpart to `apply_to_mesh`, and the two share the per-stamp
resolution rather than each having their own — where a stamp lands, how far it
reaches, how hard it presses, and how Grab and Snakehook consume the motion
between stamps are facts about a *stroke* and not about a surface, so they are
one implementation. An adaptive surface is now the only mesh representation
without a stroke entry point.

### What a level costs, and why adding one is priced first

Catmull-Clark multiplies faces by four. Over a 20k-quad cage:

| Level | Faces | Face list (authoritative) |
|---|---|---|
| 0 | 20 k | 320 KB |
| 1 | 80 k | 1.3 MB |
| 2 | 320 k | 5.1 MB |
| 3 | 1.28 M | 20 MB |
| 4 | 5.12 M | 82 MB |
| 5 | 20.5 M | 328 MB |

What is **authoritative** is the cage, the per-level face lists and the per-level
detail. Everything else — subdivided positions, frames, evaluated positions,
normals, per-level connectivity, the level's own mesh and adjacency — is a cache
that rebuilds bit-identically, which is what `memory()` separates and
`drop_inactive_caches()` acts on. Detail is never reported as rebuildable,
because a host under pressure acts on that distinction.

Three residency levers, in the order a host reaches for them:
`drop_inactive_caches()` releases what is above the levels in use;
`drop_intermediate_caches()` releases the levels *between* the cage and them,
which is what a host wants while an artist is detailing at a fine level, since a
stamp there reads that level's own subdivided positions and frames and nothing
else; `drop_all_caches()` releases everything. All three leave the authoritative
detail untouched, which the checksum is what proves.

`preflight_add_level()` prices a level from the level below by arithmetic: it
allocates nothing, has no side effects, and reports both what would remain and
the high-water mark during the call. The estimate is a **ceiling** rather than a
best guess — a budget that errs low says yes to a level that does not fit, which
is the one failure it exists to prevent — and `test_multires.cpp` holds it
against what the level actually costs once it exists. On a device that kills an app for memory
rather than warning it twice, the peak is the number that matters.
`add_level()` refuses over budget rather than allocating half of it, and is
build-then-publish, so a refusal or a cancellation leaves the surface exactly as
it was.

Detail storage is **blocked sparse**: a block of 1024 vertices exists only once
something in it is non-zero. An artist who has detailed a cheek does not pay for
the twenty million vertices that carry nothing.

The block size is a **parameter**, not a constant, and that is what makes the
default defensible: `BM_MultiresDetailBlockSize` sweeps it across three
footprints and reports what each choice costs to hold the same detail. 1024 is
the least at every one — below it the block table over the level dominates,
above it the last partly-used block does. The same discipline moved the
dense-promotion threshold to 1.0: `BM_MultiresDetailAccess` measures 611 M/s
sparse against 605 M/s dense, so promotion buys no measurable speed, while
promoting early costs a third more memory than the sparse form it replaces.

### A dab costs what it touched

A change at a level propagates to the descendants of the vertices it moved and
to nothing else, with a one-ring halo at each level because a vertex that did not
move still has a changed normal when its neighbour did — and a changed normal is
a changed frame. `MultiresEvalStats` reports what an evaluation actually did, so
"propagation is local" is a measurement rather than a claim, and
`test_multires_dirty.cpp` asserts that unrelated base patches come out
**byte-identical** rather than merely close.

A host uploads by **base patch**: a level-0 face owns a subtree that never moves
between faces, so a block's identity is stable for the life of the hierarchy and
no re-partition can invalidate what has already been uploaded. Copying the
display level after every dab is the alternative, and on a deep hierarchy it is
the difference between a preview that keeps up and one that does not.

### Seams

Geometry subdivides over **weld classes**, so a walk crosses a UV seam and the
surface cannot crack. UVs and colours subdivide over a **second connectivity
built from the raw indices**, where a seam is a boundary and the boundary rule
interpolates along it — so a seam is never averaged across itself. That is a
construction rather than a check.

Runnable: [`examples/68_mesh_multires.py`](../examples/68_mesh_multires.py) —
a ridge sculpted at level 3, the form changed at level 1, and the ridge still
there, still attached, with the locality of the propagation and the advantage of
the transported frame both measured.


### Sculpt layers over the hierarchy: a pass you can dial afterwards

The section above makes a pass **survive** an edit beneath it. A sculpt layer
makes it **addressable** afterwards: make a wrinkle pass, come back days later
and dial it to half, hide it, reorder it, delete it — without redoing the work
under it and without replaying a single stroke.

The model is one line, per level:

    E(n) = B(n) + SUM over layers of  s_i * m_i(v) * L_i(n, v)

`B` is the level's own base detail — the **form** — unchanged in meaning, and
still what `detail_checksum` hashes. `L_i` is layer *i*'s coefficients, in the
**same** `DetailField`, in the **same** transported frame, at the **same** block
size. A layer contribution and a base detail coefficient are one quantity under
two owners, so there is no second displacement representation to keep in step
with the first. With an empty stack the composed field is never allocated and a
hierarchy evaluates bit-identically to before layers existed.

#### `MeshBrush::Layer` is a brush; a sculpt layer is a channel

`Layer` means three things in this library and the API is written so the
difference cannot be missed:

| | What it is | Lives for |
|---|---|---|
| `MeshBrush::Layer` | a brush **algorithm** — deposit to a ceiling above the surface as the stroke found it | one stroke |
| `scene::LayerId` | a **document** layer, which the session history keys every step by | the document |
| `SculptLayer` | an artist **channel** — named, reorderable, dialable, stored | the document, addressably |

So nothing is spelled `Layer` unqualified: every type is `SculptLayer*`, every C
entry point is `clay_multires_sculpt_layer_*` — the same prefix the voxel stack
already spends, so the two artist stacks read alike and neither reads like the
brush — and `tools/check_c_abi.py` **gates** the discipline rather than leaving
it to be remembered. It caught this change's own first spelling. Renaming the
brush enumerator was the obvious alternative and was rejected: it is shipped in
the C enum, the Swift enum and every host's serialized preset, so renaming it
would break all three to fix a documentation problem.

#### Strength is composition, not a scale on the pen

This is the behaviour most likely to be reported as a bug. A stroke into a layer
at strength 0.5 records its **full** contribution and moves the surface half as
far as the pen asked for; raising the slider to 1 afterwards **doubles what is on
screen** and replays no stroke. Nothing in the change divides by a strength —
which is also why merge-down and bake are defined by **visual parity** (the
evaluated surface before equals the evaluated surface after) rather than by
concatenating coefficients, an arithmetic that divides by the lower layer's
strength and is undefined at exactly the value one slider reaches.

Strength 0 and invisible contribute **nothing, to the bit**: a layer at zero
effective strength is skipped rather than multiplied by zero.

Parity is the **mask's** as well as the slider's, and that is the half of it
easiest to write and never ask about. `m_i` is a second multiplier, so a merge
has to fold the weight it is removing into the coefficients it writes *and*
clear the mask it folded — one left standing applies itself a second time to a
coefficient that already carries it — and a bake writes the **masked, scaled**
coefficient into a base that has no mask to carry. Both hold, and both are now
gated with two disagreeing masks at strengths 1.0, 0.37 and 0.0, because every
parity case before ran with the identity mask.

#### Reordering is organisation, which is why the sum is taken in id order

Additive displacement commutes, so dragging a pass up or down the list changes
what the list looks like and not where a vertex is — and `move_sculpt_layer`
therefore invalidates **no block**, which is what makes a drag free.

That freedom is only sound because composition sums a block's contributors in
**layer-id** order rather than in list order. Addition commutes; float addition
does not *associate*, so `B + a + b + c` and `B + c + a + b` differ in the last
bit as soon as a stack is three deep or sits on a base detail that is already
there. With a reorder invalidating nothing, list-order accumulation would leave
the blocks a later stroke happened to recompose carrying one order and the
blocks still cached carrying the other — the surface composed two ways at once,
with no operation able to say which. An id is minted once and a reorder never
renumbers it, so ordering the sum on the id makes composition invariant under
exactly the operation the stack promises is free.

#### Three revisions, because one counter cannot say which of three things happened

| | moved by | invalidates |
|---|---|---|
| `metadata_revision` | rename, change of active layer | nothing |
| `composition_revision` | strength, visibility, mask, order, add, remove | the layer's **allocated blocks** |
| `content_revision` | coefficients written | the block written |

A rename must not re-evaluate a model, which keying the cache on a single stack
revision would have made it do. `detail_revision` and `evaluated_revision` fold
in the two that move geometry, so a host written against the multires ABI before
this existed keeps working without learning a new counter.

Both scale claims are **measurements** rather than assertions, because a correct
implementation and a quadratic one produce the same surface and there is no other
way to tell them apart from outside. `SculptLayerStats::blocks_recomposed` after
a strength change is the layer's own coverage and never the level's;
`layer_blocks_visited` is the (block, layer) pairs actually summed, so a stamp on
top of a deep stack can be shown not to sum every layer beneath it over unrelated
geometry — measured flat (1.05x wall clock) from 1 to 128 local layers.

#### The gesture is a transaction, and cancel is exact

`LayeredMultiresSculptor` — `surface.sculpt_layer_stroke()` in pyclay,
`clay_multires_sculpt_layer_stroke_*` in C — is begin / stamp / commit / cancel,
the shape the SDF sculpt transaction already established. Three reasons, none of
which exists until a stack does:

1. **A stroke enters one channel**, fixed at pointer-down and re-asserted per
   dab. A host that changes the active layer mid-stroke must not split one
   gesture across two of them. Under symmetry a mirrored stamp is another stamp
   of the same transaction, so a mirrored stroke is one layer, one delta and the
   union of the two sides' coverage.
2. **A stamp reads the evaluated surface**, which includes every visible layer,
   so the composition is **held** for the length of the stroke: strength,
   visibility, mask, order, add, remove and merge refuse, and rename, lock and
   set-active still work because none of them moves a vertex. Refusing rather
   than deferring is deliberate — a slider that appears to move and then silently
   applies later is the worse surprise.
3. **Cancel has to be exact.** A layered write is `L += dE`, so the only exact
   restore is the recorded `before` values, which is why the record exists from
   the first stamp rather than being reconstructed at the end.

Commit produces **one** delta for the whole gesture, coalesced: a hundred stamps
over one vertex are one entry keeping the first `before` and the last `after`.
In Python the transaction is a context manager, and the asymmetry is the point —
a clean exit commits and **a raising block cancels**, because a half-finished
gesture committed on the way out of an exception is an undo step for work nobody
asked for.

Layer **property** changes are in the history too — rename, strength, visibility,
reorder, lock, add, remove, merge, bake — which is the thing this does better
than the voxel stack, whose renames and strengths are still outside it. An artist
who dials a pass from 100% to 40% and presses undo means the dial.

#### The verbs the split makes possible

Because the hierarchy stores the form and the detail in different arrays, a
smooth can act on either:

| mode | acts on | leaves alone |
|---|---|---|
| `geometry` | positions; exactly the `Smooth` brush | — |
| `detail_only` | coefficients in the target channel | the form, every other layer |
| `preserve_detail` | the **form**, with the detail re-applied unchanged | every layer's contribution, bit for bit |

A plain Laplacian over pores removes the pores, which is rarely what was asked.
`erase` fades the active channel toward zero and can never reach the base;
`restore` fades the level's **own** detail toward the pure subdivision and leaves
every layer standing. Neither is undo, and the distinction is worth stating
because the temptation is to wire one to the other: undo walks a step list
backwards, these move the surface toward a named target under the cursor and are
themselves gestures that undo.

`stamp_detail` deposits a **height map** or a **tangent-space vector
displacement** through the brush's own weight, over the alpha square the mesh
brushes already project and through the same sampler. Vector displacement is
never world-space — the same map on the left and right of a face would make two
different shapes, and across a curve it would shear — so the three channels are
read in the vertex's own transported frame, which is the frame the coefficients
are already stored in. Images are **planar and borrowed**: three channels means
three consecutive `width * height` planes, `(3, H, W)` in numpy, because a plane
is exactly the buffer the alpha sampler reads. A map finer than the level can
carry is **reported** (`oversampling`, `under_resolved`) rather than silently
blurred.

#### Memory is reported and never capped

`memory()` grew two rows: `sculpt_layers` is every layer's coefficients and masks
and is **authoritative**, reported apart from `detail` because the two are the
same quantity under different owners and a host deciding what to merge, bake or
delete needs to see which is costing it; `composed` is the materialized
`B + SUM(s*m*L)` and is rebuildable. A layer costs its **coverage** and not the
model, which is what makes a hundred passes over one cheek affordable.

There is deliberately **no cap**. A cap that silently stopped recording would
leave the pass on the surface and un-dialable, which is a correctness bug wearing
a memory limit's clothes. A host under pressure has four levers instead:
`compact_sculpt_layers()` (the cheapest — it releases every all-zero block a
gesture that undid itself left behind), merge, bake and delete.

`compact_sculpt_layers()` is a memory lever and **not** a change to the picture,
which is a stronger claim than its byte count falling: a lever that ate the pass
would report the same saving. A mask is authoritative rather than rebuildable,
so what compaction may release is all-zero coefficient blocks and nothing else.
It is asserted on the evaluated surface bit for bit twice — straight after the
compaction, and again after every slider has been dialled away and back so that
every covered block has recomposed out of what survived.

The stack is serialized **inside the multires stream**, at surface version 2. The
bump is deliberate rather than incidental: `decode` ignores trailing bytes, so
appending a layer chunk and leaving the version at 1 would let a predating binary
open a layered document, load the base detail only, and present a surface missing
an artist's work with no signal at all. Version 1 still loads here, as what it
was — a hierarchy with no layers. A layer's **kind** is written and an unknown
one is **refused** rather than skipped, for the same reason one level down.

**What the chunk may declare is bounded by the stream around it, and has to be.**
The surface reader rebuilds the cage first, then requires a decoded stack's level
count and every level size to be that hierarchy's — but that comparison happens
*after* the layer decoder has returned, and both of those are numbers the layer
decoder reserves from. So the chunk applies the same two ceilings itself
(`kMaxLevels`, `kMaxLevelVertices`, held equal to the surface's by a
`static_assert`), and a stack's per-block invalidation index is sized when it is
first consulted rather than when a stream names a level. There is one path with
no second opinion at all — a **structural undo record** carries a whole stack
snapshot and is not required to name the surface it was taken against — and on
that path the decoder's own refusals are the only ones there are.

A layer's coefficients and mask must also share the **stack's blocking**, not
merely declare a legal one. Block `b` naming the same 1024 vertices in a layer's
field, in its mask and in the level's composed field is what makes a slider cost
the layer's coverage; the invalidation path hands a field's block numbers to the
stack's index without translating them, so a document pairing a 1024-blocked
stack with a 4-blocked field would mark blocks the level does not have, drop
them, and leave a surface composed from a stack nobody dialled. That is refused
at the door.

Runnable: [`examples/69_mesh_sculpt_layers.py`](../examples/69_mesh_sculpt_layers.py)
— a wrinkle pass dialled 0 -> 50% -> 100% over a form that never changes, one
layer removed with the others byte-identical, what a slider costs measured
against the level, and a stroke loop that raises leaving nothing behind.


## 8c. Voxel remesh — throwing the topology away on purpose

Section 8 preserves topology and section 8b adapts it locally. This replaces all
of it at once, which is what sculpting applications call **DynaMesh** or **Voxel
Remesh**.

`mesh::voxel_remesh` samples a whole surface into a signed narrow-band field at
a spatial resolution the caller chooses and reconstructs a new isosurface from
it. Nothing about the input's topology survives, and that is the feature: after
stretching, kitbashing or a long adaptive session, the density has to be rebuilt
rather than repaired.

### What it is for, and what it is not

| | |
|---|---|
| Overlapping shells | **Fuse.** The field knows where material is, not which mesh claimed it |
| Self-intersections | Resolve volumetrically |
| Stretched triangles | Gone — no edge of the result spans more than one lattice cell |
| Uneven density | Replaced by approximately uniform spacing at the chosen voxel size |
| Vertex / polygon identity | **Destroyed.** No index maps an input to an output |
| UVs | **Dropped**, not reprojected — see below |
| Detail finer than the voxel size | May disappear, and the estimate warns before it does |
| Vertex colour | **Survives**, resampled by closest point |
| A caller's per-vertex mask | Survives, through `mesh::transfer_vertex_scalar` |

It is **not** quad retopology. The output is a lattice-derived triangulation
with no edge loops following the form, and no setting changes that. It is not
decimation, which preserves the surface's own triangles. It is not the local
remesher of section 8b, which adapts under a brush and keeps everything else.

### Resolution is a physical size

The canonical control is a **world voxel size**, because that is what decides
which features survive and it means the same thing whatever the model's size. A
longest-axis integer is offered as the convenience an artist actually turns and
maps onto it — the longest bounding extent divided by that integer, resolved
before any sampling padding, so the number does not drift when the padding does.
Both the estimate and the report carry the resolved voxel size.

### The cost is preflighted, and a refusal is typed

`mesh::voxel_remesh_estimate` walks the source's triangles and marks the brick
lattice — no tree, no field, no mesh — and reports the resolved voxel size, the
grid dimensions, an upper bound on the narrow band's samples, the working
memory, a triangle range, the source's open-boundary and component counts, and
whether the source carries material thin enough to be lost. It is cheap enough
for a resolution slider.

A request over the caller's budget, or over the library's own ceilings, fails
with `ExceedsBudget` **before** the field, the tree or the result is allocated.
The library does not lower a resolution it was asked for: an engine that quietly
halves a request produces a result the artist did not ask for and cannot
explain. Fitting a resolution to a budget is a host policy built out of repeated
estimates, and the estimate exists so that policy is cheap.

Every other refusal is its own status too — `InvalidResolution`,
`OpenSurfaceRejected`, `ResultNotWatertight`, `Cancelled`, `EmptySource`,
`Unsupported` — because "lower the resolution", "your model has holes" and "you
stopped it" are three different things for a host to say.

### An open surface takes an explicit policy

`Reject` fails and says why. `Close` produces the closed volumetric
interpretation and validates it. `BestEffort` proceeds and reports what the
result actually is. An open source is **never** silently treated as though it
had been watertight, and the report carries the source's boundary-edge count
whatever the policy chose.

### The sampling follows the surface, not the bounding box

This is the one piece of new engineering in the feature, and it is why the
existing mesh-to-field converter is called rather than reused wholesale.
`FieldVolume::sample_parallel` evaluates the caller's function for **every**
brick of the region — 32³ bricks of 729 samples at longest-axis 256, each sample
a BVH distance query carrying a generalized winding number, and 128³ bricks at
1024. That is right for an import, where the caller chose the cell size for the
model, and wrong for a resolution dial.

So the remesh supplies its own brick fill to the same `sample_blocks` entry
point: bricks near a source triangle are evaluated, and the rest are filled with
the sign of the connected region they fall in, one winding query per region.
Within an evaluated brick the distances come first and the signs second, because
a brick whose nearest sample is further than the band cannot contain the surface
— the samples are a voxel apart and the band is three — so its sign is constant
and one query answers for all 729.

**The samples that are stored are bit-identical to what a dense evaluation would
have stored.** A brick holding a sample within the band of the surface is
necessarily within the band of some triangle, and so is necessarily marked.
Sparsity here is an optimisation of one field, not a second field, and
`test_voxel_remesh.cpp` marches the dense field on the same lattice and requires
the same mesh byte for byte.

The one place the two differ is a source with **open boundaries**: the
generalized winding number's half-crossing can fall away from every triangle, so
a brick can straddle it while holding no sample near one. The dense path records
that brick's sign per brick and this records it per region. Only sample-free
bricks are affected — neither path stores anything there — and it is stated in
the header rather than left to be found.

### Projection is clamped by distance and weighted by facing

After extraction each vertex may move part of the way toward the closest point
on the source, which is what gives a rebuild back the detail the lattice rounded
off. It is clamped by distance, and scaled by how well the source surface there
faces the same way the vertex does — to zero where it faces away.

**A weight and not a rejection, and that is a measured correction.** The first
version rejected outright and the comment asserted the danger confidently. The
fixture that actually reaches the branch is a sheet folded back through itself,
where about a fifth of the vertices inside the clamp have a back-facing closest
point — and there, at longest-axis 96, the hard reject was the *only* variant
that made the surface worse:

| | distance to source | self-intersecting pairs |
|---|---|---|
| no projection | 0.38643 | 0 |
| project, no facing test | 0.38617 | 0 |
| project, hard reject | 0.38637 | **17** |
| project, weighted | 0.38642 | 0 |

The mechanism is plain once seen: moving a vertex fully while leaving its
neighbour untouched is a discontinuous displacement, and a discontinuous
displacement tears. The weight goes to zero continuously and does not. The
strength is a lerp and never a snap either way, so a mis-clamped projection
degrades toward "no projection" rather than toward "corrupted".

### Determinism, cancellation and the document

The same source and parameters produce a **bit-identical** mesh on every run.
That is a property of the decomposition rather than of single-threading: the
brick fill, the marching waves, the projection and the transfer all write
disjoint outputs computed from position-only inputs, so no scheduling can
reorder a value into a different one.

`parallel::CancelToken` stops it, and the check is inside every expensive stage
rather than between them. A cancelled remesh returns `Cancelled` with no mesh;
the source is a `const&` and was never written, so "cancellation leaves the
source unchanged" is a property of the signature rather than a promise about a
rollback.

### Feeding the result to an adaptive surface

`DynamicSurface::from_mesh` refuses a mesh with a face whose corners coincide,
and a marched mesh has them — the default mesher emits about two per cent
zero-area triangles, and `validate` calls them slivers rather than degenerates,
so nothing had ever objected. **Weld first**: `mesh::weld` merges the coincident
vertices, drops the triangles that collapses, and hands the conversion something
it can express. Weld at *at least* the epsilon the conversion will use — welding
below it only moves the problem.

It is a **pure mesh → mesh operation**: no document, no layer, no history and no
revision token. A host holds the before and after meshes and commits them as one
undo record. A `DynamicSurface` round-trips through it the same way — `to_mesh`,
remesh, `from_mesh` — and the boundary is the caller's to see rather than an
overload's to hide.

### On a layer, as one undo step

`mesh::voxel_remesh` has no document. `clay_document_voxel_remesh_layer` (and
`Document.voxel_remesh_layer` in pyclay) is the other half: it captures the
layer's triangles, rebuilds, validates, replaces and records — one call, one
step on the undo menu, and transactional, so a refusal, a validation failure or
a cancel leaves the layer byte-identical and adds no step. A protected layer is
refused *before* the rebuild rather than after several seconds of it.

The undo record is `session::Step::Kind::MeshReplace`, holding the mesh on each
side. A sparse `VertexDeltas` cannot express this and must not be asked to: a
delta records no indices — the fixed-topology contract working as intended — so
one recorded against the old geometry applied to the new is a corruption, not an
undo. It is by a wide margin the largest step kind there is, which is why the
history's byte accounting had to learn it.

**A mesh layer carries a geometry revision**, and the asymmetry is the whole
point: a wholesale replacement bumps it, a sculpt does not. A brush moves
vertices and leaves `indices` byte-identical, which is exactly what lets an
`Adjacency`, a `Bvh` and a live `MeshSculptor` stay valid across a stroke; a
rebuild invalidates all three.

It does two jobs. A host running the rebuild on a worker thread reads the
revision first and hands it back to the commit, so a result that arrives after
the artist has moved on is **refused** rather than silently winning. And a live
sculptor compares it, which catches the case neither of the older checks could:
the layer's mesh POINTER is stable across an assignment, and the sculptor's
vertex and index COUNTS are unchanged by a replacement that happens to land on
the same ones — so before this, a same-count rebuild left a sculptor stamping
into an adjacency and a BVH describing triangles that no longer existed.

Runnable: [`examples/67_voxel_remesh.py`](../examples/67_voxel_remesh.py) —
spikes thinner than a coarse voxel disappearing, a stretched surface's edge
lengths before and after, and two crossing shells cut open to show the interior
wall that fusion removes.

---

## 8a. The brush model — how this vocabulary is organised

Read this before the ZBrush map below, because it is the answer to the question
that map raises: if ClayCore has sixteen verbs and ZBrush has dozens of named
brushes, where do the rest come from?

**They are not deformations.** Clay Buildup, Dam Standard, hPolish, Trim
Dynamic, Snake Hook and Rake are each a kernel plus a falloff plus a frame plus
an accumulation rule plus a spacing. Naming those axes separately is what turns
a named brush into a PRESET instead of an engine path, and it is what lets the
next one cost a serialized struct rather than a switch case.

### The axes

| Axis | Values | What it decides |
|---|---|---|
| **Footprint** | Ball, SurfaceWalk | How the region is REACHED — in a straight line, or along the surface |
| **Falloff** | Constant, Linear, Smooth, Gaussian | How the weight decays across it |
| **Frame** | None, RegionNormal, VertexNormal, StrokeDirection, RegionPlane | The direction a kernel displaces along |
| **Kernel** | Translate, Displace, Gather, Tangential, Plane, PlaneDeposit, CutAndGather, Laplacian, DepositCeiling, ColorBlend, ColorAdvect | The deformation itself |
| **Write target** | Position, Color | Which buffer it writes |
| **Post policy** | None, RecomputeNormals | What has to happen afterwards |

`mesh::model_of(verb)` is the decomposition table, and it is the requirement's
own test: **a verb that could not be written as a row in it would be evidence
an axis is missing**, and the axis is what should be added rather than a
seventeenth verb.

Two readings of that table are worth having in front of you:

- **Draw and inflate are ONE kernel under two frames.** They were two verbs
  whose only documented difference was the direction each takes — the region's
  averaged normal, or each vertex's own — so naming the direction made that the
  entire difference. Their results did not move by a bit when they were merged.
- **Grab and snakehook are the same row.** One stamp of snakehook IS a grab;
  what makes it a snakehook is the re-anchoring BETWEEN stamps, which is a fact
  about a stroke rather than about a brush.

### Where the axes live, and why not in `brush`

`mesh/brush_model.h`, not `brush/`. `tools/check_layering.py` records
`brush -> mesh` — `brush::apply_to_mesh` is the stroke engine's fourth
consumer — so `mesh` may not include `brush`, and the per-vertex loop that has
to read the axes is `MeshSculptor::stamp`. `brush::BrushPreset`, which pairs
the axes with a `StrokePreset`, does live in `brush`: it is the one module that
can see both vocabularies, for the same reason `apply_to_mesh` is the only
place a `MaskField` becomes a `field::MaskGate`.

### The named brushes, as data

`brush::reference_presets()` is the library, and every entry is axis values over
existing kernels:

| Preset | Is | Differs from its neighbour by |
|---|---|---|
| Standard | Draw | — |
| Clay | Clay | — |
| Clay Buildup | Clay | the STROKE alone: denser spacing, buildup accumulation |
| Clay Strips | Clay | a constant falloff, and the caller's alpha |
| Move | Grab, ball footprint | — |
| Move Topological | Grab, surface walk | ONE axis: the footprint |
| Snake Hook | Snakehook | the stroke re-anchors per stamp |
| Dam Standard | Crease | a tighter spacing |
| hPolish | Polish | a tight gate angle, two passes |
| Trim Dynamic | Flatten | `flatten_mode` cut-only |
| Flatten | Flatten | two-sided |
| Rake | Draw | the stroke follows the stylus BARREL |

A preset carries a schema version from v1, refuses a newer one rather than
reading a prefix of it, and **carries no image bytes**: an alpha stays
caller-owned and borrowed for the call, so a preset is a couple of hundred
bytes and a host owns its own resource cache.

### Automasking

The gates a brush applies to itself — normal angle, topology connectivity,
boundary proximity, cavity, surface group — are composed into the per-vertex
weight **by multiplication**, never branched into each verb. Sixteen verbs times
five factors is eighty places to get a gate wrong; one multiplication is one.

The weight's factors compose in one fixed order, and the order is a contract
rather than an implementation detail, because float multiplication is not
associative:

```
falloff -> path taper -> (1 - mask gate) -> alpha -> automask
```

The automask is applied LAST specifically so that a stamp with none of them
multiplies by an exact 1.0 and lands on the bits it landed on before
automasking existed.

Two of the five factors — cavity and surface group — are not computed in `mesh`
and cannot be: cavity is a field's Laplacian (`brush::measure_at`) and groups
are a world lattice (`voxel::GroupField`), and both modules depend on `mesh`.
They arrive as callbacks through `brush::MeshStrokeOptions`. That constraint is
load-bearing rather than an inconvenience: the requirement is that a painted
cavity mask and a cavity automask cannot disagree about one surface, and `mesh`
structurally cannot write a second estimator to disagree with.

Runnable: [`examples/65_brush_presets.py`](../examples/65_brush_presets.py) —
one gesture through five presets, with every claim above asserted rather than
illustrated.

---

## 9. ZBrush equivalents

Where a ZBrush brush maps onto the list above. This is a map, not a claim of
parity — the mechanism usually differs even where the result matches.

| ZBrush | claycore | Note |
|---|---|---|
| Standard | `Op::Relief` | Displaces the accumulated surface along its normal |
| ClayBuildup | `Op::Relief` along a stroke | Buildup accumulation scales each stamp's amplitude, so overlapping stamps deposit twice |
| Crease, DamStandard | `Op::Incise` | The same op, cutting in — a thin region gives the line |
| Inflate | `Op::Relief`, `sculpt_inflate` | Moving the surface along its own normal *is* relief; the voxel verb dilates and erodes by cells |
| Move Topological | `field::move_topological` | Geodesic falloff — the radius is travel across the surface, so it cannot step over a gap. Bakes |
| Move | `brush::move_brush` | Drags the assembled surface. Nudges form rather than growing it: a large pull buds rather than stretches. Drags that OVERLAP compound the step scale — use `snakehook` to pull a lobe out — but disjoint ones no longer do (see below) |
| Rotate | `pose` / `pose_line` | Radial, or ramped along a line |
| Gizmo Twist | `twist_range` | The gizmo acts inside its box: the rotation ramps across the span and holds beyond. Plain `twist` winds the whole item, which is the difference |
| Gizmo Bend Arc | `bend_range` | Angle-limited bend, same shape |
| Gizmo Bend Curve | `bend_curve` | A bend along an arbitrary guide. Implemented as the INVERSE of the swept primitive — the same nearest-point query and transported frames, read from the other end — so the two agree about what a guide is by construction |
| Gizmo Lattice / FFD | `mesh::Lattice` on a mesh layer, `Deformer::lattice` on an SDF item, `brush::lattice_gizmo` over a whole LAYER | **Both forms, and they are not the same map.** On a mesh it runs FORWARD and is exact — which is what ZBrush and Blender do, because a mesh knows where its vertices are. On an SDF item forward FFD has no closed-form inverse, so the cage is authored AS the inverse: closed-form and portable, but not the exact inverse of the forward map. The two differ by a term proportional to how the basis varies along the displacement — measured at under 1.5% of the drag (`examples/50_sdf_lattice.py`), and SIGNED rather than always-less, so it does not inherit `grab`'s character. Divisions are capped at 4 per axis on the SDF side against 32 on the mesh side, because that one runs per SAMPLE rather than per vertex. A gizmo acts on the whole subtool, so `brush::lattice_gizmo` resolves one world-placed cage into a per-item lattice each carrying the transform into the cage's frame — exact for ROTATED items, which no axis-aligned per-item box could express, and reaching EVERY item because a lattice's displacement outside its box is clamped rather than zero |
| Pinch | `magnify` (negative), `clay_layer_magnify_surface`, `sculpt_pinch` | One signed strength, not two verbs |
| Magnify | `magnify` (positive), `clay_layer_magnify_surface`, `sculpt_magnify` | Maxon's own page calls them inverses |
| Smooth | `field::relax`, `sculpt_smooth` | Bakes on the SDF side |
| Flatten | `field::flatten` (two-sided), `sculpt_flatten` | Region required on the SDF side |
| hPolish, Planar, Trim | `field::flatten` in **cut-only** mode | Planes down without filling, which is what keeps the facet crisp |
| Trim (Rect/Circle/Lasso) | `cut::cut_item` | The practitioners' "90% tool" |
| Trim Curve | `CutShape::from_open_curve` | An OPEN stroke closed against the frame bounds — not the lasso constructor, which closes the stroke and cuts a sliver |
| Clip | `cut::cut_item` | **As a solid, Clip is exactly Trim.** Clip's distinctive look is a zero-thickness fin a field cannot represent and users delete anyway |
| ZSpheres | `Prim::armature` | A tree of spheres; `blend_k` is the skin. No per-node rotation — a sphere is isotropic, and no mesher here consults a node frame |
| SnakeHook | `brush::snakehook` | Adds material rather than pulling it |
| Surface Noise | `noise` deformer | Integer hash, so all four backends agree |
| Mask | mask fields, layer lock/ghost | Painted by the stroke engine like any brush; freezes every verb on both representations; survives resolution changes |
| Extract | `brush::mask_extrude` | The mask is the region — no radius to supply. Outward, inward or centred, with a roundable rim |
| Alphas | `sculpt_carve_alpha` on voxels, `Deformer::alpha` on SDF items | **Both representations.** On an SDF item an alpha is a DEFORMER rather than a primitive — an item shaped like the stamp would ADD material in the stamp's shape, where an alpha modulates a surface already there, so it is a distance offset under the same radial falloff `blob`, `grab` and `magnify` use. The engine decodes no images either way: a host hands over the samples. What it costs is honest and visible — the Lipschitz bound comes from the largest difference between ADJACENT samples over the world distance between them, so a flat stamp costs nothing for having large values and a high-frequency one lowers the step scale (`examples/53_sdf_alphas.py` measures both). White is raised, as everywhere else |
| Blob | `blob` | Noise with the finite support `grab` and `magnify` have — outside the radius the field is untouched. One SIGNED amplitude, so a single dab both swells and eats in, which is what reads as blobby rather than as a uniform bulge |
| Slice / Knife (polygroup splits) | — | Splitting without removing volume has no single-solid equivalent; it needs two items |
| ZRemesher (quad retopology) | partly — `mesh_quads` | **Not the same thing, and the difference matters.** claycore meshes a sculpt into a QUAD GRID DERIVED FROM ITS LATTICE: quad-only, regular, no T-junctions, with a target count you choose — enough to hand a form to a DCC as OBJ or FBX. What it is not is field-aligned: no edge loops following the form, no poles placed at features, density that does not follow curvature. A retopology pass REPLACES this rather than refining it |
| Surface-mode mesh brushes | `mesh::MeshSculptor` (§ 8) | **The non-goal was narrowed, not dropped.** Vertices move on a mesh layer's own triangles; dyntopo, multires and remeshing remain out of scope |
| Dynamic tessellation (Dyntopo, LiveClay) | — | **Absent today, and no longer a non-goal.** It was out of scope on the grounds that an SDF sidesteps topology entirely — which stayed true and stopped being the whole answer once fixed-topology brushes shipped and a stretched snakehook became a reason to leave the engine. Scoped as `add-dynamic-topology`, `openspec/ROADMAP.md` Phase 5 |

---

## 10. Where each is reachable

Names differ between bindings, so this lists them rather than ticking boxes.

| Verb | C++ | `pyclay` | C ABI |
|---|---|---|---|
| Combine ops | `scene::Op` | `clay.Op.*` | `CLAY_OP_*` |
| Blends | `scene::Blend`, `BlendProfile` | `clay.Smooth/Cubic/Circular/Chamfer(k)` | `clay_item_set_blend`, `CLAY_BLEND_*` |
| Deformers | `scene::Deformer::twist(...)` etc. | methods on the prim: `p.twist(...)`, `p.noise(...)`, `p.magnify(...)` | `clay_item_add_deformer` |
| Ranged twist / bend | `Deformer::twist_range`, `bend_range` | `p.twist_range(...)`, `p.bend_range(...)` | `CLAY_DEFORM_TWIST_RANGE`, `CLAY_DEFORM_BEND_RANGE` |
| Bend along a curve | `Deformer::bend_curve` | `p.bend_curve(...)` | `clay_item_add_bend_curve` — its own entry point, because a guide is not a fixed number of floats |
| Stroke engine | `brush::resolve_stroke`, `StrokePreset` | `clay.StrokePreset`, `layer.apply_stroke(...)` | `clay_stroke_resolve`, `clay_stroke_preset_*`, `clay_layer_apply_stroke`, `clay_voxel_apply_stroke` |
| Smooth — `relax` on SDF layers | `field::relax`, `VoxelGrid::sculpt_smooth` | `Volume.relaxed(...)`, `VoxelGrid.sculpt_smooth(...)` | `clay_item_volume_relax`, `clay_item_volume_relax_from`, `clay_voxel_sculpt_smooth` |
| Flatten | `field::flatten` | `Volume.flattened(...)`, `Volume.flattened_from(...)` | `clay_item_volume_flatten`, `clay_item_volume_flatten_from` |
| Cut tool | `cut::cut_item`, `cut::CutShape` | `clay.Cut(...)`, `clay.CutShape.rect/circle/from_polygon/from_curve` | `clay_cut_create`, `clay_cut_polygon_from_curve` |
| Snakehook | `brush::snakehook` | `clay.snakehook(...)` | `clay_item_create` + `clay_item_set_curve_points` |
| Tube | `brush::tube` | `clay.tube(...)` | `clay_tube_create` |
| Voxel verbs | `VoxelGrid::sculpt_*` | `VoxelGrid.sculpt_*` | `clay_voxel_sculpt_*` |
| Did an edit change anything | `VoxelGrid::change_count()` | `VoxelGrid.change_count` | `clay_voxel_change_count` |
| Sculpt layers (voxel) | `VoxelGrid::begin_sculpt_layer` / `end_sculpt_layer`, `set_sculpt_layer_strength`, `move_sculpt_layer`, `merge_sculpt_layer_down` | `with grid.sculpt_layer(name):`, `grid.set_sculpt_layer_strength(...)` | `clay_voxel_begin_sculpt_layer`, `clay_voxel_end_sculpt_layer`, `clay_voxel_set_sculpt_layer_strength`, `clay_voxel_move_sculpt_layer`, `clay_voxel_merge_sculpt_layer_down` |
| Move brush | `brush::move_brush`, `moved_chain` | `Layer.move_surface(...)`, `.move_surface_preview(...)` | `clay_layer_move_surface`, `clay_layer_move_surface_preview` |
| Move Topological | `field::move_topological` | `Volume.moved_topologically_from(...)` | `clay_item_volume_move_topological` |
| Deformers on a placed node | `scene::SetDeformersCmd` | (through `move_surface`) | `clay_layer_add_deformer` |
| Masks | `voxel::MaskField` | `clay.MaskField` | `clay_mask_*` |
| Mask brush | `brush::apply_to_mask` | `MaskField.apply_stroke(...)` | `clay_mask_apply_stroke` |
| Bounded complement | `MaskField::fill`, `invert_within` | `MaskField.fill/.invert_within` | `clay_mask_fill`, `clay_mask_invert_within` |
| Mask as a distance | `brush::mask_to_field` | `MaskField.to_field(...)` | `clay_mask_to_field` |
| Mask extrude | `brush::mask_extrude` | `Document.mask_extrude(...)`, `VoxelGrid.mask_extrude(...)` | `clay_document_mask_extrude`, `clay_voxel_mask_extrude` |
| Armatures | `Prim::armature`, `scene::armature_*` | `clay.Armature`, `Layer.armature_edit(...)` | `clay_layer_armature_edit`, `clay_item_set_armature_parents` |
| Consolidate a layer | `scene::consolidate_layer` | `Layer.consolidate(...)`, `.consolidation_cost(...)` | `clay_layer_consolidate`, `clay_layer_consolidation_cost`, `clay_layer_consolidation_state` |
| What a layer's field costs | `scene::field_report` | `Layer.field_report()` | `clay_layer_field_report` |
| Voxel resolution levels | `VoxelGrid::add_level` etc. | `VoxelGrid.add_level(...)`, `.set_active_level(...)` | `clay_voxel_add_level`, `clay_voxel_set_active_level`, `clay_voxel_drop_level` |
| Groups a host builds | `scene::Node::is_group` | `Layer.add_group(...)` | `clay_layer_add_group`, `clay_layer_add_item_in_group`, `clay_item_add_child`, `clay_layer_children`, `clay_layer_node_count`, `clay_layer_node_at` |
| What a placed node holds | `scene::Node::xform/prim/op/blend` | — (pyclay follow-up) | `clay_layer_node_transform`, `clay_layer_node_params`, `clay_layer_node_op_blend` |
| A per-axis scale on an item | `scene::Node::scale_axes`, `cscale_nu_*` | `scale=(sx, sy, sz)` on every placement | `clay_item_set_scale_nonuniform`, `clay_layer_set_transform_nonuniform`, `clay_layer_node_transform_nonuniform`, `clay_mesh_transform_nonuniform` |
| Quad meshing, with a target count | `mesh::mesh_tape_quads`, `mesh_tape_quads_fit`, `VoxelGrid::mesh_quads` | `Document.mesh_quads(...)`, `VoxelGrid.mesh_quads(...)` | `clay_document_mesh_quads`, `clay_voxel_mesh_quads` |
| Triangles straight to voxels | `VoxelGrid::rasterize_mesh` | `VoxelGrid.rasterize_mesh(...)` | `clay_voxel_rasterize_mesh` |
| A mesh a document carries | `scene::LayerKind::Mesh` | `Document.add_mesh_layer(...)`, `.mesh_layer(...)` | `clay_document_add_mesh_layer`, `clay_document_mesh_layer`, `clay_document_mesh_layer_by_id`, `clay_mesh_layer` |
| Fixed-topology mesh brushes | `mesh::MeshSculptor::stamp`, `mesh::MeshBrush` | `MeshSculptor.stamp(...)` | `clay_mesh_sculptor_stamp`, `CLAY_MESH_BRUSH_*` |
| Lattice cage on a mesh layer | `mesh::Lattice`, `mesh::MeshSculptor::apply_lattice` | `Lattice(...)`, `MeshSculptor.lattice(...)` | `clay_mesh_lattice_*`, `clay_mesh_sculptor_lattice` |
| Lattice cage on an SDF item | `scene::Deformer::lattice` | `p.lattice(...)` | `clay_item_add_lattice`, `CLAY_DEFORM_LATTICE` |
| One cage over a LAYER (the gizmo) | `brush::lattice_gizmo`, `brush::caged_chain` | `Layer.lattice_gizmo(...)` | `clay_layer_lattice_gizmo`, `clay_layer_lattice_gizmo_preview` |
| A mesh stroke | `brush::apply_to_mesh` | `MeshSculptor.apply_stroke(...)` | `clay_mesh_sculptor_apply_stroke` |
| Mesh vertex adjacency | `mesh::Adjacency` | `MeshSculptor.class_count` | `clay_mesh_sculptor_class_count` |
| Mesh stroke undo | `mesh::VertexDeltas` | `clay.VertexDeltas` | `clay_mesh_deltas_*` |
| Picking a mesh layer | `pick::raycast_mesh`, `mesh::Bvh::raycast` | `MeshSculptor.raycast(...)` | `clay_mesh_sculptor_raycast` |
| Serializing without a path | `io::save_clayspace`, `save_obj/ply/fbx/glb` | `Document.to_bytes()`, `Mesh.to_bytes(fmt)`, `clay.load_bytes`, `clay.load_mesh_bytes` | `clay_document_save_memory`, `clay_mesh_save_memory`, `clay_*_load_memory`, `clay_blob_*` |
| What a mesh's quality actually is | `mesh::validate`, `signed_volume`, `surface_area` | `Mesh.validation_report(...)`, `.signed_volume`, `.surface_area` | `clay_mesh_validation_report`, `clay_mesh_measure` |

Snakehook has no dedicated C entry point on purpose: it is a **resolver** that
produces an ordinary stroke item, and the C ABI already builds those. A separate
call would be a second way to say the same thing.

Parity between `pyclay` and the C ABI is enforced in CI by
`tools/check_binding_parity.py`, which fails on a Python capability with no C
counterpart **and** on an exemption that has gone stale.

---

## 11. The same verb on three representations

§ 10 answers "which binding reaches this". This one answers a different
question: **the same verb often exists on more than one representation, and the
versions are not the same operation.** They differ in what they cost, what they
preserve, and whether the gesture survives as something you can revisit.

The README carries a shortened form of this table. This is the complete one.

### Why a verb differs across representations at all

Three structural facts drive every row below, and none of them is a quality
difference:

1. **An SDF edit is a node in a list; a voxel edit is a cell write; a mesh edit
   is a vertex move.** So an SDF gesture stays editable, a voxel gesture does
   not, and a mesh gesture is undoable but is not an edit list.
2. **The baked field operations bake.** `field::relax`, `field::flatten` and
   `field::move_topological` sample the assembled field into a volume. Chained
   bakes steepen the field — each one's output is a slightly worse distance
   estimate than its input — until `consolidate_layer` redistances it. Voxel and
   mesh verbs have no equivalent decay: a cell write is a cell write and a vertex
   move is a vertex move, however many you do.
3. **Only the SDF side composes.** `compile_document` chains visible SDF layers;
   a voxel layer and a mesh layer are not in the tape. So any verb whose result
   must be booleaned, blended or reordered afterwards wants to be on the SDF
   side, whatever it costs there.

### Verbs that exist on more than one representation

| Verb | SDF | Voxel | Mesh | The difference that matters |
|---|---|---|---|---|
| **Smooth** | `field::relax` | `VoxelGrid::sculpt_smooth` | `MeshBrush::Smooth` | SDF **bakes and steepens**; voxel is a majority filter over the 26-neighbourhood; mesh is a one-ring Laplacian weighted by falloff. Only the SDF version has a cost that accumulates across passes |
| **Flatten** | `field::flatten` — `TwoSided` / `CutOnly` / `FillOnly` | `sculpt_flatten` — two-sided only | `MeshBrush::Flatten` — the same three modes | Cut-only *is* hPolish, and the voxel side does not have it. The SDF version **requires a region**; the brush versions take the plane from the stamp |
| **Inflate** | `Op::Relief` | `sculpt_inflate` | `MeshBrush::Inflate` | Relief displaces the accumulated surface along its own normal; the voxel verb dilates or erodes occupancy `\|amount\|` times; the mesh verb moves each vertex along **its own** normal — which is exactly what distinguishes it from `Draw`, whose direction is shared per stamp |
| **Pinch** | `magnify` with negative strength, resolved against the surface by `clay_layer_magnify_surface` | `sculpt_pinch` | `MeshBrush::Pinch` | One signed strength on the SDF and mesh sides rather than two verbs. The voxel pair are separate entry points but share one walk so they cannot drift. Mesh pinch is **tangential** — it gathers within the surface rather than moving it along a normal |
| **Magnify** | `magnify`, positive, resolved against the surface by `clay_layer_magnify_surface` | `sculpt_magnify` | — (use `Pinch` negative) | Maxon's own documentation calls pinch and magnify inverses, which is why they are one signed parameter here |
| **Grab** | `Deformer::grab` | `sculpt_grab`, and `VoxelGrid.grab` for a drag | `MeshBrush::Grab` | The voxel verb uses **the same inverse map** as the SDF deformer, deliberately, so both mean the same thing. But voxel resampling is nearest-cell and rounds **per axis**: a drag under half a cell on every axis moves nothing. Accumulating past `voxel_size` and emitting is **not** the fix — see below. The SDF deformer acts on **one item's own field**, not the assembled surface |
| **Move** | `brush::move_brush`; `field::move_topological` | — | `MeshBrush::Grab` | `move_brush` drags the **assembled** surface, which no other representation offers, and `move_topological` weights by distance *along the material* so a part close in space but far across the surface does not follow. There is no voxel or mesh Move Topological |
| **Scrape** | — | `sculpt_scrape` | `MeshBrush::Scrape` | Both are flatten **and** smooth taken from *one* snapshot. Calling the two in sequence is a different result, which is why it is a verb rather than a recipe |
| **Smudge / Nudge** | — | `sculpt_smudge` | `MeshBrush::Nudge` | `sculpt_smudge` drags **surface** material and leaves the interior — grab moves a lump, smudge smears a skin. `Nudge` slides the region along per-vertex tangent planes, where `Grab` carries it rigidly |
| **Snakehook** | `brush::snakehook` | — | `MeshBrush::Snakehook` | The SDF resolver **adds material** — it produces an ordinary stroke item. The mesh brush re-anchors grab on the class it drags and stretches the triangles it already has, to the extreme. Same gesture, opposite relationship to topology |
| **Crease / DamStandard** | `Op::Incise` | **deliberately absent** — see § 7 | `MeshBrush::Crease` | On a lattice it is a recipe rather than a verb. The mesh version is a tight negative draw **and** a pinch in one stamp; sequenced separately they leave a rounded ditch |
| **Clay / ClayBuildup** | `Op::Relief` along a stroke, with buildup accumulation | — | `MeshBrush::Clay` | Buildup scales each stamp's amplitude so overlapping stamps accumulate. The mesh `Clay` clamps draw's deposit to a plane floating at the stamp height |
| **Polish** | `field::flatten` in cut-only mode | — | `MeshBrush::Polish` | The mesh version is smoothing **gated by the dihedral angle** — full strength where normals agree, fading out where the surface bends — which is what keeps corners up. The SDF version planes down without filling |
| **Colour** | per item, plus `Op::Paint` regions | per cell, via the palette | `MeshBrush::Paint` / `Smear` | The mesh verbs are the only two brushes in the set that move no vertex. Both refuse a mesh with no colour attribute rather than creating one |
| **Alphas** | `Deformer::alpha` | `sculpt_carve_alpha` | — | On an SDF item an alpha is a **deformer**, not a primitive: an item shaped like the stamp would add material in the stamp's shape, where an alpha modulates a surface already there. The engine decodes no images on either side — a host hands over samples |
| **Mask extrude** | `brush::mask_extrude` | `VoxelGrid::mask_extrude` | — | The mask is the region, so there is no radius. The SDF result is an ordinary item and stays an operand |
| **Taper / twist** | `Deformer::taper`, `twist`, and the ranged forms | — | `MeshSculptor::deform` | The SDF deformers run **backwards** — they answer "where did the material at this point come from". The mesh versions are **forward** point maps, which is both easier and exact, so a tapered mesh and a tapered field are the same shape. There is deliberately no mesh `bend`: its map folds distinct points together past a gentle angle, so no forward map exists |
| **Lattice / FFD** | `Deformer::lattice` on an item, `lattice_transformed` through a frame; `brush::lattice_gizmo` over a whole layer | — | `mesh::Lattice`, `MeshSculptor::apply_lattice` | **Direction.** The mesh cage runs FORWARD — `p + Bernstein(offsets, clamp(s,t,u))` — exact, once per vertex, up to 32 divisions an axis. An SDF deformer must run backwards and forward FFD has no closed-form inverse, so the SDF cage authors its offsets AS the inverse warp: not the exact inverse, differing by a term that over-travels a drag toward rising basis weight and under-travels one pointing away, measured under 1.5% of the drag. It costs `nx*ny*nz` multiply-adds **per sample inside the raymarcher**, hence 4 divisions an axis rather than 32. Both store offsets, so an untouched cage is exactly the identity, and both hold rigidly beyond the box |
| **Masking** | gates any **operation**, a boolean included | freezes cells against every verb | gates every brush | The SDF gate rides the combine record rather than being a mode, which is why it protects from a boolean the same way it protects from a brush |
| **Strokes** | `stamps_to_nodes` | `apply_to_grid` | `apply_to_mesh` | One resolved stroke, four consumers (the fourth is `apply_to_mask`). Spacing, pressure, jitter and taper mean the same thing on all of them |

### Verbs that exist on exactly one, and why

| Verb | Only on | Why it cannot be elsewhere |
|---|---|---|
| Booleans, blends, the 17 combine ops under 5 profiles | **SDF** | Composition needs a signed distance from any point to each operand. A grid has occupancy and a mesh has neither — see the README's "Why composing needs a distance field" |
| Cut / trim (rect, circle, polygon, lasso, trim-curve) | **SDF** | Each is an exact prism combined into the edit list. On a grid it would be a cell write and on a mesh it would change topology |
| Armatures (ZSpheres) | **SDF** | A tree of spheres whose links are swept cones and whose skin is the blend. It is a *primitive*, not a gesture |
| `Draw` | **Mesh** | Displacement along the region's **averaged** normal — one shared direction per stamp. `Op::Relief` is the SDF analogue but is per-point along the accumulated normal, not per-stamp |
| `Layer` | **Mesh** | Deposits up to a ceiling above the surface **as the stroke found it**. Every other deposit verb acts on the surface as it is now, so this one needs the stroke's starting snapshot |
| `Relax` | **Mesh** | Slides vertices *along* the surface to even their spacing. There is nothing to even on a grid, and an SDF has no vertices. **It recovers a stretched grab and not a deformation** — after a taper, six passes move edge-length variation 0.2929 → 0.3050, slightly worse, because the damage is anisotropy and no slide changes how many vertices a ring has |
| `sculpt_fill_cavities`, `repair_close_holes`, `repair_fill_voids`, `repair_report` | **Voxel** | Questions about occupancy and enclosure. `fill_voids` *decides* enclosure rather than guessing locally |
| Sculpt layers | **Voxel** | The grid records what a bracketed run of strokes changed, so its strength stays adjustable afterwards. The SDF side does not need it — the document already is the history |
| Resolution levels (`add_level`) | **Voxel** | An SDF has no resolution to add, and a mesh's is fixed by its import |

### Choosing, in one paragraph

Block out and hard-surface on **SDF**, because that is the only side that
composes and the only side where an edit stays editable. Move to **voxels** the
moment the work becomes free-form — smooth, inflate, pinch and smudge chain
there at flat per-cell cost, where the SDF equivalents bake and steepen. Come
back to SDF as an operand via `Volume.from_voxels`, which is non-destructive and
loses nothing the grid had not already quantised. Use a **mesh layer** when the
topology is one you want to keep: sixteen verbs, colour, taper and twist and a
lattice all reach it without changing a polygon — and if a pull stretches the
triangles badly enough to notice, that is the model asking for retopo rather
than the engine failing.

What each of these costs on the reference iPad, and which tier it lands in, is
in [`docs/09-brush-latency-and-coverage.md`](09-brush-latency-and-coverage.md).

---

## See also

- [`docs/01-sdf-math-foundations.md`](01-sdf-math-foundations.md) — exactness,
  Lipschitz bounds and why the safe step scale moves
- [`docs/05-claycore-library.md`](05-claycore-library.md) — architecture
- [`examples/`](../examples/README.md) — every verb above has a runnable script
  with committed renders and self-checks
- [`openspec/specs/`](../openspec/specs/) — the living requirements
- [`openspec/ROADMAP.md`](../openspec/ROADMAP.md) — what is missing, and why
