# Brushes and features

Every sculpting verb claycore ships, what it does, and how it is parameterised.

The engine has no brush *objects*. What a host calls a brush is one of five
mechanisms, and knowing which one a verb is tells you most of what you need:
whether it is exact, whether it can be undone as an edit, and whether it bakes.

| Mechanism | What it is | Exactness | Undo |
|---|---|---|---|
| **Combine op** | How an item joins the field accumulated before it. Lives on the node; recompiled every evaluation. | Booleans under a hard blend are exact; smooth blends, the extended set and relief are not, and the tape says so | Ordinary edit |
| **Deformer** | A map applied to the point before the item's primitive is evaluated. A chain per node. | Breaks exactness where it is not rigid; the Lipschitz factor is folded into the tape | Ordinary edit |
| **Baked field operation** | Samples a field into a narrow-band `FieldVolume` with the operation applied. | Result is a bound field, not a distance — it declares the Lipschitz it measured | Replaces the item's volume |
| **Resolver** | A pure function turning a gesture into an *ordinary item*. No document is read or touched. | Whatever the item it produces is | Ordinary edit |
| **Voxel verb** | An in-place edit of a palette-indexed grid. | n/a — occupancy, not a field | **Not on the undo stack** — the command vocabulary covers document edits only |

Two rules hold throughout and explain most of the API shapes below:

- **No camera enters the engine.** A caller passes the frame, plane, anchor or
  normal it already has, in world units. Picking and cameras stay in the host.
- **Everything a document brush produces is an ordinary edit**, so undo,
  coalescing, serialization and instancing apply without any brush-specific
  machinery. Voxel verbs are the exception: they mutate a grid in place, and
  `scene::Command` has no voxel variant, so a host that wants undo over voxel
  edits is snapshotting them itself today (roadmap item).

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

Two consequences worth knowing before using them:

- The **rounding does double duty**: it is the falloff width *and* it rounds the
  region's own field, exactly as it does for groove and tongue. So the reach is
  region + rounding + falloff, and the influence bound is dilated by both.
- **Amplitude ÷ falloff width is one number seen twice.** It is what turns the
  rim into a ledge rather than a swell, and it is exactly the slope the op adds
  — so a picture that looks harsh and a step scale that dropped are the same
  fact. See [`examples/25_relief.py`](../examples/25_relief.py).

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
share and leaves the rest behind: on two blended balls, grabbing the left lifts
its side by 0.118 and the right by only 0.022.

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
evaluates at the origin. Worth knowing in its own right — a group carrying a
transform silently does nothing.

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
| `field::relax` | Smooth the field — the ZBrush Smooth brush. Averages over a cell neighbourhood | strength, `radius_cells`, iterations, centre, `region_radius`, falloff |
| `field::flatten` | Pull the surface onto a plane. **Two-sided**: material on the normal's side goes *and* hollows on the other side fill | plane point + normal, strength, centre, `region_radius`, falloff |

Both take a region with a falloff. A `region_radius` of zero means:

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

`flatten` has two overloads. Prefer the one taking a **document sampler**: a
volume's band tracks the surface only while the surface stays inside it, and
flatten moves it many band widths, so flattening a volume in place is accurate
only near the band it came from. The volume overload exists for imported meshes,
where there is no document behind the surface.

---

## 4. Resolvers

Pure functions turning a gesture into an ordinary edit item. Nothing is read or
written, so a host can preview the result before committing it.

| Resolver | What it does | Returns |
|---|---|---|
| `cut::cut_item` | A shape drawn on a frame (rect, circle, polygon, spline lasso) becomes an extruded item sized to cut through | a `Node`, or nothing for a non-orthonormal frame or a zero-area shape |
| `brush::snakehook` | A drag from a surface anchor becomes a tapered stroke item — a horn, tendril or spike | a `Node`, or nothing for an empty path or degenerate normal |
| `brush::move_brush` | A world-space drag becomes the per-item warps that move the ASSEMBLED surface | one `grab` per contributing item, in that item's own frame |

