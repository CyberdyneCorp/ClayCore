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
| **Voxel verb** | An in-place edit of a palette-indexed grid. | n/a — occupancy, not a field | **Not on the undo stack** — the command vocabulary covers document edits only |
| **Mesh verb** | An in-place displacement of a mesh layer's own vertices. Topology never changes. | n/a — triangles, not a field | **Not on the undo stack** either; a sparse `VertexDeltas` record restores the gesture bit-exactly instead |

Two rules hold throughout and explain most of the API shapes below:

- **No camera enters the engine.** A caller passes the frame, plane, anchor or
  normal it already has, in world units. Picking and cameras stay in the host.
- **Everything a document brush produces is an ordinary edit**, so undo,
  coalescing, serialization and instancing apply without any brush-specific
  machinery. Voxel and mesh verbs are the exception: they mutate storage beside
  the document in place, and `scene::Command` has no variant for either. A host
  that wants undo over voxel edits is snapshotting them itself today (roadmap
  item); for mesh edits it keeps a `mesh::VertexDeltas` per gesture, which is
  bounded by the vertices the gesture reached rather than by the mesh.

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

`move_brush` is pure, so a drag can be **previewed** — `Layer.move_surface_preview`
and `clay_layer_move_surface_preview` return the nodes it would warp without
touching the document.

Applying the result needs `SetDeformersCmd`, which is new too: the command
vocabulary could not change a node's deformers at all, so a deformer could only
be set when its node was created.

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

`relax` has the same pair, mirroring flatten symbol for symbol
(`clay_item_volume_relax_from` beside `clay_item_volume_flatten_from`). For
relax the document-sourced form is exactly bake-then-relax fused into one call
— relax averages cell-aligned taps, and a fresh bake's taps *are* the document
at those lattice points — so inside the band the two are identical, and a test
holds that. What it buys is one entry point, and a source that can never be a
volume derived from another volume.

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
| `sculpt_grab` | Translate occupancy through the same inverse map the SDF `grab` deformer uses, so both representations mean the same thing. Resampling is nearest-cell and rounds **per axis**, so a displacement under half a cell on every axis moves nothing — a drag fed raw pointer deltas is dead until the host accumulates them past `voxel_size` |
| `sculpt_fill_cavities` | Fill pockets: an empty cell with ≥4 of its 6 face neighbours occupied is inside a cavity. The rule is local, so it fills what is **narrow**, not what is enclosed — a through-hole wider than one cell does not qualify, a one-cell perforation does. Its everyday input is a **dithered soft stamp**, which leaves single-cell holes through its own deposit; closing them cut a test stroke's greedy mesh by 27%. `repair_fill_voids` is the one for sealed voids, and neither substitutes for the other |
| `sculpt_carve_alpha` | A caller-supplied scalar stamp modulating per-cell strength. **The engine decodes no images** — a host with an alpha has already loaded a PNG |
| `repair_report` | What a pre-bake check wants to know, without performing the fix |
| `repair_close_holes` | Seal perforations by the same pocket rule. Only ever adds cells |
| `repair_fill_voids` | Fill every empty cell the outside cannot reach — enclosure is *decided*, not guessed locally |

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
a painted mask protects polygons from all eleven verbs — `Grab` and `Snakehook`
included — with no per-verb code.

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

**Not dynamic topology.** No dyntopo, no multires, no remeshing, no
subdivision — see the amended non-goal in
[`docs/sculpt_comparison.md`](sculpt_comparison.md). Not in the parity system
either: this is CPU-side like the voxel verbs, and the determinism bar is
asserted instead — same stroke, same mesh, same result, bit for bit.

