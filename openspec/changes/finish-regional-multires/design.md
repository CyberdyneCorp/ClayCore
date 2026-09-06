# Design

Three questions have to be answered before any code, because each one decides
what the other two are allowed to look like. Every answer below is decided, with
the measurement or the tree citation that decided it.

## 1. One `for_each_surface_neighbor`, or several readers over one helper?

**Decided: one topology helper, several readers. The helper answers in FACES, not
in vertices. The guide's single callback is refused.**

The guide proposes one traversal — conceptually
`for_each_surface_neighbor(vertex, callback)` — that hides whether a neighbour is
same-level, coarse-side, fine-side or transition-derived. The tree refuses it on
three separate grounds.

**Shape.** `smooth_targets` runs `laplacian_pass` up to `kMaxSmoothIterations`
times over the *same* materialized CSR, swapping two buffers. A callback
traversal re-walks the topology once per pass, per stamp. `build_neighbors`
already made this decision and wrote the reason down — the walk is done "once
instead of once per verb and once per smoothing pass". A traversal that is
materialized once and read many times is the tree's existing answer; a callback
is the one it moved away from.

**Payload.** Across the multires path a "neighbour" is nine different things over
three different topologies:

| reader | payload | topology |
|---|---|---|
| `laplacian_pass` | position at `P(n)` | weld classes |
| `form_shift` | position at `S(n)` | weld classes |
| `smooth_detail` | `LocalDetail` coefficients | weld classes |
| `polish_gate` | per-neighbour geometric normal, then the gate value twice | weld classes |
| smear | colour | weld classes |
| every kernel, both automask factors | workset slot | weld classes |
| `class_normal`, `recompute_normals` | triangle + corner index + corner angle | triangles |
| `level_normals` | incident face | `LevelTopology` faces |
| `is_boundary_class` | shared-triangle count per edge | triangles |

One callback signature carrying that union is strictly worse than what exists,
and the tree already priced the union once: `SculptNeighbors` fills `normals`
only for polish and `colors` only for smear, because "a geometric normal per
neighbour costs a pass over every neighbour's own ring, and no other verb would
use it".

**Normals are two evaluators, not one.** `class_normal` is angle-weighted over
triangles; `level_normals` is an unweighted Newell sum over `LevelTopology`
faces. The first shades the brush, the second shades the display and builds the
frames. A single traversal that served both would have to change one of their
results, and the header for the first explains at length why area weighting was
rejected. 3.4's "normals" is two paths and this change budgets for two.

### Why the helper is a FACE ring

Every reader that is *wrong* at a transition is a face or corner walk — display
normals, transported frames, the propagation halo, the brush's angle-weighted
normal, the write-back normal pass, the boundary automask's shared-triangle
count. Only the position-averaging kernels want a vertex ring, and a vertex ring
falls out of the corners of a face ring.

The measurement says the same thing. The predicate "this vertex's display normal
differs from the dense hierarchy's" and the predicate "this vertex's face ring is
smaller than the dense hierarchy's" were checked against each other and
**disagreed on 0 vertices at every level**. The defect *is* the incomplete face
ring. A helper that answers in vertices would be describing the symptom.

So the helper answers, for a vertex at a level: **the complete set of faces
incident to it, each named by the level it lives at and its index there**, with
the coarse-side faces included. From it:

- `level_normals` gets a Newell normal per face, each computed at the level that
  face lives at, and sums them unweighted as it does now
- `class_normal` and `recompute_normals` get the triangle, the corner and the
  angle
- `is_boundary_class` gets a shared-face count that no longer reads a seam as a
  border
- `expand_by_face_ring` gets a halo that does not stop at the region rim
- the position kernels get a vertex ring by taking the corners

**Ordering is part of the answer, not a detail.** Two sites already record that a
neighbourhood's order is load-bearing because float addition is not associative:
the walk's frontier and pop order, and the sorted candidate list that keeps a
region stable across a BVH rebuild. The helper's order is therefore fixed:
**ascending level, then the level's own face order**. That is already
deterministic — faces are patch-major in the parent's order and `full_of` is
ascending — so this needs no new rule, only a stated one.

**Where it lives.** On the surface, beside the existing per-level structures, and
cached in `LevelCache` like everything else derived. It is not plumbed into
`MeshSculptor` as a back-pointer to `MultiresSurface`: `MultiresSculptor::bind`
builds a level sculptor over one level's `Mesh` and `Adjacency` and that
separation is worth keeping. The brush side reaches it the way it reaches
everything else about the level — through what `bind` hands over.

**Staleness.** A `(level, vertex)` pair handed outside a stamp is a new stale
index trap: `bind` renumbers every weld class on a level change *and* on a
cache-generation change, and a regional level's numbering is compacted to what it
stores. Any such pair carries the same revision discipline the existing seed
token has, or it becomes a second silent-empty-region bug.

## 2. Is the transition topology stored or derived?

**Decided: derived, cached, never serialized. `kSurfaceVersion` does not move.**

The transition topology is a pure function of the cage, the subdivision rule and
the per-level patch sets — and all three are already in a version-3 stream.
`encode()` writes, per level above 0, either the dense marker or a count plus an
ascending patch list, and *nothing else about depth*; `decode()` replays the build
through `add_level` / `add_level_for_patches` and rebuilds the face lists,
`full_of`, the edge counts and the chunk tables. The file header states the rule
this change inherits: "the per-level FACE LISTS are not written: they follow from
the cage and the rule."

Three consequences, all of them free:

