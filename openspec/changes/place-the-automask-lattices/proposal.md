## Why

**A painted cavity mask and a cavity automask protected different crevices of
the same surface, on any layer that had been moved.**

`brush::apply_to_mesh` builds both from the same estimator, and `mesh/automask.h`
says why that matters:

> The requirement is that a painted cavity mask and a cavity automask cannot
> disagree about one surface, and the way to guarantee that is to have exactly
> one estimator.

The estimator was one. The QUESTION was two. Both the cavity field and the group
lattice are world-addressed, and a sculptor's vertices are the mesh's own, so a
vertex has to be placed before either is asked. `mesh_mask_gate` does that:

```cpp
const math::Transform to_world = options.mesh_to_world;
return [mask, to_world](kernel::cfloat3 p) { return mask->sample(to_world.apply(p)); };
```

`mesh_automask_inputs`, three lines below it in the same anonymous namespace,
did not — it sampled `measure_at(field, ..., p, measure)` and `groups->at(p)` at
the raw mesh-space point. One estimator does not prevent a disagreement when its
two callers ask it about different points.

**Reachable from pyclay today**, which is why this is not waiting behind the C
ABI work that found it. `MeshStrokeOptions` has carried `mesh_to_world` since it
shipped; nothing here needs a new entry point.

**And a second one beside it.** `read_mesh_brush` treated
`automask_cavity_strength` the way it treats the two appended scalars either side
of it — "zero means the caller declared an older layout, give them the engine
default". That rule is right for a normal angle and a ring count and wrong for
this one, because it is a SLIDER and zero is a value a host MEANS.
`mesh/automask.h` again:

> Cavity: how much of the caller's cavity measure to apply. 1 masks a full
> crevice completely; 0 is off even when the factor bit is set, **which is what
> a host's slider at zero should cost**.

Off is what it should cost. Full is what it cost.

## What Changes

**The vertex is placed before the cavity field and the group lattice are asked**,
by `options.mesh_to_world`, exactly as the painted mask already is.

**`automask_cavity_strength` is passed straight through**, on the footing
`stamp_azimuth` three fields below it already had, and its documented default
becomes 0 — off — with `clay_mesh_brush_defaults` supplying the engine's 1.0.

**The descriptor rule gains the distinction that was missing.** `struct_size` is
the signal for "the caller did not declare this field". The field's own VALUE is
not that signal, and using it as one is undetectable for any field whose zero is
meaningful.

## What This Does Not Do

Nothing here makes `CLAY_AUTOMASK_CAVITY` or `CLAY_AUTOMASK_SURFACE_GROUP`
reachable from C — they remain inert from that binding, as `clay.h` says at the
field. That is a separate change with its own entry points and its own version
bump. This one is the two defects underneath it, which are reachable from C++
and pyclay now and should not wait behind an ABI addition.

## Measurements

The placement, on a C++ stroke over a layer placed 5 units along +X, with a
cavity field whose crease sits where the layer ends up. Probed at five points
across the stamp:

| probe | placed (this change) | unplaced (before) |
|---|---|---|
| x = -0.4 | 1.000000 | 0.461912 |
| x = -0.2 | 1.000000 | 0.479507 |
| x =  0.0 | 1.000000 | 0.498629 |
| x =  0.2 | 1.000000 | 0.519419 |
| x =  0.4 | 1.000000 | 0.541735 |

**The wrong answer was not obviously wrong.** Five units clear of the shape the
estimator still returns something plausible — around a half — so a host saw a
cavity automask that half-protected everything rather than one that visibly did
nothing. That is considerably harder to notice.

The strength, through the round trip that could see it:

| saved | deserialized before | deserialized now |
|---|---|---|
| 0.00 | 1.00 | 0.00 |
| 0.25 | 0.25 | 0.25 |
| 0.50 | 0.50 | 0.50 |
| 1.00 | 1.00 | 1.00 |

## The Header Said "Only The Mask", And Was Right About The Code

`clay_mesh_sculptor_apply_stroke` documents its `mesh_to_world` parameter as:

> `mesh_to_world` is the layer transform and is used ONLY to find each vertex on
> the mask's world-addressed lattice; NULL means identity. Everything else here
> is in the mesh's own space.

That is a TRUE description of the code and a FALSE description of what the code
needs. There are three world-addressed lattices — the painted mask, the cavity
measure, the group field — and the transform reached one, with the sentence
naming that one as though it were the design rather than the omission.

**The documentation and the code agreed with each other and both were wrong**,
which is why review found nothing: a reader checking the code against the
comment finds them consistent. Reworded here to name the transform's reach as a
SET rather than as a single lattice, so the next world-addressed thing added to
this call is not the next instance of this bug.

## What Building It Refuted

**A single sphere cannot see the placement bug.** The regression case was first
written on the sphere the existing cavity test uses, and passed — a sphere is
CONVEX, so its cavity measure is zero at the placed point and the unplaced one
alike, and every assertion in it held with the fix deleted. The case carries an
explicit `any_differed` assertion for exactly that reason: the two sides being
compared must actually differ somewhere, or the comparison above proves nothing.
That assertion is what failed. The fixture is two overlapping spheres now, whose
union has a concave seam.

**Every existing automask fixture leaves its layer at the origin**, where
`mesh_to_world` is the identity and the error is exactly zero. That is why this
survived — not subtlety, but that nothing had a reason to move the layer.

**The stamp cannot see the strength bug.** `CLAY_AUTOMASK_CAVITY` is inert from
C, so the value reached `MeshBrushSettings` and was never consulted. A preset
round trip consults it, and that is what the regression test uses. A test that
stamped would have passed with the bug in place.

## Verification

- `cpu-only` suite green.
- `test_mesh_sculpt.cpp` adds two cases for the placement, `test_c_brush_preset.cpp`
  one for the strength.
- Both proven by reverting: all three cases fail, 0 of 3 passing. The placement
  cases report the probe values above and `applied > 0` reading 0 — a stroke
  that reached nothing, because every vertex read group 0 at the origin. The
  strength case fails on the zero and on the loop.
- No version bump: no entry point is added, and both are fixes restoring
  documented behaviour.
