# A lattice on a mesh layer

## Why

The last of ZBrush's four Gizmo 3D deformers, and #116 records it as the hard
one: *"forward FFD has no closed-form inverse"*. The issue lists three answers,
all of them compromises, because it assumes the target is an **SDF item** — and
a claycore SDF deformer is an inverse point map, so a lattice there must be run
backwards.

That assumption no longer holds. **ZBrush and Blender do not invert anything**,
because neither has the problem: both deform explicit mesh vertices, so FFD runs
*forward* — find each vertex's parametric position in the cage, evaluate the
basis, move it there. Blender's Lattice modifier is a per-vertex forward warp;
ZBrush's Gizmo Lattice acts on the PolyMesh3D's points, which #116 itself notes
("its deformers are mesh-only").

ClayCore now has **mesh layers with fixed topology**. `MeshSculptor` moves
vertices and records undo, and nothing in it can change `indices`. A lattice is
another way to produce those vertex moves. So the ZBrush/Blender feature is
available with **no inverse, no approximation, and no tape change at all**.

## What Changes

`mesh::Lattice` — a cage of control-point OFFSETS over a box, and
`MeshSculptor::apply_lattice`, which moves every vertex through it.

Offsets rather than positions, and that choice carries the whole design: the
warp is

```
new_position = p + Bezier(offsets, clamp(s, t, u))
```

so a cage nobody has touched is **exactly** the identity, at every point,
without a special case. A vertex inside the box gets full forward FFD; the
clamp means a vertex outside travels rigidly with the nearest point of the cage
rather than collapsing onto it, which is the same "held beyond it" convention
`twist_range` and `bend_range` established.

**Trivariate Bernstein**, one formula for every cage size. Degree is one less
than the control-point count per axis, so a 2×2×2 cage is exactly trilinear and
a 3×3×3 is quadratic — no modes, no separate linear path. Bernstein also
*interpolates the corners*, so dragging a corner control point moves that corner
of the box exactly, which is what a lattice UI should do.

Cage resolution is free per axis (≥2, default 3×3×3), because the reason #116
suggested a fixed 3³ was the **tape record's slot budget** — and a mesh lattice
has no tape record. The cost of the freedom is stated rather than hidden:
Bernstein support is global per axis, so on a large cage one control point still
moves everything a little.

## Impact

- `meshing` — the cage and the sculptor verb
- `bindings` — `pyclay` and the C ABI
- Undo is `VertexDeltas`, the record the brushes already produce, so a lattice
  is one undo step

## Out of scope

**A lattice on SDF items** — #116's original subject, and still open. It needs
an inverse and therefore one of that issue's three compromises; the blob-carried
payload it would need already exists from `bend-along-a-curve`. This change does
not decide it, and says so where a reader would otherwise assume the gap is
closed.
