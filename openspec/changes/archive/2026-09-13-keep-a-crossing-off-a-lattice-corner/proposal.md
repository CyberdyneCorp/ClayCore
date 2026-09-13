## Why

**The brick mesher emitted sliver triangles — near-zero-area triangles whose
face normal is a cross product of near-parallel edges, and therefore numerically
garbage.** On the fixture in `tests/unit/test_mesh.cpp` it produced **2,297 of
them in 83,464 triangles**; on the probe document, 1,006.

The cost is not cosmetic, and that is the part worth stating. A host hid them by
re-meshing the **whole field** after every completed stroke instead of the
bricks the stroke touched. Measured with `benchmarks/brick_mesh_holes_probe.cpp`
across a 48-dab sculpt:

```text
                        1 dab     48 dabs    degradation
clay_document_mesh      2.8 ms    26.2 ms    9.40x
clay_brick_cache_mesh   4.2 ms     6.3 ms    1.49x
```

`clay_document_mesh` evaluates densely with no cull, so its cost tracks the
document rather than the edit. The slivers were costing a host the cull. That
makes this a performance defect that happens to look like a shading one, which
is why it survived as "some black specks" for as long as it did.

## What it was

The crossing parameter, `t = f0 / (f0 - f1)`, landing at an edge **endpoint**.
Two edges of a cell that both cross at their shared corner produce two vertices
at the same point and a triangle with no area.

That is likelier in the brick path than in the tape path because a brick stores
`dim^3` **fp16** lattice samples clamped to a band: quantisation and band
clamping both drive a sampled value to exactly the stored neighbour's, and
`t` to 0 or 1.

Measured rather than assumed, with `benchmarks/sliver_origin_probe.cpp` — the
distance from each vertex to the nearest lattice plane along the axis it moves:

```text
sliver vertices      median 0.0178 voxels from the lattice
every other vertex   median 0.2576 voxels
```

A 14x separation is what said a guard on `t` was the fix and not a guess.

## What this is NOT

**Not a global change to the mesher.** The guard is on the **brick path only**:
`mesh_bricks` and `shell_corner_lattice`. `mesh_lattice` and
`mesh_lattice_parallel` — the tape path — pass no guard and are bit-identical.

A first version clamped everywhere and **broke 15 assertions** across
`test_mesh_weld.cpp`, `test_voxel_remesh.cpp` and `test_c_voxel_remesh.cpp`.
Those fixtures produce degenerate triangles **deliberately**, to exercise the
welder; clamping `t` removed the degenerates they were built to feed it. The
same version made `decimate` return a **non-manifold** mesh from a valid
watertight input, which is a fragility in the decimator rather than anything
about this change and is filed separately as #567.

So the scope is narrow because the wide version was tried and measured, not
because narrow was assumed safer.

## What changes for a caller

**Vertex positions move, by at most 5% of a voxel, on meshes built from bricks.**
Topology does not: the marching-cubes case index comes from **sign tests alone**
(`if (f[i] < 0.0f) inside_mask |= 1 << i`), so `t` cannot add or remove a
triangle. The fixture's triangle count is unchanged at 83,464 and the probe
document's at 47,532.

A host that pins rendered images will see them change — the black specks are
gone, which is the point, but a pixel comparison does not know that. A host that
pins vertex positions from a brick mesh will see them move within the guard.
