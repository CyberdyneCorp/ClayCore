# Proposal: carry a mesh's colours and uvs across a round trip

## Why

A mesh layer's whole reason to exist is that it holds triangles somebody meant:
a retopology, a UV layout, painted colour. Sculpting one preserves all of that,
because the mesh brushes move vertices and never polygons.

Anything that leaves the mesh layer does not. `Volume.from_mesh` samples the
model onto a lattice, and the mesh that comes back out of a mesher is new
geometry with new vertices: **the shape survives, and the colours and uvs are
gone.** That is the price the README quotes for composing a mesh, and it is the
price a host pays for any trip through the field — boolean, consolidate,
remesh, level change.

Most of that price is refundable. The nearest point on the ORIGINAL surface
knows what colour and uv belonged there, and `Bvh::closest` already returns the
triangle and the barycentrics needed to read it. Its own header says so:

> What the extra information buys is any ATTRIBUTE transfer — a colour, a uv, a
> normal — read from the triangle the closest point landed on and interpolated
> by its barycentrics.

`VoxelGrid::rasterize_mesh` is the first caller and does exactly this for
colour, in ten lines (`mesh_colour_at`). This generalises that one function
into an operation a host can ask for.

## What changes

`mesh::transfer_attributes(const Mesh& source, Mesh* target, options)` — for
each of the target's vertices, find the closest point on the source and
interpolate the source's attributes there.

It is deliberately NOT a round-trip-only helper. "Give this mesh the colours
and uvs of that one" is also how a decimated LOD keeps its polypaint, how a
re-levelled voxel mesh keeps colour, and how any two meshes of the same object
are reconciled.

### Four decisions the implementation has to make

**Colours and uvs by default; normals only on request.** Transferring normals
is usually WRONG and the default should say so. A resampled mesh has its own
geometry, and its normals should describe it; taking the source's would make a
new shape shade like the old one. The option exists for the case where the two
meshes are near-identical and the source's normals were authored, not computed.

**Positions are never touched.** This is an attribute transfer, not a
projection or a shrinkwrap. A verb that moved vertices toward the source would
be a different operation, and conflating them is how "transfer" quietly becomes
"deform".

**A target vertex too far from the source gets a fallback, not a lie.** After a
boolean, geometry exists that was nowhere near the original surface, and the
closest point to it is meaningless. Past a caller-supplied distance the vertex
SHALL take a documented fallback rather than the attribute of whatever happened
to be nearest, and the call SHALL report how many vertices that was — a silent
75%-fallback result is indistinguishable from a good one.

**The uv seam limitation is stated, not discovered.** `Mesh` carries uvs per
VERTEX, which is how a seam is represented at all: the source duplicates a
position into two vertices with different uvs. A target vertex sitting on that
seam has one uv slot and two right answers, so it gets whichever triangle the
closest-point query returned — and a triangle spanning the two lands stretched
across the whole layout. Colour has no such problem, because colour is
continuous across a seam. This is a property of per-vertex uvs and not a bug to
be fixed later, so it belongs in the requirement.

## What this does NOT give back

**Topology.** The target is still the mesher's geometry: new vertices, new edge
loops, no relationship to the retopology that went in. Attribute transfer
refunds the paint and (mostly) the uvs; it does not refund the mesh.

That distinction is the whole reason this is worth doing INSTEAD of mesh-level
booleans rather than as a step toward them — see the roadmap entry recorded
alongside this change. If a workflow needs the topology back, no amount of
attribute transfer substitutes and the answer is a different feature.

## Impact

Additive. One new engine entry point plus its bindings; nothing existing
changes behaviour. `mesh_colour_at` in `src/voxel/grid.cpp` becomes a caller of
the shared implementation rather than a private duplicate of it.
