## Why

**A coarse surface is face-shaded by construction, and face normals are up to
84.78 degrees wrong.**

`clay_brick_cache_mesh_lod` refused gradient normals at any level above 0. A
host drawing a coarse LOD therefore had exactly one option — `CLAY_NORMAL_FACE`
— and no way to ask for anything better. ClaySpaceDesktop's own comment records
what that looks like from the other side:

> Level 1 refuses gradient normals rather than downgrading them, so the coarse
> surface is face-shaded by construction.

Measured on a worked sphere with `benchmarks/brick_mesh_holes_probe.cpp`,
against the field's own gradient at the mesh's own vertices:

```text
CLAY_NORMAL_FACE      24,116 vertices, 5,179 over 5 deg, worst 84.78 deg
CLAY_NORMAL_GRADIENT  24,116 vertices,     0 over 5 deg, worst  0.00 deg
```

## The refusal was right about the cull, not about the gradient

Its reasoning, in the header's words: a coarse vertex sits on the MIP's surface
rather than the field's, "which can be most of a coarse cell off it, where the
culled tape and the full one are only both out-of-band rather than equal."

That is an argument against the **per-brick culled tape**, and it is correct.
It is not an argument against the gradient. The whole document's tape is not
band-clamped, so it still has a real gradient at exactly that point — the
flatness is a property of the cached lattice, not of the field.

So a level's attributes are evaluated through the whole-document tape.

## What it costs, stated rather than buried

**A level's attribute pass no longer follows the bricks named.** Issue #73
bought that property for lod 0 and it is kept there unchanged; at a level it is
given up deliberately, because the alternative a host actually has is a normal
84 degrees from the surface.

**Colours stay refused at a level**, and for a reason gradient normals do not
share: the mip carries no colour lattice of its own, which
`clay_brick_cache_read_bricks` already reports rather than averaging. Nothing
supplies one, so there is nothing to downgrade from. That refusal is unchanged.

## What building it found

**`CLAY_NORMAL_FACE` is area-weighted and still wrong.** `compute_face_normals`
accumulates the UNNORMALISED cross product per vertex, whose length is twice the
triangle's area, so the weighting is intrinsic and a near-zero-area triangle
contributes near-zero. Reading that, I concluded a sliver could not poison a
face normal and that this whole change was unnecessary. The measurement above
says otherwise, and it was taken before the conclusion was acted on.

**Excluding slivers from the normal accumulation fixes nothing**, and was
scored before being proposed rather than after:

```text
848 of 48,228 triangles excluded
all triangles      worst 84.78 deg,  5,179 over 5 deg
slivers excluded   worst 84.78 deg,  5,161 over 5 deg
```

The worst case does not move. The face-normal error is marching-cubes
tessellation — a geometric normal on a coarse lattice genuinely differs from
the field's gradient — and not the degenerate triangles. A change that removed
every sliver would have left this untouched.

## What changes for a caller

Additive. A call that previously received `CLAY_ERROR_INVALID_ARGUMENT` now
returns a mesh; nothing that previously succeeded answers differently, and lod 0
is byte-identical. A caller relying on the refusal to detect a level would be
relying on an error as a feature.
