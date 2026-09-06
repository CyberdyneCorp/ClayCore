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
incident to it**, with the ones the level does not store included. From it:

- `level_normals` gets a Newell normal per face, each computed at the level that
  face lives at, and sums them unweighted as it does now
- `class_normal` and `recompute_normals` get the triangle, the corner and the
  angle
- `is_boundary_class` gets a shared-face count that no longer reads a seam as a
  border
- `expand_by_face_ring` gets a halo that does not stop at the region rim
- the position kernels get a vertex ring by taking the corners

**CORRECTED BY WHAT LANDED: a missing face is not named by the level it lives
at.** This section first said each face comes back "named by the level it lives
at and its index there", which reads as though the coarse faces were handed over
as they are. They cannot be, and the reason is the one `cross_level.h` opens
with: a coarse neighbour has no vertex, no weld class and no face AT THIS LEVEL
to be named at all, so a reader handed one would be summing over two depths and
would not get the dense hierarchy's answer, which is the whole claim. What
`CrossLevelNeighborhood` holds instead is the faces the DENSE level would have
had here — quads, in the subdivision's own corner order — over a joined
numbering: an id below the level's vertex count is one of its own vertices, and
an id above it is a vertex the level does not store, carrying the pure
subdivision of the level below. That makes a boundary reader's answer the dense
hierarchy's answer rather than an approximation of it.

**Ordering is part of the answer, not a detail.** Two sites already record that a
neighbourhood's order is load-bearing because float addition is not associative:
the walk's frontier and pop order, and the sorted candidate list that keeps a
region stable across a BVH rebuild. With the faces derived rather than borrowed,
the order that falls out is **the order the dense level would have numbered them
in** — parent face order, then corner order — and a ring ascends in the joined
numbering. That is already deterministic: faces are patch-major in the parent's
order and `full_of` is ascending, so this needs no new rule, only a stated one.

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

### Quad-only where the arithmetic allows it, which is not everywhere

`Mesh::quads` requires `quads.size() / 4 * 6 == indices.size()` with quad `q` at
`indices[6q .. 6q+5]`, and `level_faces_into` already clears `quads` for any
non-uniform face list — "because a quad list that does not describe indices is
the lie `mesh_data.h` forbids". One 5-gon transition face therefore turns the
whole export into a triangle soup for a DCC.

**OVERRULED BY THE TREE, and by arithmetic rather than by judgement.** This
section asked for a quad-only template per incidence pattern — 1-sided (40
patches on a 2×2 region), 2-sided (up to 9), 3-sided (2), 4-sided (1, when a
refined ring leaves the hole coarser) and corner-only. There is no such template
for a split edge and there cannot be: a coarse quad with one split edge is a
PENTAGON, and a polygon with an odd number of boundary vertices has no
quadrangulation — four edges per quad counts every interior edge twice, so the
boundary count must be even however many vertices are added inside. Making it
even means splitting a second edge of that face, which its coarse neighbour must
then carry, and so on across the model.

So what the export does instead: `Mesh::quads` survives exactly while NO edge is
split — every uniform export, and every corner-only transition, which is the one
incidence pattern above that adds no boundary vertex — and is dropped whole
otherwise. `level_faces_into` already makes that choice from the face list, so no
new code decides it. Measured on the fixture: 48 patches emit more triangles than
twice their faces (a split edge), 12 emit exactly twice and still span two levels
(corner-only, all quads), 84 are untouched.

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

The C++ half of that landed and is gated: a refusal has a name
(`MultiresMixedStatus`), a refused export comes back empty rather than partial,
and the header says what the call does not promise. The descriptor rules in the
paragraph above are owed by the C entry point of task 6.1, which is not built —
so nothing in this ABI answers a mixed export yet, rather than answering it
badly.

### One thing a mixed export was expected to cost, and does not

`mesh_at_level` is already non-const and evaluates. The worry recorded here was
that a mixed-depth export forces levels `0 .. max` simultaneously resident, so
the peak-versus-persistent argument that produced `preflight_add_level` would
apply and an export preflight might be owed. **MEASURED AND ANSWERED: it is not.**
`mesh_at_level` already walks every level below its own — both calls open with
`evaluate_up_to(level)` — and the mixed export then reads the evaluated positions
and builds no level mesh, no adjacency and no chunk table, so its resident set is
a SUBSET rather than a superset. Gated as a byte comparison: `memory().rebuildable`
after a mixed export is no larger than after `mesh_at_level` on the same
hierarchy, and both are above the cold figure. No export preflight is added.

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

## A host question the export half must answer, whether or not it delivers it

Raised by ClaySpaceDesktop on 2026-09-06, after it checked its own code rather
than answering from memory. It is recorded here so the export work knows what it
must NOT become, and it is deliberately not allowed to steer the work.

**Their state today.** `export_mesh` takes their combined mesh, and a hierarchy
contributes its CAGE rather than the level it is drawn at. Their comment records
that as a deliberate trade, not an oversight: keeping a layer's triangles in step
with the display level would mean a wholesale geometry replacement — one engine
undo entry per gesture, or per save — and either would put a document EDIT inside
something a sculptor did not ask to be an edit. The route they point people at is
the crossing that bakes a level out to a mesh, which is one step and says what it
gives up.

**Their question.** If mixed-depth export makes it possible to read a sculpted
level's triangles WITHOUT a wholesale replacement — a read that does not become a
document edit — then the trade above stops being a trade and becomes a thing they
would take.

**What this change owes it, which is not the feature.** The export path must be a
READ. A path that produces a sculpted level's geometry and quietly costs an undo
entry, bumps a revision that invalidates a host's caches, or mutates a layer's
triangles is the same complaint arriving from this side of the wire. **State in
the header and the spec whether the export is a read, and if any part of it is
not, say which part and why.**

They asked explicitly that this not shape the brief and it does not: the export
half is built because the residual is real, and to be correct rather than fast,
since no host is waiting on it. If a non-mutating read of a level falls out of
the work anyway, that is worth telling them; nothing is to be bent to chase it.