**The cut is a prism, not a frustum.** A converging cut has a non-flat face and
a result that depends on where the camera stood, so the sweep is parallel and
the caller passes the frame. Keep-inner versus keep-outer is *the op* — place
the result with `Subtract` or `Intersect` — not a separate flag, which would be
a second way to say one thing.

**Snakehook adds material rather than moving it.** ZBrush pulls existing
surface, so the body dimples slightly where the tendril came from; this grows a
tendril and leaves the body alone. The difference shows only at the base.
`taper_curve` shapes the thinning as `(1 − t)^c`: **above 1 thins away quickly**
and leaves a long thin whip, below 1 holds thickness and then drops, which is
the shape of a horn.

---

## 5. The stroke engine

Samples in, edit items out. `resolve_stroke` is the pure core; the consumers
apply the resulting stamps to a voxel grid or turn them into SDF nodes. Because
stamps become ordinary edits, undo, coalescing and serialization apply
unchanged.

| Feature | Field | What it does |
|---|---|---|
| Spacing | `spacing` | Distance between stamps as a fraction of brush **diameter**. 0.25 is dense; 1.0 places them just touching |
| Pressure | `pressure.size`, `.strength`, `.curve` | Exponents on normalized pressure. 0 disables a channel — that is what "size only" and "flow only" brushes are |
| Jitter | `jitter_position`, `jitter_size`, `jitter_rotation`, `seed` | Derived from the stamp index and seed, **never from a random source**, so a stroke resolves identically everywhere |
| Rotate along stroke | `rotate_along_stroke` | Turns each stamp to follow the path; only matters for stamps that are not rotationally symmetric |
| Taper | `taper_start`, `taper_end` | Fraction of stroke length over which the radius ramps in and out |
| Steady stroke | `steady` | "Lazy mouse" — the emission point trails the cursor, smoothing a shaky path |
| Accumulation | `accumulation` | `Buildup`: passing twice acts twice. `Clamped`: the stroke reaches its strength once, however many stamps overlap |
| Base | `radius`, `strength` | What pressure, taper and jitter modulate |

Presets serialize with a **schema version from the first release** rather than
one retrofitted later: presets outlive engine versions, and a library of them
silently reinterpreted by a later build is the failure that number prevents.
Deserialization accepts its own version and earlier ones, and **refuses a newer
one** rather than reading a prefix and pretending.

A tap has to leave a mark: a single sample, or a path shorter than one spacing,
yields exactly one stamp at the start.

---

## 6. Voxel sculpting verbs

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
| `sculpt_grab` | Translate occupancy through the same inverse map the SDF `grab` deformer uses, so both representations mean the same thing |
| `sculpt_fill_cavities` | Fill pockets: an empty cell with ≥4 of its 6 face neighbours occupied is inside a cavity. A through-hole does not qualify |
| `sculpt_carve_alpha` | A caller-supplied scalar stamp modulating per-cell strength. **The engine decodes no images** — a host with an alpha has already loaded a PNG |
| `repair_report` | What a pre-bake check wants to know, without performing the fix |
| `repair_close_holes` | Seal perforations by the same pocket rule. Only ever adds cells |
| `repair_fill_voids` | Fill every empty cell the outside cannot reach — enclosure is *decided*, not guessed locally |

Every verb takes `BrushParams`: `size`, shape (`Cube`/`Sphere`), falloff
(`Constant`/`Linear`/`Smooth`/`Gaussian`), `strength`, `seed`, and an optional
**mask**. Where a mask is given the effective weight is scaled by `1 − mask`, so
a fully masked cell is untouched by *every* verb rather than by a hand-picked
few.

---

## 7. ZBrush equivalents

Where a ZBrush brush maps onto the list above. This is a map, not a claim of
parity — the mechanism usually differs even where the result matches.