- **No bytes, no version 4, no format-minor bump.** A bump would make every older
  build refuse a document for a purely derived feature, which is the opposite of
  what the version-2 bump was reasoned for.
- **The spec line stays literally true.** "Transition geometry is derived display
  data and SHALL NOT become the authoritative sculpt representation" is a
  structural guarantee rather than a discipline someone has to keep.
- **It is cacheable with no new policy.** It belongs in `LevelCache`, which means
  `drop_intermediate_caches` and `drop_all_caches` release it and it must rebuild
  bit-identically. `cache_generation` must move on **release** as well as on
  create — this file already records what happened the one time only the create
  side bumped: a stamp written into freed storage that silently did nothing.

## 3. What must a transition guarantee?

**Watertight by construction, deterministic, stable under re-refinement,
quad-only, and never silent.**

### Watertight by IDENTITY, not by distance

A vertex shared by a coarse face and a finer patch is the **same index** in the
emitted mesh, and it takes the **fine side's value** — the one that is a
subdivision step closer to the limit. It is never welded by proximity. The tree
forecloses tolerance welding twice over: `level_adjacency` welds at exactly
`0.0f` because "a level's vertices are already the geometric points of the
surface", and the mismatch is not a hairline — the same cage vertex is
**0.043439553** apart across a **0.5** cage edge, 8.7% of an edge. Any epsilon
large enough to close that would weld unrelated geometry.

### The ripple rule, which is the part task 2.3 undercounts

Adopting the fine value at a shared cage vertex moves the coarse face's corner,
so **every** coarse face incident to that vertex must adopt it — including a
**corner-only** patch that shares no edge with the fine region at all and
therefore has no T-junction to fix. Measured: 4 of 44 transition patches on a 2×2
region, 19 on three scattered patches. Stopping at edge-adjacent faces does not
remove the crack, it relocates it one face over onto a coarse–coarse edge, where
it will be reported as a different bug.

So the emission set is **"every coarse face incident to a cage vertex shared with
a finer patch"**, not "beside a finer patch". The T-junction — the fine edge
point sitting **0.023213101** off the coarse chord, half the corner gap — is the
second half of the same fix, not the whole of it.

### Quad-only, or the export stops being a subdivision cage

`Mesh::quads` requires `quads.size() / 4 * 6 == indices.size()` with quad `q` at
`indices[6q .. 6q+5]`, and `level_faces_into` already clears `quads` for any
non-uniform face list — "because a quad list that does not describe indices is
the lie `mesh_data.h` forbids". One 5-gon transition face therefore turns the
whole export into a triangle soup for a DCC. Every configuration that occurs —
1-sided (40 patches on a 2×2 region), 2-sided (up to 9), 3-sided (2), 4-sided (1,
when a refined ring leaves the hole coarser) and corner-only — gets a **quad-only
template**.

There is **one bridging case to template**, not a family. `resolve_keep` refuses
to refine a patch unless it and its whole vertex ring are resident one level
down, which is stricter than 2:1: the measured maximum depth spread across a
shared cage vertex is **1**, on five graded hierarchies and again on a
deliberately awkward direct sequence where 2 of 4 requests were refused. A
transition never bridges more than one level, so the configuration table the
guide asks for is a table of one row per incidence pattern and no chain.

### Deterministic, and stable under re-refinement

Determinism needs no new rule: the transition set is derived from `full_of`
(ascending), the patch-major face order, and ring growth in ascending patch id,
and "the same request twice is the same hierarchy" is already gated by a
byte-equal `encode()` with the patch list reversed.

Stability is a consequence of refinement being **monotonic** in v1 — there is no
removal — so refining more patches can only **shrink** the transition set. The
guarantee to state and to gate: a coarse face whose vertex ring's residency did
not change emits the same faces, byte for byte, before and after an unrelated
region is refined.

### Never silent

`build_block` returns **true with 0 vertices and 0 indices** for a non-resident
patch, so a host that forgets `effective_level` draws a hole with no error. A
mixed-depth export must not repeat that shape at a larger blast radius: a request
it cannot answer completely says so. Per the C ABI rules the new entry point
takes a descriptor beginning with `uint32_t struct_size`, grown by appending, a
new field's zero meaning today's behaviour, caller-owned buffers, and
`BUFFER_TOO_SMALL` — never `INVALID_ARGUMENT` — for a short buffer, with the
header documenting what the call does **not** promise.

### One thing a mixed export costs that a single-level one does not

`mesh_at_level` is already non-const and evaluates. A mixed-depth export forces
levels `0 .. max` simultaneously resident, so the peak-versus-persistent argument
that produced `preflight_add_level` applies here, and there is no preflight for an
export today. Whether one is added is a decision left to the export stage, taken
against a measured peak rather than assumed.

## What this change does NOT decide

- **Per-face grouping inheritance.** `native-mesh-polygroups` is a design proposal
  that writes no code, and its chosen unit is the triangle with a quad-consistency
  invariant on a sidecar array. Designing inheritance now would be designing
  against a proposal. The only commitment made here: a transition face inherits
  the `face_patch` of the coarse face it replaces, which is derivable and costs no
  bytes.
- **Level removal.** Still monotonic. Removing a refined region needs a policy for
  the detail authored there, and the predecessor already recorded why choosing one
  silently is worse than not offering the call.
- **Caching beyond `LevelCache`.** The tree caches at three levels with the
  justification written into the code, including a measured regression from moving
  one scan. Nothing here removes a cache.