Runnable: [`examples/45_mesh_brushes.py`](../examples/45_mesh_brushes.py),
[`46_mesh_brush_compositions.py`](../examples/46_mesh_brush_compositions.py) and
[`47_mesh_brush_reach_and_undo.py`](../examples/47_mesh_brush_reach_and_undo.py).

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
| Move | `brush::move_brush` | Drags the assembled surface. Nudges form rather than growing it: a large pull buds rather than stretches, and a stroke's drags compound the step scale — use `snakehook` to pull a lobe out |
| Rotate | `pose` / `pose_line` | Radial, or ramped along a line |
| Gizmo Twist | `twist_range` | The gizmo acts inside its box: the rotation ramps across the span and holds beyond. Plain `twist` winds the whole item, which is the difference |
| Gizmo Bend Arc | `bend_range` | Angle-limited bend, same shape |
| Gizmo Bend Curve | `bend_curve` | A bend along an arbitrary guide. Implemented as the INVERSE of the swept primitive — the same nearest-point query and transported frames, read from the other end — so the two agree about what a guide is by construction |
| Gizmo Lattice / FFD | `mesh::Lattice` on a mesh layer, `Deformer::lattice` on an SDF item, `brush::lattice_gizmo` over a whole LAYER | **Both forms, and they are not the same map.** On a mesh it runs FORWARD and is exact — which is what ZBrush and Blender do, because a mesh knows where its vertices are. On an SDF item forward FFD has no closed-form inverse, so the cage is authored AS the inverse: closed-form and portable, but not the exact inverse of the forward map. The two differ by a term proportional to how the basis varies along the displacement — measured at under 1.5% of the drag (`examples/50_sdf_lattice.py`), and SIGNED rather than always-less, so it does not inherit `grab`'s character. Divisions are capped at 4 per axis on the SDF side against 32 on the mesh side, because that one runs per SAMPLE rather than per vertex. A gizmo acts on the whole subtool, so `brush::lattice_gizmo` resolves one world-placed cage into a per-item lattice each carrying the transform into the cage's frame — exact for ROTATED items, which no axis-aligned per-item box could express, and reaching EVERY item because a lattice's displacement outside its box is clamped rather than zero |
| Pinch | `magnify` (negative), `sculpt_pinch` | One signed strength, not two verbs |
| Magnify | `magnify` (positive), `sculpt_magnify` | Maxon's own page calls them inverses |
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
| Alphas | `sculpt_carve_alpha` | Voxel side only so far |
| Blob | `blob` | Noise with the finite support `grab` and `magnify` have — outside the radius the field is untouched. One SIGNED amplitude, so a single dab both swells and eats in, which is what reads as blobby rather than as a uniform bulge |
| Slice / Knife (polygroup splits) | — | Splitting without removing volume has no single-solid equivalent; it needs two items |
| ZRemesher (quad retopology) | partly — `mesh_quads` | **Not the same thing, and the difference matters.** claycore meshes a sculpt into a QUAD GRID DERIVED FROM ITS LATTICE: quad-only, regular, no T-junctions, with a target count you choose — enough to hand a form to a DCC as OBJ or FBX. What it is not is field-aligned: no edge loops following the form, no poles placed at features, density that does not follow curvature. A retopology pass REPLACES this rather than refining it |
| Surface-mode mesh brushes | `mesh::MeshSculptor` (§ 8) | **The non-goal was narrowed, not dropped.** Vertices move on a mesh layer's own triangles; dyntopo, multires and remeshing remain out of scope |
| Dynamic tessellation (Dyntopo, LiveClay) | — | Out of scope on purpose: an SDF sidesteps topology entirely, and competing on dynamic tessellation is not this engine's fight |

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
| Quad meshing, with a target count | `mesh::mesh_tape_quads`, `mesh_tape_quads_fit`, `VoxelGrid::mesh_quads` | `Document.mesh_quads(...)`, `VoxelGrid.mesh_quads(...)` | `clay_document_mesh_quads`, `clay_voxel_mesh_quads` |
| Triangles straight to voxels | `VoxelGrid::rasterize_mesh` | `VoxelGrid.rasterize_mesh(...)` | `clay_voxel_rasterize_mesh` |
| A mesh a document carries | `scene::LayerKind::Mesh` | `Document.add_mesh_layer(...)`, `.mesh_layer(...)` | `clay_document_add_mesh_layer`, `clay_document_mesh_layer`, `clay_mesh_layer` |
| Fixed-topology mesh brushes | `mesh::MeshSculptor::stamp`, `mesh::MeshBrush` | `MeshSculptor.stamp(...)` | `clay_mesh_sculptor_stamp`, `CLAY_MESH_BRUSH_*` |
| Lattice cage on a mesh layer | `mesh::Lattice`, `mesh::MeshSculptor::apply_lattice` | `Lattice(...)`, `MeshSculptor.lattice(...)` | `clay_mesh_lattice_*`, `clay_mesh_sculptor_lattice` |
| Lattice cage on an SDF item | `scene::Deformer::lattice` | `p.lattice(...)` | `clay_item_add_lattice`, `CLAY_DEFORM_LATTICE` |
| One cage over a LAYER (the gizmo) | `brush::lattice_gizmo`, `brush::caged_chain` | `Layer.lattice_gizmo(...)` | `clay_layer_lattice_gizmo`, `clay_layer_lattice_gizmo_preview` |
| A mesh stroke | `brush::apply_to_mesh` | `MeshSculptor.apply_stroke(...)` | `clay_mesh_sculptor_apply_stroke` |
| Mesh vertex adjacency | `mesh::Adjacency` | `MeshSculptor.class_count` | `clay_mesh_sculptor_class_count` |
| Mesh stroke undo | `mesh::VertexDeltas` | `clay.VertexDeltas` | `clay_mesh_deltas_*` |
| Picking a mesh layer | `pick::raycast_mesh`, `mesh::Bvh::raycast` | `MeshSculptor.raycast(...)` | `clay_mesh_sculptor_raycast` |

Snakehook has no dedicated C entry point on purpose: it is a **resolver** that
produces an ordinary stroke item, and the C ABI already builds those. A separate
call would be a second way to say the same thing.

Parity between `pyclay` and the C ABI is enforced in CI by
`tools/check_binding_parity.py`, which fails on a Python capability with no C
counterpart **and** on an exemption that has gone stale.

---

## See also

- [`docs/01-sdf-math-foundations.md`](01-sdf-math-foundations.md) — exactness,
  Lipschitz bounds and why the safe step scale moves
- [`docs/05-claycore-library.md`](05-claycore-library.md) — architecture
- [`examples/`](../examples/README.md) — every verb above has a runnable script
  with committed renders and self-checks
- [`openspec/specs/`](../openspec/specs/) — the living requirements
- [`openspec/ROADMAP.md`](../openspec/ROADMAP.md) — what is missing, and why
