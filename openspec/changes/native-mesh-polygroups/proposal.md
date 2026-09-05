## Why

A ClayCore surface group names a region of the MODEL through one world-space
lattice, asked "which group is this surface point in" identically whatever the
surface is made of. That design is deliberate and it bought something real:
groups survive SDF <-> voxel <-> mesh conversion BY CONSTRUCTION, because they
were never in any of the three.

It also costs exactly one thing, and the header says so:

> "a group boundary is quantised to this lattice rather than to the
> representation, so a mesh that could have carried an exact per-face boundary
> does not. That is a visible edge at the group border."

That cost is fine for "isolate the head" and fatal for everything whose border
must BE an edge set: crease or bevel at a group border, split by group, polish
by group, per-group UV islands, and importing a model whose polygroups another
DCC already drew exactly.

`clay_mask_fill_from_group` (ABI 0.85.0) made a group a selection, so a group
now reaches every verb a mask gates. It did not and could not make the border
exact.

## What this change is

**A design proposal only. It writes no code.** The implementation guide
(`ClayCore_Regional_Multires_and_Native_Mesh_PolyGroups_Developer_Guide`, §9)
lists nine questions to answer "in the OpenSpec/design PR before locking the
ABI", and three of them change the shape of the code rather than its details.
This answers them against the tree, so the implementing change starts from
decisions rather than from a menu.

## The three that matter, decided

### D1 — the unit is the TRIANGLE, with a quad-consistency invariant

The guide leans toward "preserve artist-facing logical faces" and allows "a
pragmatic v1 may use triangle ids plus a quad-consistency invariant". The tree
settles it.

`Mesh::quads` is an OPTIONAL array carried only by a mesh from the quad mesher.
`mesh_data.h` makes it a rule that any operation rewriting `indices` must clear
it — and two of the operations Feature C exists to survive do exactly that:

    src/mesh/subdivide.cpp:399        out->quads.clear();
    src/mesh/dynamic_surface.cpp:738  out.quads.clear();

A group unit that evaporates on subdivision and on conversion to
`DynamicSurface` is not a unit. So membership is per TRIANGLE, and where
`quads` is non-empty the two triangles of a quad SHALL carry the same id.

This is not a compromise invented here — `voxel::drop_hidden` already filters
by quad for the same reason, and says so: "a triangle-wise filter would hand
back a quad export carrying no quads."

### D2 — a SIDECAR array, not a field on the face

The guide says to measure rather than decide by elegance. The measurement is
about the case that does not use the feature.

`DynamicFace` is `{HalfEdgeId halfedge; cfloat3 normal; uint32 flags;}`. A group
id as a fourth member costs every face on every surface, used or not. The
extreme-poly target is 20M faces (`add-extreme-poly-runtime`: "at 1M to 20M
vertices"), so:

| | 20M faces |
|---|---:|
| direct member, `uint32` | 80 MB, always |
| direct member, `uint16` | 40 MB, always |
| **sidecar, allocated on first use** | **0 MB unused** |

An empty-when-unused parallel array is also what the flat `Mesh` already does
for `normals`, `colors` and `uvs` — "empty or positions.size()". So the sidecar
is both the cheaper answer and the idiomatic one, and it makes §2.3's
pay-for-play rule structural rather than a thing to remember.

### D3 — storage implies no constraint; operations take a policy

Taken as the guide recommends, because it is the only split that keeps the core
topology primitives general: storage says which faces are in which group and
nothing about what may cross a border. Remeshing and sculpt operations take an
explicit Preserve / Ignore policy, and a host's sculpt default is Preserve.

## The rest, briefly

- **D4 metadata** — core owns the numeric identity, host owns name and colour.
  Revisit only if `.clayspace` portability demands it.
- **D5 deletion** — deleting a group reassigns its faces to the default group;
  it never deletes faces. `GroupField::reassign` already means exactly this
  ("Merging into CLAY_NO_GROUP deletes a group"), and two vocabularies for one
  word is how a host learns to distrust both.
- **D7 refinement requests** — resolve to stable base-patch lineage, never to a
  transient fine-level face id.
- **D8 coarsening** — out of v1, with the reason recorded rather than the
  question left open.
- **D9 fine-level-only boundaries** — refused in v1: a boundary that exists only
  at a level the artist may not be viewing cannot be shown or edited honestly.

## What this deliberately does NOT decide

**The id width.** The guide proposes `uint32`. `voxel::GroupId` is already
`uint16_t`, which caps at 65,535 groups, halves the sidecar and keeps one width
across both grouping systems. That is a real choice with a real memory
consequence at 20M faces (40 MB against 80 MB) and it should be made with a host
in the room, not here.

**Whether to build it at all.** Everything above is contingent on hard-surface
work or DCC round-trip being on the roadmap. If the need is "select a region and
sculpt it", ABI 0.85.0 already covers it and this whole change is cost without
a customer.

## Capabilities

### Modified Capabilities
- `meshing`: a mesh may carry an exact per-face grouping, distinct from the
  representation-independent spatial one.

## Impact

None yet — this change adds a decision record and a spec delta. The
implementing change would touch `include/clay/mesh/`, `src/mesh/`, the C ABI and
pyclay, and is explicitly out of scope here.
