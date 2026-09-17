## Context

`ccombine_relief` evaluates `a - k * w(b)` (and `ccombine_incise` the same
with `-k`, in the same branch) with
`w = smoothstep(1 - clamp(b / W, 0, 1))` and `b` the region's rounded field
(`include/clay/kernel/tape.h`, `ctape_combine_dist`). It is a function of the
accumulated value `a` at the sample point and of the region, which is why it
costs one record, keeps finite support, and declares an ADDITIVE slope term
`k * 1.5 / W` (`cfi_relief`).

The mesh sculptor's Draw and Inflate share `kernel_displace` and differ only in
`BrushFrame`: `RegionNormal` (one `deposit_direction` per stamp) or
`VertexNormal`.

## The probe

Two scripts drove pyclay built from this tree (`CLAY_BUILD_PYTHON=ON`, cpu-only
preset). They were measurement only and are not committed; the fin fixture
becomes the test in `tasks.md`.

**Frame probe.** Per fixture: check the relief model against the engine; mesh
the base shape at voxel 0.01; stamp copies with the engine's `draw` (explicit
`deposit_normal = N`) and `inflate` (`geodesic = False`, radius = reach,
`strength = k / reach`); build `inflate_ref` and `draw_ref` with relief's weight
solved at the moved point by a damped fixed-point iteration; score each surface
against each point set by `|f| / |grad f|` with a central-difference gradient.

**Stroke probe.** 1, 10 and 30 dabs along an arc over a unit sphere and over a
24-item smooth-unioned blockout, each dab on the surface with `N` the field
gradient there; as relief items versus as `move_surface(c, k*N, reach,
ease=smoothstep)`. Reports records, `safe_step_scale`, `warp_cost`, the best of
five whole-document evaluations of 200k points in a shell around the form, and
the rise at mid-stroke.

## Results the decision rests on

Distance to draw_ref, as a fraction of `k`, at the standard mapping:

| candidate | sphere mean | saddle mean / p95 | bowl mean | fin mean / p95 |
|---|---|---|---|---|
| Relief (engine) | 0.017 | 0.077 / 0.204 | 0.027 | 0.568 / 1.091 |
| warp `a(p - k w N)` (model) | 0.000 | 0.000 / 0.000 | 0.000 | 0.000 / 0.000 |
| first order `a - k w (n . N)`, `n` the normalized gradient (model) | 0.002 | 0.012 / 0.043 | 0.004 | 0.129 / 0.733 |
| `move_surface`, smoothstep ease, mesh profile (engine) | — | — | — | identical to the warp model with that profile |

Stroke cost, 24-item blockout:

| stroke | records | safe step scale | eval 200k points | rise at mid-stroke |
|---|---|---|---|---|
| none | 24 items | 1.0000 | 4.3 ms | — |
| 1 dab relief | +1 item | 0.4000 | 4.4 ms | +0.163 |
| 1 dab move_surface | 20 warps | 0.6667 | 6.4 ms | +0.123 |
| 10 dabs relief | +10 items | 0.0625 | 5.8 ms | +0.242 |
| 10 dabs move_surface | 233 warps | 0.0173 | 28.3 ms | +0.165 |
| 30 dabs relief | +30 items | 0.0217 | 8.8 ms | +0.352 |
| 30 dabs move_surface | 700 warps | 0.0000 (1.5^-30) | 76.2 ms | +0.304 |

## Decisions

### Do not add a draw-frame combine op

A combine record sees `a` at `p`. The draw frame's exact form needs `a` at
`p - k w(p) N`; its first-order form needs `grad a` at `p`. The tape carries
neither, and making it carry either changes what a record is: a prefix
re-evaluated at a displaced point costs the prefix again per such record, so k
of them nest, and the prefix cache (`sdf-prefix-cache`) could no longer reuse a
prefix sample at `p`. The first-order form is also not sound to march: its
slope includes `k w * Hessian(a) . N`, and a box edge's curvature is unbounded,
so no declared Lipschitz term covers it — and it misses the fin by 0.733k at
p95 regardless.

### Do not route Standard through `move_surface` per dab

It is exact, and it is the engine's surface warp done properly (every
contributing item, front of chain). Per dab it is one warp per reached item,
the bound multiplies (1.5 per overlapping dab at the standard mapping), and the
evaluation cost grows with items times dabs: 8.7x relief's at 30 dabs. The Move
proposal (`steady-the-move-and-name-what-it-ignores`) reached the same
conclusion for grab — one deformation per stroke, never per dab.

### Name the approximation instead

Relief stays the SDF Inflate and is documented as such. Standard on an SDF is
Relief with a measured, stated error that grows with the spread of normals
under the stamp: 0.02-0.10k mean on the sphere, saddle and bowl at the standard
reach (k = 0.05 and 0.15), about 0.57k mean on the saddle and bowl once region,
rounding and amplitude are all doubled, and the whole amplitude on a feature
narrower than the stamp.

## Alternatives recorded for a later proposal

- **A stroke-level warp**: one deformer per reached item per STROKE, carrying
  the dab polyline and summing `k w_i N_i`. Keeps the draw frame exact, makes
  records O(items) per stroke rather than O(items x dabs), and its slope is one
  bound for the stroke rather than a product. It is a new deformer kind with a
  variable-length payload — scene minor, ABI and parity work — and needs its own
  measurement of the per-sample cost of a long polyline.
- **A draw-frame stamp on the working volume** of an SDF sculpt transaction
  (`session/sdf_sculpt.h`), where a warp is a resample rather than a record.
  It bakes, which is the trade the Smooth transaction already makes.

## Risks

- A host that reads the corrected header may move Standard to
  `clay_layer_move_surface` per dab. The header states the stroke cost beside
  the suggestion so the single-stamp use is the only one offered.
