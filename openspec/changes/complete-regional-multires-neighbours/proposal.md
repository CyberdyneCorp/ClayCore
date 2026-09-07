## Why

**A regional multires level's positions are bit-identical to the dense
hierarchy's, and its NORMALS AND FRAMES are not.** Measured here, independently
of the roadmap entry that found it, on a 4x4 quad cage with the four centre
patches refined and compared corner-for-corner against a hierarchy refined
everywhere to the same level:

| level | corners compared | worst position error | worst normal error | corners with a wrong normal |
|---|---:|---:|---:|---:|
| 1 | 64 | **0.000000000** | **0.406** (23.4°) | 20 |
| 2 | 256 | **0.000000000** | 0.209 (12.0°) | 52 |
| 3 | 1024 | **0.000000000** | 0.103 (5.9°) | 116 |

The wrong ones are exactly the vertices on the region boundary, and the cause is
one line: `level_normals` sums the face normals of `conn.faces_of(v)`, and at a
region boundary that ring is INCOMPLETE — the faces on the unrefined side are
not stored at this level, so half the ring is missing and the average leans.

**This reaches storage, not display.** A multires surface is
`P(n) = S(n) + Frame · Detail`. A frame 23 degrees off means a coefficient
authored at a boundary vertex reconstructs to a different world offset than the
same coefficient on a dense hierarchy. So the shipped bit-identity gate holds
only while boundary detail is ZERO, which is the only case it exercises. Sculpt
across a region boundary and the guarantee is gone, silently.

It is not reached through export — no host exports hierarchies — it is reached
by SCULPTING near a boundary, which is the ordinary use.

## What Changes

**One neighbourhood, computed once, consumed by everything that needs a ring.**
Not a transition rule taught separately to normals, to Smooth and to every
future brush.

A regional level gains a `LevelHalo`: the child faces of the parent faces the
level did NOT refine, restricted to those touching a vertex the level DOES
store, plus those faces' own vertex positions.

**It is the existing machinery, not a second hierarchy.** The halo faces are
emitted by the same loop `subdivide_topology_for_patches` runs — the same
`ChildLayout`, the same corner order — and their positions come from the same
`subdivide_positions` stencils against the same parent. So a halo vertex holds
what the dense level holds at that point, bit for bit, for the same reason a
stored vertex does.

**The parent always has what the halo needs.** `add_level_for_patches` refuses
with `PatchNotRefinable` unless every patch sharing a vertex with a refined one
is resident at the parent, and `refine_patches_to_level` grows each intermediate
level by the rings the levels above need. The one-ring the halo reads is exactly
the ring that guarantee already provides.

**Normals and frames first, and nothing else in this change.** The consumer
order is deliberate: a visually acceptable Smooth over a wrong frame still
reinterprets authored detail, so the frame is fixed before anything that reads
one. Smooth, Relax and the neighbour-dependent automasks follow in their own
change, over the same object.

**Cost is the boundary, not the region.** The halo is one face-ring wide by
construction — a face is in it only if it touches a stored vertex — and it is a
REBUILDABLE cache in `LevelCache`, released by `drop_all_caches` and counted in
`MultiresMemory::runtime_index` with the rest of the per-level runtime.

**A dense level builds no halo at all**, which keeps every existing uniform
hierarchy on exactly the path it is on today, byte for byte.

## Capabilities

### Modified Capabilities
- `mesh-multires`: a regional level's normals and detail frames are the dense
  hierarchy's, and the neighbourhood that makes them so is one object rather
  than a rule per consumer.

## Impact

- `include/clay/mesh/subdivide.h`, `src/mesh/subdivide.cpp` — the halo topology
  builder, beside the level builder it mirrors.
- `src/mesh/multires_internal.h`, `src/mesh/multires_eval.cpp` — the cached
  halo and the normal/frame path that consumes it.
- `include/clay/mesh/multires.h`, `src/mesh/multires.cpp` — memory reporting.
- `tests/unit/test_multires_regional.cpp` — the gate, against the dense
  hierarchy as oracle.
- No ABI change, no format change: the halo is derived and droppable, and
  nothing about a level's stored vertices, its detail or its serialization
  moves.
