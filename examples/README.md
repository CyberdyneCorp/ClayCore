# claycore examples

Runnable scripts that exercise the library through its Python bindings, with
their rendered output committed so the gallery is viewable without building
anything.

Every image on this page was produced by the script next to it. There is no
renderer dependency: `examples/_render.py` traces the field with
`Document.raycast_many` and writes PNGs with `zlib` and `struct` from the
standard library. Examples import only `pyclay`, `numpy`, and stdlib.

## Running them

```bash
cmake --preset cpu-only -DCLAY_BUILD_PYTHON=ON
cmake --build --preset cpu-only
PYTHONPATH=build/cpu-only/bindings/python python examples/run_all.py
```

Or, against an installed wheel:

```bash
python -m pip install .
python examples/run_all.py
```

`run_all.py` regenerates everything under `examples/output/` in place, so a
kernel change shows up as an image diff. Each script also runs standalone.
CI runs all of them and fails on a non-zero exit, so a binding rename breaks
the build rather than silently rotting the gallery.

## The gallery

### 00 — the hero image

The README's banner: an SDF sculpt and a voxel sculpt side by side, rendered
at matching size and joined with a divider. Deliberately the same kind of
subject on both sides — one form in the round — so the contrast is the
representation, not the subject.

![SDF and voxels side by side](output/00_hero.png)

The SDF half is a lesson in blend radii: every feature has to sit *outside*
the body it blends onto, or smooth-min swallows it and the result is a plain
egg. The voxel half is a character rather than a landscape, because at this
size a landscape's voxels would each be a handful of pixels.

### 01 — primitives

Every primitive class the module exposes, enumerated from the module itself:
if a primitive is added to the bindings without a tile here, the example
fails rather than quietly under-reporting coverage.

![primitives](output/01_primitives.png)

The plane and infinite cylinder have no finite bounds, so they are shown by
what they carve out of a sphere. They also cannot be auto-framed — the
example passes an explicit camera, and `orbit_camera` raises rather than
returning a blank image.

![unbounded primitives](output/01_primitives_unbounded.png)

### 02 — blends and combine modes

The same sphere and box in every tile, so only the combination varies.

![blends](output/02_blends.png)

The eight extended modes shape the seam rather than the volumes.

![extended combine modes](output/02_blends_extended.png)

Colour blends across a smooth seam, and `PAINT` recolours without touching
the field.

![colour blending](output/02_blend_colors.png)

### 03 — deformers

Twist, bend, taper and displace, plus chaining. The last two tiles are the
same two deformers in opposite order — deformers apply in authoring order,
and the results differ. Each prints the safe step scale it costs, since a
warped field is no longer a true distance function.

![deformers](output/03_deformers.png)

### 04 — repetition

Finite grids, radial arrays, and infinite grids. The example prints the
bounds of each, because that is the part that matters: a finite array is
still cullable, while an infinite grid reports infinite influence and is
never dropped by per-brick culling.

![finite grid](output/04_repeat_grid_finite.png)
![radial array](output/04_repeat_radial.png)

Repetition composes with deformers — this is an array of a twisted box.

![deformed array](output/04_repeat_deformed.png)

### 05 — profile lifts

Closed 2D profiles swept into 3D. Lifts are exact, so unlike deformers they
cost nothing in step scale.

![extruded profiles](output/05_extrude_profiles.png)
![revolved profiles](output/05_revolve_profiles.png)

Polygon profiles handle concavity, including self-intersection, by the
even-odd rule.

![extruded star](output/05_extrude_star.png)
![revolved vase](output/05_revolve_vase.png)

### 06 — transition morphs

Morphing a box into a sphere along an axis and outward from a centre, with
easing curves over the band.

![linear transition](output/06_transition_linear.png)
![easing](output/06_transition_easing.png)
![radial transition](output/06_transition_radial.png)

### 07 — voxel sculpting

Box and line fills, brush stamps, mirrored placement, flood select, erase and
paint, then greedy meshing and picking.

![voxel edits](output/07_voxel_edits.png)
![mirrored sculpting](output/07_voxel_mirror.png)
![flood select](output/07_voxel_flood.png)
![carving](output/07_voxel_carve.png)

Brushes come in two footprints, cube and sphere, selected with `shape=`. A
brush of size N covers exactly N cells per axis for every N: the footprint runs
`-((N-1)/2) ..= N/2`, symmetric for odd N and biased half a cell toward the
positive axes for even N. The sphere is the ball of diameter N, so it is always
a subset of the cube and its occupancy ratio approaches π/6.

![cube and sphere brushes](output/07_brush_shapes.png)

Voxel renders go through `VoxelGrid.raycast`, which handles one ray at a
time, so they run at lower resolution than the SDF examples.

### 08 — meshing and I/O

The three meshers compared, decimation ratios, watertightness, every export
format, the `.clayspace` round trip, and rasterizing an SDF into a voxel
layer.

![scene](output/08_scene.png)
![rasterized SDF](output/08_rasterized.png)

### 09 — falloff brushes and sculpting verbs

Occupancy is binary, so a soft brush cannot make a cell half-solid. What it
can do is cover a *fraction* of its footprint, thinning toward the rim. That
is a falloff curve resolved by dithering against a hash of the cell
coordinate — a hash rather than a random number, so the same stroke always
produces the same cells and these renders regenerate identically.

![falloff curves](output/09_falloff_curves.png)

Strength scales the same curve down.

![falloff strength](output/09_falloff_strength.png)