| ZBrush | claycore | Note |
|---|---|---|
| Standard, ClayBuildup | `Op::Relief` | Displaces the accumulated surface along its normal |
| Crease, DamStandard | `Op::Incise` | The same op, cutting in — a thin region gives the line |
| Inflate | `Op::Relief`, `sculpt_inflate` | Moving the surface along its own normal *is* relief; the voxel verb dilates and erodes by cells |
| Move | `brush::move_brush` | Drags the assembled surface. The raw `grab` deformer is per **item** and **local** — see the note below |
| Rotate | `pose` / `pose_line` | Radial, or ramped along a line |
| Pinch | `magnify` (negative), `sculpt_pinch` | One signed strength, not two verbs |
| Magnify | `magnify` (positive), `sculpt_magnify` | Maxon's own page calls them inverses |
| Smooth | `field::relax`, `sculpt_smooth` | Bakes on the SDF side |
| Flatten | `field::flatten`, `sculpt_flatten` | Two-sided; region required on the SDF side |
| Trim (Rect/Circle/Lasso) | `cut::cut_item` | The practitioners' "90% tool" |
| Clip | `cut::cut_item` | **As a solid, Clip is exactly Trim.** Clip's distinctive look is a zero-thickness fin a field cannot represent and users delete anyway |
| SnakeHook | `brush::snakehook` | Adds material rather than pulling it |
| Surface Noise | `noise` deformer | Integer hash, so all four backends agree |
| Mask | mask fields, layer lock/ghost | Paintable, survives resolution changes |
| Alphas | `sculpt_carve_alpha` | Voxel side only so far |
| Blob | — | Not yet: `add-blob-brush`, unblocked by the noise field |
| Slice / Knife (polygroup splits) | — | Splitting without removing volume has no single-solid equivalent; it needs two items |
| Surface-mode mesh brushes | — | Out of scope: claycore sculpts fields and voxels, not meshes |

---

## 8. Where each is reachable

Names differ between bindings, so this lists them rather than ticking boxes.

| Verb | C++ | `pyclay` | C ABI |
|---|---|---|---|
| Combine ops | `scene::Op` | `clay.Op.*` | `CLAY_OP_*` |
| Blends | `scene::Blend`, `BlendProfile` | `clay.Smooth/Cubic/Circular/Chamfer(k)` | `clay_item_set_blend`, `CLAY_BLEND_*` |
| Deformers | `scene::Deformer::twist(...)` etc. | methods on the prim: `p.twist(...)`, `p.noise(...)`, `p.magnify(...)` | `clay_item_add_deformer` |
| Stroke engine | `brush::resolve_stroke`, `StrokePreset` | `clay.StrokePreset`, `layer.apply_stroke(...)` | `clay_stroke_resolve`, `clay_stroke_preset_*`, `clay_layer_apply_stroke`, `clay_voxel_apply_stroke` |
| Relax | `field::relax` | `Volume.relaxed(...)` | `clay_item_volume_relax` |
| Flatten | `field::flatten` | `Volume.flattened_from(...)` | `clay_item_volume_flatten` |
| Cut tool | `cut::cut_item`, `cut::CutShape` | `clay.Cut(...)`, `clay.CutShape.rect/circle/from_polygon/from_curve` | `clay_cut_create`, `clay_cut_polygon_from_curve` |
| Snakehook | `brush::snakehook` | `clay.snakehook(...)` | `clay_item_create` + `clay_item_set_curve_points` |
| Voxel verbs | `VoxelGrid::sculpt_*` | `VoxelGrid.sculpt_*` | `clay_voxel_sculpt_*` |
| Move brush | `brush::move_brush`, `moved_chain` | `Layer.move_surface(...)` | `clay_layer_move_surface` |
| Deformers on a placed node | `scene::SetDeformersCmd` | (through `move_surface`) | `clay_layer_add_deformer` |
| Masks | `voxel::MaskField` | `clay.MaskField` | `clay_mask_*` |

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
