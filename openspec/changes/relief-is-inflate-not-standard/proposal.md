## Why

A host binds two ZBrush brushes on an SDF layer to one op: **Standard** and
**Inflate** both go to `CLAY_OP_RELIEF`, because the header says
`/* build up: ZBrush Standard, ClayBuildup */` and `docs/07` §9 maps Standard
and Inflate to `Op::Relief`. They cannot both be faithful. On a mesh the two
differ in exactly one column of the brush model (`clay_brush_frame`): Draw —
the engine's own `Standard` preset in `brush::reference_presets()` — displaces
along ONE averaged normal per stamp, and Inflate along each vertex's own.

The hypothesis to test: Relief offsets the accumulated field, `a - k*w(b)`, and
offsetting a distance moves each point of the isosurface along the field's own
gradient. That is the Inflate frame, so the brush Relief only approximates is
**Standard**.

## What was measured

Four fixtures, each meshed at voxel 0.01 and stamped once at the standard clay
mapping (`k = rounding = region radius = 0.15`, reach 0.45):

- a unit sphere, stamped at its pole (convex)
- a torus `R = 1, r = 0.4`, stamped on its inner equator (saddle)
- a box with a sphere `r = 0.8` carved out, stamped in the bowl (concave)
- a fin 0.1 thick and 1.0 tall on a slab, stamped on its top edge (a ridge
  narrower than the stamp)

**First, the model is the engine.** Engine Relief equals `a - k*smoothstep`
of `(|p - c| - R - W) / W` to within 1.4e-7 at 20,000 points per fixture, and
the mesh sculptor's draw displaces every moved vertex along one direction
(within 0.03 deg on the sphere, 1.6 deg on the saddle) and inflate along each
vertex's own normal (within 4.4 deg of the marching mesh's normals on the
sphere; the fin's corners reach 45 deg, where the sculptor's welded normal and
the mesher's disagree).

**Frame-isolated references.** For every mesh vertex in the stamp's support,
two references were built with the relief's OWN weight (solved at the moved
point, as the field reads it): `inflate_ref = v + k*w*n(v)` and
`draw_ref = v + k*w*N`, where `N` is the falloff-weighted averaged normal. The
table is each reference's distance to the engine's relief surface, as a
fraction of `k`:

| fixture | normal spread under the stamp (mean / max) | inflate_ref: mean / max | draw_ref: mean / p95 / max |
|---|---|---|---|
| sphere, convex | 17.4 / 27.4 deg | 0.000 / 0.000 | 0.017 / 0.033 / 0.042 |
| torus inner equator, saddle | 36.6 / 74.3 deg | 0.000 / 0.000 | 0.077 / 0.204 / 0.356 |
| bowl, concave | 21.6 / 34.8 deg | 0.000 / 0.000 | 0.027 / 0.053 / 0.066 |
| thin fin, ridge | 79.2 / 95.2 deg | 0.001 / 0.093 | 0.568 / 1.091 / 1.278 |

