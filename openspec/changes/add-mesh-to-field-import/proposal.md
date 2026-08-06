# Proposal: mesh to field import

## Why

The library can already *load* a mesh — OBJ, PLY and FBX all land in
`mesh::Mesh` — and can now hold a sampled field. What is missing is the step
between: turning a triangle soup into a distance field so an imported model can
be sculpted, blended and cut like anything else the engine made itself.

Without it, an imported mesh is something you can display and export, not
something you can *work on*, which is the only reason an artist imports one.

## The hard part is the sign, not the distance

Unsigned distance to a triangle soup is a closest-point query: build a BVH,
descend nearest-first, prune. It is well understood and it is not where this
change earns its keep.

The sign is. The obvious methods each fail on the meshes people actually
import:

- **Ray casting / parity.** Count crossings along a ray. A single hole in the
  mesh flips the answer for an entire half-space behind it, and a ray that
  grazes an edge or hits a shared vertex counts twice or not at all.
- **Closest-point pseudonormal.** Exact for a closed, clean mesh, and
  meaningless near a hole: the closest triangle to a point inside the model may
  be one facing away, because the wall it should have hit is missing.

Real assets are not watertight. They have holes, duplicated faces, flipped
normals, and self-intersections. A method that is correct on clean input and
catastrophic on dirty input is the wrong method, because dirty input is the
input.

So the sign comes from the **generalized winding number** (Jacobson et al.
2013): the sum of the signed solid angles each triangle subtends at the query
point, over 4π. For a closed surface this is exactly 1 inside and 0 outside.
For an open one it degrades *continuously* — near a hole it passes smoothly
through ½ — which is what makes it usable. A point is inside when the winding
number exceeds ½.

Summed exactly this is O(triangles) per query, which a narrow band cannot
afford: a hundred thousand samples against ten thousand triangles is a billion
solid angles. So the same BVH that answers the distance query also carries, per
node, the aggregate area-weighted normal and centroid of the triangles beneath
it. A node far enough from the query is summarized by a single dipole term
rather than descended (Barill et al. 2018). Near the surface — which is where a
narrow band spends its samples — the tree is descended and the answer is exact.

## What Changes

- **`mesh::Bvh`**: a triangle BVH over a `mesh::Mesh`, built by median split.
  It answers two queries: closest point, and generalized winding number. It is
  CPU-only and used at bake time, so it is an ordinary C++ module and not part
  of the kernel dialect.
- **`mesh::to_field`**: sample a mesh into a `field::FieldVolume`, choosing the
  region from the mesh's own bounds padded by the band.
- **Python**: `clay.Volume.from_mesh`, and loading a mesh straight to a volume.
- **The C ABI gains a volume producer**, which is what
  `add-sampled-fields` deferred: `CLAY_PRIM_VOLUME` stops being refused,
  because there is now something that can supply the samples. The exemptions
  recorded in the ABI and binding-parity gates come out with it.
- **An example** importing a mesh, sampling it, and sculpting the result.

## What this change does not do

- **No repair.** A mesh with holes is imported as the winding number reads it,
  not patched first. `mesh::validate` already reports what is wrong; deciding
  what to do about it is a separate concern from importing.
- **No UV or colour transfer.** Positions only; the field has no attributes.
- **No second-order expansion.** The dipole term with a distance cutoff, which
  is the standard accuracy/speed point. Higher moments are additive later.

## Capabilities

### Modified Capabilities

- `meshing`, `python-bindings`, `c-abi`.

## Impact

- New `include/clay/mesh/bvh.h`, `include/clay/mesh/to_field.h` and their
  sources; `mesh` gains `field` as an allowed dependency; the Python bindings,
  the C ABI, both gate tools, tests, docs, an example.