The four verbs reshape material rather than stamping a footprint. Every tile
starts from the same bumpy slab, so the only difference is which verb ran.

![sculpting verbs](output/09_sculpt_verbs.png)

Each verb reads a snapshot of the region before writing, so no cell's outcome
depends on a neighbour the same call already changed. Note that inflate
followed by erode is not an identity — that is a morphological closing, which
fills small hollows on the way through.

### 10 — editing an existing document

Every other example builds a document and renders it. This one changes one that
is already there: place items, keep their ids, then move, swap, re-blend,
recolour and remove them. Each tile starts from the same three blobs, so the
only difference is which edit ran.

![editing](output/10_editing.png)

Strokes can be grown after they are placed, which is what a drag gesture
issues.

![extended stroke](output/10_stroke_extended.png)

Node ids come back from `Layer.add` and survive every edit, which is what lets
a UI hold a selection. Each entry point applies one command from the engine's
vocabulary — the same one `.clayspace` records.

### 11 — masks

A mask is a scalar field in [0, 1] scaling how strongly a brush may act: the
effective strength at a cell is `strength * (1 - mask)`. All three tiles below
run the same erase, with the same dither seed; the third has the left half
masked, and nothing there moves.

![mask freeze](output/11_mask_freeze.png)

Painting the mask with a falloff gives a graded gate, so the cut fades out
across the boundary instead of stopping at a wall.

![mask falloff](output/11_mask_falloff.png)

A mask is addressed in **world units on its own lattice**, not in a layer's
voxel cells, so it survives a resolution change and a `.clayspace` round trip —
the example asserts both. That is deliberate: masks silently dying on
voxelization is the most complained-about defect in the tool this engine is
measured against.

Masking gates edits where they are *authored*. Voxel edits consume it per cell
at edit time; it does not retroactively protect a region from SDF items already
in the edit list.

### 12 — strokes

A drag becomes stamps, and a stamp becomes an ordinary edit. Resolving is pure —
samples and a preset in, stamps out, no document touched — and applying produces
voxel brush stamps or nodes in a layer's edit list. Because a stroked edit is an
*ordinary* edit, undo, coalescing, `.clayspace` serialization and picking apply
to it without knowing this engine exists.

![stroke presets](output/12_stroke_presets.png)

Steady stroke ("lazy mouse") lets the emission point trail the cursor. Seen from
above, the wobble drops from 0.050 to 0.011 across these three.

![steady stroke](output/12_stroke_steady.png)

One stroke on an SDF layer: 67 nodes, and exactly one undo step.

![stroked sdf](output/12_stroke_sdf.png)

The example asserts three things rather than showing them: spacing follows the
path and not the sample rate, jitter is a hash so a preset reproduces its stroke
exactly, and a preset from a newer schema version is refused rather than
reinterpreted. It also shows what freeze means for a declarative edit — a stamp
in a masked region produces no item at all.

### 13 — curves

A stroke point carries a type saying how it joins the next: a hard corner, a
Catmull-Rom spline through the points, an approximating B-spline that rounds
corners off, or a Bezier shaped by two local-space handles. All four tiles
below are the *same six control points*.

![point types](output/13_point_types.png)

Typed points are tessellated into the segment chain the engine already
evaluates, at compile time, so a curve costs nothing at evaluation time and no
backend knows it exists. The tolerance is the largest distance a span's
midpoint may sit from its chord, and it is a property of the **document**, not
of the viewer — two builds have to agree on what a document means.

![tolerance](output/13_tolerance.png)

Bounds come from the tessellated curve rather than the control points. Both
control points of this arc sit at y = 0; the handles carry the span to y = 1.5,
and a bound taken from the control points would have culling drop the arc and
picking miss it.

![bezier arc](output/13_bezier_arc.png)

The example asserts three things rather than showing them: an all-hard chain
evaluates bit-identically to what it did before types existed, a ray really
finds the arc outside the control hull, and editing a placed curve undoes
exactly.

### 14 — the cut tool

A shape drawn over the model, cut through it: rect, circle, polygon, or a
spline lasso flattened through the curve tessellator.

![cut shapes](output/14_cut_shapes.png)

Which side survives is the **op**, not a parameter of the cut — subtract
removes what the shape covers, intersect keeps only that. 3DCoat's "Shift =
keep-outer" modifier is exactly this choice.

![cut sides](output/14_cut_sides.png)

The sweep is sized to the region so a cut goes all the way through; giving the
extent by hand is how a deliberate partial cut is expressed, and rounding
bevels the walls.

![cut depth](output/14_cut_depth.png)

Nothing in the cut knows where "the camera" is — only what frame it was handed,
so the same call from a frame looking down cuts top to bottom.

![cut from above](output/14_cut_from_above.png)

The example asserts the design's load-bearing claim: **a cut is a prism, not a
frustum**. The same cut resolved from a frame ten times further away gives the
identical solid. A converging cut would have a face that is not flat and a
result that depended on where the camera stood.

## Notes

- **Committed models are budgeted.** `_render.save_model` fails above 400 KiB
  so the repository does not accumulate large binaries; `export_model` is the
  one place meshing settings are tuned. Binary PLY is much smaller than ASCII
  OBJ for the same mesh.
- **Voxel meshes report `watertight=False`.** Greedy meshing gives each merged
  quad its own vertices, which is what keeps per-face colour exact. The
  duplicated vertices make an index-based watertightness check see boundary
  edges even though the surface is geometrically closed. SDF meshes, which
  share vertices, report watertight.
