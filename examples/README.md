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
