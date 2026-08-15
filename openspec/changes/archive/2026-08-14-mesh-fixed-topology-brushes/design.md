# Design: fixed-topology mesh brushes

## Module placement, and the one dependency that is not allowed

`voxel` already depends on `mesh` (`VoxelGrid::mesh_greedy` returns a
`mesh::Mesh`). So **`mesh` may not include `voxel`**, and the falloff curve
cannot simply be `voxel::BrushFalloff`. `mesh/sculpt.h` declares its own
`MeshFalloff` with the same four curves and the same values, and says in the
header that the duplication is the layering, not an oversight.

The mask is `voxel::MaskField`, so it cannot enter `mesh/` either. It does not
need to: the verbs take a `field::MaskGate` (`std::function<float(cfloat3)>`,
already the mask hook `relax` and `flatten` use, and `mesh` already depends on
`field`). `brush::apply_to_mesh` — which lives in `brush`, which already
depends on `voxel` — is where a `MaskField*` becomes that gate. One conversion,
one place, and every verb is masked without knowing what a mask is.

```
  math ── kernel
    │
  field ──────────┐
    │             │
  mesh ── adjacency, sculpt, bvh
    │             │
  voxel ──────────┘
    │
  brush ── stroke.h::apply_to_mesh   (sees mesh AND voxel; the join)
    │
  pick  ── raycast_mesh
```

## Adjacency: weld classes, not raw vertices

A real asset is a triangle soup with **split vertices**: a UV seam, a hard
edge or a per-face normal duplicates a position into two or more independent
indices. A one-ring built over raw indices stops at every seam, so smoothing a
head with a UV seam down the back produces a visible crack along it, and a
geodesic walk cannot cross it at all.

So adjacency is built over **weld classes**: vertices sharing a position (to a
quantization epsilon) are one class, and the ring is the class graph. Every
verb computes one displacement per class and writes it to **all members of that
class**, which is what keeps coincident duplicates coincident. A crack is
unrepresentable rather than merely untested.

Storage is CSR throughout — `class_offsets/class_members`,
`ring_offsets/ring_neighbours`, `vtri_offsets/vtri_triangles` — three flat
index pairs, built in one pass, no per-vertex containers.

`Adjacency::matches(const Mesh&)` compares vertex and index counts and is what
every entry point asserts instead of trusting a caller. Fixed topology means
this can never legitimately fail mid-feature; it fails when a caller reused an
adjacency across two different meshes, which is a caller bug worth naming.

### The quantization epsilon

Welding on exact float equality would miss vertices a DCC wrote at
`0.1000000015` and `0.1000000002`. Welding too loosely fuses a thin wall to
itself. The default is `1e-5` **relative to the mesh's bounding-box diagonal**,
so it does not change meaning when a model is authored in millimetres. It is a
parameter, and 0 means exact-bit welding for a caller who knows their mesh.

## The region a brush reaches

The Move Topological lesson: a brush on the upper lip must not move the chin,
even though the chin is within the Euclidean radius through the closed mouth.

A class is in the region when a path over the one-ring leads to it that **(a)
never leaves the ball of the brush radius, and (b) is itself no longer than a
path budget** — 1.6 × the radius by default. The walk is Dijkstra over the class
graph with Euclidean edge lengths, seeded at the class nearest the brush centre;
ties in the frontier break on class index, so it is deterministic.

**The weight comes from the STRAIGHT-LINE distance, not from the path.** This
pair of decisions was arrived at by looking at renders, and both halves are
load-bearing:

- Weighing by the path length was tried first. An edge path overestimates true
  geodesic distance by a direction-dependent amount, and the falloff picks that
  up as visible **herringbone banding** across the stamp — obvious in a render,
  invisible in any number a test was checking. The straight-line distance
  carries no such bias, and it is bounded by the path length, so a class the
  walk admitted is inside the radius in space too.
- Bounding the region by path length alone was tried next. The same
  overestimate makes the walk stop *short* in some directions and not others,
  which leaves a **ragged rim** where the falloff was supposed to reach zero
  smoothly. On a structured grid the overestimate reaches √2, because a
  diagonal direction has to zigzag; on a marching-cubes mesh it is a few
  percent. Bounding by the ball removes it outright: the rim is now the
  falloff's own zero.

What survives is exactly the property the walk exists for. The chin is inside
the ball, but every path to it leaves the ball or exceeds the budget, so it is
not in the region and its weight never comes up. The budget is what
distinguishes this from a plain connected-component flood: without it, a path
that runs the long way round the mouth *inside* the ball would admit the chin.

The frontier is a binary heap over classes with a `dist` scratch array reused
across stamps (a stroke resolves hundreds of stamps; reallocating a per-class
array each time is the whole cost).

Euclidean mode stays available and is the default for `flatten` and `scrape`,
where the artist means "everything under this disc" and a surface walk would
refuse to flatten across a groove. On a connected sheet the two modes now agree
bit for bit, which is asserted.

## One stamp = gather a region, then apply

```cpp
BrushRegion gather_region(const Mesh&, const Adjacency&, const MeshBrushSettings&,
                          const field::MaskGate& gate, Scratch&);
void apply_stamp(Mesh&, const Adjacency&, MeshBrush, const MeshBrushSettings&,
                 const BrushRegion&, VertexDeltas* record);
```

`BrushRegion` is the **pre-stamp snapshot** and carries everything a verb needs
to be a single operation rather than a sequence: the reached classes, their
weights (falloff × pressure × `1 - mask`), their positions and normals *as they
were before this stamp*, the region's area-weighted average normal, its
centroid, and its best-fit plane (from the covariance of the weighted
positions, smallest eigenvector by one Jacobi sweep).