Relief reproduces the per-point-normal displacement to the precision of the
measurement on all four; the 0.093 on the fin is its corner, on the medial axis.
**The hypothesis holds: Relief is Inflate.** The same holds at `k = 0.05`
(inflate_ref max 0.002k off the fin's corner) and at a stamp twice as wide
(max 0.017k off the fin's corner, which stays at 0.093k).

**How wrong it is as a Standard depends on one thing: how far the normals under
the stamp spread.** Where the surface is smooth at the brush's scale the
difference is a few percent of the amplitude. On a feature narrower than the
stamp it is the whole amplitude:

| the fin after one stamp (`k = 0.15`) | top rise | half-thickness change at y = 0.4 |
|---|---|---|
| SDF Relief | +0.1500 | **+0.1500** (0.05 becomes 0.20) |
| mesh Inflate (engine) | +0.1500 | +0.1268 |
| mesh Draw (engine) | +0.1491 | **+0.0115** |

And when the stamp is wide against the form (reach 0.90), the difference stops
being small anywhere with curvature: draw_ref sits 0.573k (saddle) and 0.566k
(bowl) from the relief surface on average.

**Against the engine's mesh brushes directly**, with the SDF side given the
mesh brush's own smoothstep falloff so only the frame differs, on the fin: mesh
Draw lies 0.014k (mean) from a draw-frame field and 0.229k from relief; mesh
Inflate lies 0.051k from relief and 0.289k from the draw frame. The residual
floor on the smooth fixtures (about 0.02-0.04k mean, 0.18k max) is the mesh
reading its weight at the vertex's rest position where a field reads it at the
sample.

## Can the missing Standard be built as a relief variant? Measured: no

The plan was an op beside Relief that displaces along one per-stamp direction.
Three spellings were measured (details in `design.md`):

- **The exact draw frame is a warp of the accumulation**, `a(p - k*w(p)*N)`. It
  matches draw_ref to 0.000k mean on all four fixtures. It is not pointwise in
  `a`: it needs the accumulated field at ANOTHER point, and a combine record in
  the tape only has `a` at this one. That is precisely what makes Relief cheap.
- **Its first-order form**, `a - k*w*(grad a . N)`, needs the accumulation's
  gradient, which the tape does not carry either, and fails anyway where it
  matters: 0.129k mean / 0.733k p95 from draw_ref on the fin. Its slope carries
  the field's curvature, which a box edge makes unbounded, so no Lipschitz term
  is sound.
- **The engine can already spell it exactly**: `Layer.move_surface(c, k*N,
  radius, ease=smoothstep)` scores identically, to the printed 0.001k, to the
  warp model carrying the same falloff, against every reference on every
  fixture. It is one per-item warp per dab, and a stroke is
  where it collapses. 30 dabs over a 24-item blockout: **700 warps**, a
  200k-point evaluation of **76.2 ms against 8.8 ms** for the same stroke as
  relief (4.3 ms bare), and a safe step scale of **1.5^-30 ≈ 5e-6** against
  relief's 1/46, because warp bounds multiply and offset bounds add.

## What Changes

- **Nothing in the engine.** No new op, no ABI or format change. The op the
  plan asked for cannot be a combine op, and the per-item warp that can express
  it is unaffordable per dab.
- Correct every place that maps ZBrush Standard onto `Op::Relief` as though it
  were faithful: `bindings/c/clay.h` (the `CLAY_OP_RELIEF` comment),
  `include/clay/kernel/tape.h`, `docs/07` §9, `docs/09`'s inventory,
  `docs/sculpt_comparison.md`, and `examples/25_relief.py`'s docstring. Relief
  is named as Inflate; Standard as its approximation, with the numbers above and
  the one condition — normal spread under the stamp — that decides how far off.
- Tell a host what to bind: Inflate to `CLAY_OP_RELIEF`, faithfully. Standard to
  `CLAY_OP_RELIEF` as an approximation that thickens thin features. A single
  exact draw-frame stamp is `clay_layer_move_surface` with a smoothstep ease;
  a stroke of them is not.
- A test pins the frame, so a future "fix" that turns Relief into a Standard is
  seen to have changed Inflate.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `sdf-kernels`: states the frame Relief displaces along, and what that makes
  it relative to the mesh Draw and Inflate brushes.

## Impact

- Header comments and documentation only; `CLAY_ABI_*`, `kSceneMinor`, pyclay
  and the Swift surface are untouched.
- One test in `tests/unit/test_relief.cpp`.
- Hosts binding Standard to Relief keep working exactly as before; what changes
  is that the divergence on ridges and wide stamps is written down rather than
  found.

## What building it found

- **`docs/07` already contradicted itself.** §9 maps Standard to `Op::Relief`,
  while the one-representation table two sections later says "`Op::Relief` is
  the SDF analogue but is per-point along the accumulated normal, not
  per-stamp", and the preset library defines Standard as Draw.
- **The mesh brush's own averaged normal tilted 16.8 deg on the fin** (0.5 deg
  on the sphere, 2.2 on the saddle), against a falloff-weighted average of the
  mesher's normals that should point straight up by symmetry. The cause was not
  chased; the frame comparisons above pass an explicit `deposit_normal` so both
  sides use the same `N`, and it is worth a look on its own.
- **`move_surface`'s default ease is linear, not smoothstep**, and with it the
  fin's top rises 0.1115 rather than 0.15. Only `ease = smoothstep` is the mesh
  Draw's profile.
- **Relief's stepping bound is already poor at stroke density**: 30 overlapping
  dabs declare L = 46 on the whole-document tape. That is additive and per
  record, not overlap-aware; it is not this change's to fix, but a draw-frame op
  would start from the same place.
