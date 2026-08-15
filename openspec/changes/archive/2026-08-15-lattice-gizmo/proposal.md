# One cage over a layer

## Why

`lattice-on-sdf-items` gave an item a cage in its OWN local space. A gizmo does
not work that way: ZBrush's acts on the whole subtool, and #116 says so —

> A gizmo lattice over a layer wants the same treatment: one cage in world
> space, resolved into per-item lattice deformers.

The treatment it means is `brush::move_brush`, which turns one world-space drag
into the per-item `grab` that reproduces it. Without the equivalent, a lattice
is a per-item modifier: placing one over a blocked-out form means authoring a
cage per item, in each item's own frame, and keeping them in step by hand.

## What Changes

`brush::lattice_gizmo` — one world-placed cage in, one lattice deformer per item
out, each expressed in that item's own frame.

### The thing that made it need a kernel change

An item's frame can be ROTATED, and a lattice box is axis-aligned by
construction. A world-axis-aligned cage is not axis-aligned in a rotated item's
local space, so there is no per-item box that reproduces it. Resampling the
world cage onto a per-item grid would be an approximation, and an avoidable one.

So the deformer gains an optional TRANSFORM, carried in the blob beside the
offsets: the point is mapped into cage space, warped there, and mapped back.

```
p' = T⁻¹( T(p) + D(T(p)) )
```

Exact for any `math::Transform`, which is rigid with uniform scale by design —
the same property `move_brush` relies on to keep a spherical falloff spherical.

**It is a separate opcode, not a flag.** The axis-aligned lattice keeps its own
path and pays nothing, because adding per-sample work to a path that does not
need it is exactly what #137 and #140 had to undo.

### Two things fall out of the transform being rigid with uniform scale

**The Lipschitz factor is unchanged by it.** With `T = sR`, the warp's Jacobian
in local space is `R⁻¹ J R` — similar to the cage-space Jacobian, so the same
norm. The bound `cfi_lattice` already computes is the right one, and the spec
says so rather than leaving a reader to wonder whether the transform needs a
term.

**The influence hull scales by `1/s`.** A displacement bounded by the largest
offset in cage space is bounded by that over `s` in local space.

### Reachability, which is not `move_brush`'s

`move_brush` skips items a drag cannot reach, because `grab` has finite support.
A lattice does not: outside its box the displacement is CLAMPED, not zero, so
material out there travels rigidly. A gizmo cage therefore reaches **every item
in the layer**, by design, and the resolver says so rather than quietly
inventing a cutoff. Only an untouched cage resolves to nothing.

## Impact

- `sdf-kernels` — the transformed lattice opcode and its blob layout
- `brush-engine` — the resolver
- `bindings` — `pyclay` and the C ABI

## Out of scope

**Mesh layers.** A mesh layer already takes a cage directly and forward, through
`mesh::Lattice`, and exactly. A gizmo spanning both would resolve the SDF layers
through this and the mesh layers through that; the composition is a host's to
make, and inventing a document-wide entry point before one exists would be
guessing at its shape.