That snapshot is what makes the header's promises true:

- **draw** displaces along `region.average_normal`, taken before it deposits
  anything, so a stroke does not chase its own deposit into a balloon.
- **inflate** displaces along `region.normals[i]`, per vertex. That one
  difference is the whole distinction between the two brushes, and it is why
  they are separate verbs rather than a flag.
- **smooth** reads neighbour positions from the snapshot where the neighbour is
  in the region and from the mesh where it is not, so the Laplacian is a true
  simultaneous average and not a Gauss-Seidel sweep whose result depends on
  vertex order.
- **scrape** is flatten-cut-only and smooth from **one** snapshot, mirroring
  `sculpt_scrape`.
- **crease** is a tight negative draw and a pinch in **one** stamp: sequenced
  separately they leave a rounded ditch, because the pinch would gather
  vertices that the draw had already pushed down.

## The verbs, as vertex math

For a class `i` with weight `w` (falloff × strength × `1 - mask`), pre-stamp
position `p`, pre-stamp normal `n`, brush centre `c`, radius `r`:

| verb | displacement |
|---|---|
| `grab` | `w * delta` — `delta` is the stroke's per-stamp motion, supplied by the caller |
| `draw` | `w * strength * r * region.average_normal` |
| `inflate` | `w * strength * r * n` (signed) |
| `smooth` | `w * (laplacian(p) - p)`, `laplacian` = one-ring mean |
| `pinch` | `w * strength * ((c - p) - n * dot(c - p, n))` — the TANGENTIAL part only, so a pinch gathers along the surface instead of sinking the region. Negative strength spreads (magnify) |
| `flatten` | `w * strength * (proj_plane(p) - p)`, clamped by `FlattenMode` to the cut side, the fill side, or neither |
| `clay` | a FILL-ONLY flatten onto a plane floating `strength * r` along `average_normal` from the region's centroid: material is added up to the plane and no further, which is what makes flat-topped strips instead of a swell. On a flat surface it is indistinguishable from `draw`; the difference is what it does to an uneven one |
| `crease` | `draw` with a negative amplitude and a squared falloff, plus `pinch`, summed before either is written |
| `scrape` | `flatten(CutOnly)` plus `smooth * 0.5`, summed from one snapshot |
| `polish` | `smooth * gate(i)`, `gate` = `1` where the one-ring's normals agree within `polish_angle` and falling to 0 at twice it — a hard edge disagrees, so it survives |
| `snakehook` | `grab` with the falloff re-centred on the DRAGGED position each stamp, so the region walks with the pull instead of snapping back |

Every one of them writes `positions` and nothing else. `indices` and `quads`
are not touched by any code path in this change; the tests assert both are
byte-identical after a stroke.

## Normals

A moved vertex with a stale normal shades wrong immediately, so normals are
recomputed — but only for the **touched region plus its one-ring**, because a
triangle's normal changes when any of its three corners moves.

`recompute_normals(Mesh&, const Adjacency&, span<class ids>)` does area-weighted
accumulation over each affected vertex's incident triangles. It runs per stamp
by default and can be deferred to the end of a stroke
(`MeshStrokeOptions::defer_normals`), which is the host's choice the issue asks
for: per-stamp is correct for a live preview, per-drain is faster.

A mesh with **no** normals stays a mesh with no normals — recomputing them
would change what the layer exports, and `Mesh::normals` is documented as
optional.

## Vertex-delta undo

```cpp
struct VertexDeltas {
    std::vector<std::uint32_t> vertices;
    std::vector<kernel::cfloat3> before_position, after_position;
    std::vector<kernel::cfloat3> before_normal,   after_normal;   // empty if the mesh has none
};
```

Sparse: only vertices a falloff actually reached. **Coalesced per gesture** —
a vertex touched by forty stamps of one stroke appears once, keeping the FIRST
`before` and the LAST `after`, via a class→slot scratch map. That is what makes
one stroke one undo step, and it is bounded by the vertices the stroke reached
rather than by the stamps it took.

Normals are recorded, not recomputed on revert. An imported mesh's normals are
whatever its author wrote; recomputing them on undo would restore a mesh that
is *geometrically* identical and *byte* different, and the acceptance bar is
bit-exact.

`revert` walks backwards and writes `before`; `apply` walks forwards and writes
`after`. Both are idempotent. Neither touches `indices` — there is nothing to
restore, which is the contract paying off.

`Mesh` is not a `scene::Command`, and this change does not make it one — the
voxel verbs have the same gap and it is a known roadmap item. A host coalesces
`VertexDeltas` per gesture and keeps its own stack, exactly as it does for
voxel edits.

## Mesh picking

`Bvh` gained nothing but an index: `build` already permutes triangles into
`tris_`, so it now carries `tri_index_[i]` = the triangle number in the source
mesh. Distance and winding queries do not read it and are unchanged.

`Bvh::raycast(ray, tmax)` is an ordinary front-to-back BVH traversal with
Möller–Trumbore at the leaves, returning `t`, the source triangle index and the
barycentrics. `pick::raycast_mesh` wraps it with the layer transform (the ray
goes into layer space, the hit comes back out) and interpolates the shading
normal from the mesh's own normals when it has them, falling back to the
geometric normal when it does not.

Back-face culling is **off**: a sculptor pulling on the inside of a shell means
it.

## Threading

Per-class application partitions cleanly and there are no read-write hazards
inside one stamp (every write target is distinct, and every read is from the
snapshot). It lands under `add-mobile-thread-scheduling`'s rules — byte
identity with the serial path, no new public locks — but this change ships the
**serial** path and the determinism test that a parallel one would have to
match. Parallelizing before the semantics are asserted is how byte identity
gets lost.
